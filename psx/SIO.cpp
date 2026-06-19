#include "SIO.h"

#include "Scheduler.h"
#include "INTC.h"
#include "Timing.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/MathsHelpers.h" // Max
#include "core/ArrayHelpers.h" // COUNTOF_ARRAY
#include "core/Helpers.h" // HP_UNUSED

// #TODO: Might want to use BAUD * 8 (see Emudev)
static const unsigned int kIRQ7Delay = 1088; // Value from EmuDev

static bool s_logJOY_STAT = true; // very noisy

SIO::SIO(INTC& intc, Scheduler& scheduler)
	: m_ports{ ControllerPort(0), ControllerPort(1) }
	, m_intc(intc)
	, m_scheduler(scheduler)
{
	m_stat.TXIdle = 1;
	m_stat.TXFIFOHasSpace = 1;
}

void SIO::Reset()
{
	// #TODO: What are the initial values for these registers?
	m_stat.val = 0;
	m_stat.TXIdle = 1;
	m_stat.TXFIFOHasSpace = 1;

	m_mode.val = 0;
	m_baudrateReloadMultiplier = 1;

	m_ctrl.val = 0;

	m_baud = 0;
	m_baudTimerEventID = 0;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_rxFIFO); i++)
		m_rxFIFO[i] = 0;
	
	m_rxWriteIndex = 0;
	m_rxReadIndex = 0;
	m_rxFifoCount = 0;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_ports); i++)
	{
		m_ports[i].Reset();
	}
}

/*
Peripheral I/O Ports

  1F801040h 1/4  JOY_DATA Joypad/Memory Card Data (R/W)
  1F801044h 4    JOY_STAT Joypad/Memory Card Status (R)
  1F801048h 2    JOY_MODE Joypad/Memory Card Mode (R/W)
  1F80104Ah 2    JOY_CTRL Joypad/Memory Card Control (R/W)
  1F80104Eh 2    JOY_BAUD Joypad/Memory Card Baudrate (R/W)
  1F801050h 1/4  SIO_DATA Serial Port Data (R/W)
  1F801054h 4    SIO_STAT Serial Port Status (R)
  1F801058h 2    SIO_MODE Serial Port Mode (R/W)
  1F80105Ah 2    SIO_CTRL Serial Port Control (R/W)
  1F80105Ch 2    SIO_MISC Serial Port Internal Register (R/W)
  1F80105Eh 2    SIO_BAUD Serial Port Baudrate (R/W)

  https://psx-spx.consoledev.net/iomap/#peripheral-io-ports
*/

u8 SIO::Read8(unsigned int address)
{
	switch (address)
	{
		case 0:
		{
			u8 val = readRxFIFO();
			if (g_logSIO)
				LOG_INFO("[SIO] JOY_DATA read RX FIFO value %02X\n", val);
			return val;
		}
		default:
		{
			if (g_logSIO)
				LOG_INFO("[SIO] 8-bit read from address %08X NOT IMPLEMENTED\n", address);
			return 0xff;
		}
	}
}

u16 SIO::Read16(unsigned int address) const
{
	HP_ASSERT((address & 1) == 0, "Not 16-bit aligned");

	switch (address >> 1)
	{
		case 2: // JOY_STAT Joypad/Memory Card Status (R)
		{
			u16 val = m_stat.val & 0xffff;
			if (g_logSIO && s_logJOY_STAT)
				LOG_INFO("[SIO] JOY_STAT read %04X. DSRInputLevel (bit 7) = %u\n", val, m_stat.DSRInputLevel);
			return val;
		}
		case 5: // 1F80104Ah JOY_CTRL Joypad/Memory Card Control (R/W)
		{
			if (g_logSIO)
				LOG_INFO("[SIO] JOY_CTRL read %04X\n", m_ctrl.val);
			return m_ctrl.val;
		}
		case 7: // 1F80104Eh JOY_BAUD Joypad/Memory Card Baudrate (R/W)
		{
			if (g_logSIO)
				LOG_INFO("[SIO] JOY_BAUD read %04X\n", m_baud);
			return m_baud;
		}
		default:
		{
			if (g_logSIO)
				LOG_INFO("[SIO] 8-bit read from address %08X NOT IMPLEMENTED\n", address);
			return 0xffff;
		}
	}
}

u32 SIO::Read32(unsigned int address) const
{
	switch (address >> 2)
	{
		case 1: // 1f801044 JOY_STAT Joypad/Memory Card Status (R)
		{
			u32 val = m_stat.val;
			if (g_logSIO && s_logJOY_STAT)
				LOG_INFO("[SIO] JOY_STAT read %08X. DSRInputLevel (bit 7) = %u\n", val, m_stat.DSRInputLevel);
			return val;
		}
		default:
		{
			if (g_logSIO)
				LOG_INFO("[SIO] 32-bit read from address %08X NOT IMPLEMENTED\n", address);
			return 0xffff;
		}
	}
}

void SIO::Write8(unsigned int address, u8 val)
{
	switch (address)
	{
		case 0:
		{
			if (g_logSIO)
				LOG_INFO("[SIO] JOY_DATA write %02X (selected port: %u)\n", val, m_ctrl.SIO0PortSelect);

			// DTR Out is connected to controller port's chip select, so only communicate with them if this bit is set.
			if (m_ctrl.DTROutputLevel)
			{
				// Write to selected port
				// On SIO0, DSR is wired to the /ACK pin on the controller and memory card ports.
				// STAT bit 7 is thus set when /ACK is low (asserted) and cleared when it is high.
				u8 response;
				bool ack = m_ports[m_ctrl.SIO0PortSelect].Write8(val, response);

				// DSR is set high if ack asserted
				m_stat.DSRInputLevel = ack;

				// - When RXEnable is not set, incoming data will be ignored unless DTROutputLevel bit 1 is also set.
				// - When RXEnable is set, data will be received regardless of whether /CS (connected to DTROutputLevel) is asserted,
				//   however RXEnable bit 2 will be automatically cleared after a byte is received.
				if (m_ctrl.RXEnable == 0)
				{
					if (m_ctrl.DTROutputLevel)
						writeRxFIFO(response);
				}
				else
				{
					writeRxFIFO(response);
					m_ctrl.RXEnable = false; // #TODO: Should this clear be delayed?
				}

				// Trigger interrupt if enabled and conditions met?
				if (ack && m_ctrl.DSRInterruptEnable)
				{
					// Emulation Note:
					// After sending a byte, the Kernel waits 100 cycles or so, and does THEN acknowledge any old IRQ7, 
					// and does then wait for the new IRQ7. Due to that bizarre coding, emulators can't trigger IRQ7 
					// immediately within 0 cycles after sending the byte.
					// https://psx-spx.consoledev.net/controllersandmemorycards/#emulation-note

					// n.b. Delay setting STAT.InterruptRequest bit 9 too
					// 
					// Implementation note. No need to store the event ID. There can be multiple in flight at a time and this is fine. e.g. PS1MiniPadTestV0.4.exe
					// If the machine is reset then the scheduler will be reset and any event cleared.
					m_scheduler.Schedule(SIO::irq7Callback, this, kIRQ7Delay, "SIO0 IRQ7 Controller/Memory Card Byte Received");
				}

				if (ack)
				{
					// IMPORTANT: Clear DSR after a short delay to simulate ACK pulse
					// https://psx-spx.consoledev.net/controllersandmemorycards/#controller-and-memory-card-signals
					// The pulse is required to fix input in Spyro, Street Fighter Alpha 3 and Silent Hill.
					// This should happen BEFORE the IRQ7 fires.

					// The ACK has been measured to be 2.84 us. See https://discord.com/channels/465585922579103744/466353899670863873/703613987346186280
					static constexpr double kControllerAckPulseWidthSeconds = 2.84e-6;

					// Convert to cycles
					// Cycles = (cycles/second) * seconds: N = f * t
					// 33.8688 * 10^6 * 2.84 * 10^6 = 33.8686 * 2.84 = 96.187392
					static constexpr unsigned int kControllerAckPulsePeriodCycles = (unsigned int)((double)kCpuClock * kControllerAckPulseWidthSeconds); // 96

					m_scheduler.Schedule([](void* userdata) {
						SIO* pSIO = (SIO*)userdata;
						pSIO->m_stat.DSRInputLevel = 0;
						}, this, kControllerAckPulsePeriodCycles, "SIO0 Clear DSR/ACK");
				}
			}
			else
				m_stat.DSRInputLevel = false; // #TODO: Is this correct?

			break;
		}
		default:
		{
			if (g_logSIO)
				LOG_INFO("[SIO] 8-bit write to address %08X value %02X NOT IMPLEMENTED\n", address, val);
			break;
		}
	}
}

void SIO::Write16(unsigned int address, u16 val)
{
	HP_ASSERT((address & 1) == 0, "Not 16-bit aligned");

	switch (address >> 1)
	{
		case 4: // 1F801048h JOY_MODE Joypad/Memory Card Mode (R/W)
		{
			if (g_logSIO)
				LOG_INFO("[SIO] JOY_MODE write %04X\n", val);

			m_mode.val = val;
			m_mode.unusedBits15_9 = 0; // Ensure unused bits are always zero

			// Cache the baudrate reload multiplier for use when writing to JOY_BAUD
			m_baudrateReloadMultiplier = m_mode.BaudrateReloadFactor == 2 ? 16 : (m_mode.BaudrateReloadFactor == 3 ? 64 : 1);

			break;
		}
		case 5: // 1F80104Ah JOY_CTRL Joypad/Memory Card Control (R/W)
		{
			writeCTRL(val);
			break;
		}
		case 7: // 1F80104Eh JOY_BAUD Joypad/Memory Card Baudrate (R/W)
		{
			// https://psx-spx.consoledev.net/serialinterfacessio/#1f80104ehn10h-sio_baud-rw-eg-00dch-9600-bps-when-factormul16

			m_baud = val;

			// Upon reload, the 16-bit Reload value is multiplied by the Baudrate Factor (see SIO_MODE.Bit0-1),
			// divided by 2, and then copied to the 21-bit Baudrate Timer (SIO_MODE.Bit11-31).
			m_stat.BaudrateTimer = (m_baudrateReloadMultiplier * val) >> 1;

			if (g_logSIO)
			{
				// SIO0: BitsPerSecond = 33868800 / MAX(((Reload*Factor) AND NOT 1),1)
				// #TODO: Should this use kCpuClock	33868500 instead?
				unsigned int bps = 33868800 / Max((m_baud * m_baudrateReloadMultiplier) & ~1u, 1u);
				LOG_INFO("[SIO] JOY_BAUD write %04X = %u bps = %x cycles\n", val, bps, m_stat.BaudrateTimer);
			}

			// The standard baud rate for SIO0 devices, including both controllers and memory cards, is ~250 kHz,
			// with SIO0_BAUD being set to 0088h resulting in serial clock high for 44h cycles then low for 44h cycles.

			// Cancel any existing event
			if (m_baudTimerEventID != 0)
			{
				m_scheduler.Cancel(m_baudTimerEventID);
				m_baudTimerEventID = 0;
			}

			// #TODO: This is scheduling for 44h cycles. Is this correct? Does shift occur on both rising and falling edges?
			// Removed this for now because it is not required.
//			m_baudTimerEventID = m_scheduler.Schedule(SIO::baudTimerExpiredCallback, this, m_stat.BaudrateTimer, "SIO0 Baudrate Timer");
			break;
		}
		default:
			if (g_logSIO)
				LOG_INFO("[SIO] 16-bit write to address %08X value %04X NOT IMPLEMENTED\n", address, val);
			break;
	}
}

void SIO::writeCTRL(u16 val)
{
	m_ctrl.val = val;
	if (g_logSIO)
		LOG_INFO("[SIO] JOY_CTRL write %04X DTROutputLevel=%u SIO0PortSelect=%u\n", val, m_ctrl.DTROutputLevel, m_ctrl.SIO0PortSelect);

	if (m_ctrl.Reset) // "Reset most registers to zero"
	{
		if (g_logSIO)
			LOG_INFO("[SIO] Reset\n");

		// #TODO: Which registers should JOY_CTRL Reset bit 6 reset, and to what values?
		// Implementation note: Don't call Reset(), because will discard scheduled IRQ7 event.
		m_stat.val = 0;
		m_stat.TXIdle = 1;
		m_stat.TXFIFOHasSpace = 1;

		m_mode.val = 0;
		m_baudrateReloadMultiplier = 1; // #TODO: Should this be reset?

		m_ctrl.val = 0;

		for (unsigned int i = 0; i < COUNTOF_ARRAY(m_rxFIFO); i++)
			m_rxFIFO[i] = 0;

		m_rxWriteIndex = 0;
		m_rxReadIndex = 0;
		m_rxFifoCount = 0;

		// #TODO: Should any port state be reset by SIO JOY_CTRL write with reset bit set?
		
#if 0 // Disabled because don't want to reset port->controller from config mode back to normal mode.
		for (unsigned int i = 0; i < COUNTOF_ARRAY(m_ports); i++)
		{
			// #TODO: Be careful that this API is safe to call here i.e. not hard reset if may cause issues.
			m_ports[i].Reset();
		}
#endif

		// The Reset bit is marked as write only, so clear it after use.
		m_ctrl.Reset = 0;
	}

	if (m_ctrl.Acknowledge)
	{
		// Acknowledge bit 4 resets SIO_STAT bits 3,4,5,9
		m_stat.RXParityError = 0;
		m_stat.SIO1RXFIFOOverrun = 0;
		m_stat.SIO1RXBadStopBit = 0;

		if (m_stat.InterruptRequest)
		{
			m_stat.InterruptRequest = 0;

			if (g_logSIO)
				LOG_INFO("[SIO] Interrupt acknowledged\n");
		}

		// #TODO: Trigger another interrupt request if conditions are met:
		// When acknowledging via SIO_CTRL.4 with the enabled condition(s) in SIO_CTRL.10-12 still being true e.g. the RX FIFO is still not empty
		// the IRQ does trigger again (almost) immediately. It goes off only for a very short moment; barely enough to allow I_STAT.8 to sense a edge.

		// The Acknowledge bit is marked as write only, so clear it after use.
		m_ctrl.Acknowledge = 0;
	}

	// On SIO0, DTR bit 1 is wired to the /CS pin on the controller and memory card ports; bit 1 will pull (assert) /CS low when set.
	// SIO0 PortSelect bit 13 is used to select which port's /CS shall be asserted (all other signals are wired in parallel).
#if 1
	m_ports[0].SetSelected(m_ctrl.DTROutputLevel && (m_ctrl.SIO0PortSelect == 0));
	m_ports[1].SetSelected(m_ctrl.DTROutputLevel && (m_ctrl.SIO0PortSelect == 1));
#else
	m_ports[m_ctrl.SIO0PortSelect].SetSelected(m_ctrl.DTROutputLevel);
#endif
	if (g_logSIO)
		LOG_INFO("[SIO] RXEnable %s\n", m_ctrl.RXEnable ? "set" : "reset");

	// n.b. Some emulators do not implement all SIO0 interrupts, as the kernel's controller driver only ever uses the DSR (/ACK) interrupt.
}

void SIO::baudTimerExpiredCallback(void* userdata)
{
	SIO* pSIO = (SIO*)userdata;
	pSIO->baudTimerExpired();
}

void SIO::baudTimerExpired()
{
	if (g_logSIO && 0) // disabled - too verbose
		LOG_INFO("[SIO] Baudrate timer expired. Rescheduling\n");

	// #TODO: Handle baud rate timer expiry e.g. shift bits, handle TX/RX, set STAT bits, trigger interrupts etc.

	// #TODO: This is scheduling for 44h cycles. Is this correct? Does shift occur on both rising and falling edges?
	m_baudTimerEventID = m_scheduler.Schedule(SIO::baudTimerExpiredCallback, this, m_stat.BaudrateTimer, "SIO0 Baudrate Timer");
}

void SIO::irq7Callback(void* userdata)
{
	SIO* pSIO = (SIO*)userdata;
	pSIO->irq7();
}

void SIO::irq7()
{
	m_stat.InterruptRequest = 1;
	m_intc.SetIRQ(IRQ::IRQ7_ControllerAndMemoryCardByteReceived);
}

void SIO::writeRxFIFO(u8 val)
{
	// Responses just keep getting written to the circular FIFO, regardless of what has been read.

	HP_DEBUG_ASSERT(m_rxWriteIndex < COUNTOF_ARRAY(m_rxFIFO));
	m_rxFIFO[m_rxWriteIndex] = val;

	static_assert(COUNTOF_ARRAY(m_rxFIFO) == 8);
	m_rxWriteIndex = (m_rxWriteIndex + 1) & 0x7;

	// #TODO; If the FIFO is full, should the read pointer be modifed i.e. pushed?
	m_rxFifoCount = Min(m_rxFifoCount + 1u, (unsigned int)COUNTOF_ARRAY(m_rxFIFO));

	m_stat.RXFIFOHasData = 1;

	if (g_logSIO)
		LOG_INFO("[SIO] RX FIFO write %02X (size now %u)\n", val, m_rxFifoCount);
}

u8 SIO::readRxFIFO()
{
	if (m_rxFifoCount > 0)
	{
		u8 val = m_rxFIFO[m_rxReadIndex];
		m_rxReadIndex = (m_rxReadIndex + 1) % COUNTOF_ARRAY(m_rxFIFO);
		m_rxFifoCount--;

		// Update RX FIFO Not Empty status bit
		m_stat.RXFIFOHasData = m_rxFifoCount > 0 ? 1 : 0;

		if (g_logSIO)
			LOG_INFO("[SIO] RX FIFO read %02X (size now %u)\n", val, m_rxFifoCount);

		return val;
	}
	else
	{
		if (g_logSIO)
			LOG_INFO("[SIO] RX FIFO read when empty. Returning 0xFF.\n");
		return 0xff;
	}
}

ControllerPort& SIO::GetPort(unsigned int index)
{
	HP_ASSERT(index < COUNTOF_ARRAY(m_ports));
	return m_ports[index];
}

