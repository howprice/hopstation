// PSX Serial Interfaces (SIO)
//
// SIO0: Controller and memory card ports. Synchronous mode.
// SIO1: Serial port. Asynchronous mode.
//
// References:
// - https://psx-spx.consoledev.net/serialinterfacessio/
// - https://psx-spx.consoledev.net/controllersandmemorycards/

#pragma once

#include "core/Types.h"

#include "ControllerPort.h"

inline bool g_logSIO = false;

class INTC;
class Scheduler;

class SIO
{
public:

	static const unsigned int kNumPorts = 2;

	SIO(INTC& intc, Scheduler& scheduler);

	void Reset();

	u8 Read8(unsigned int address);
	u16 Read16(unsigned int address) const;
	u32 Read32(unsigned int address) const; // Required by FF7 with memory card inserted

	void Write8(unsigned int address, u8 val);
	void Write16(unsigned int address, u16 val);

	ControllerPort& GetPort(unsigned int index);

private:

	void writeCTRL(u16 val);

	static void baudTimerExpiredCallback(void* userdata);
	void baudTimerExpired();

	static void irq7Callback(void* userdata);
	void irq7();

	void writeRxFIFO(u8 val);
	u8 readRxFIFO();

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

	/*
	  1F801044h+N*10h - SIO#_STAT (R)

	  0     TX FIFO Not Full       (1=Ready for new byte)  (depends on CTS) (TX requires CTS)
	  1     RX FIFO Not Empty      (0=Empty, 1=Data available)
	  2     TX Idle                (1=Idle/Finished)       (depends on TXEN and on CTS)
	  3     RX Parity Error        (0=No, 1=Error; Wrong Parity, when enabled) (sticky)
	  4     SIO1 RX FIFO Overrun   (0=No, 1=Error; received more than 8 bytes) (sticky)
	  5     SIO1 RX Bad Stop Bit   (0=No, 1=Error; Bad Stop Bit) (when RXEN)   (sticky)
	  6     SIO1 RX Input Level    (0=Normal, 1=Inverted) ;only AFTER receiving Stop Bit
	  7     DSR Input Level        (0=Off, 1=On) (remote DTR) ;DSR not required to be on
	  8     SIO1 CTS Input Level   (0=Off, 1=On) (remote RTS) ;CTS required for TX
	  9     Interrupt Request      (0=None, 1=IRQ) (See SIO_CTRL.Bit4,10-12)   (sticky)
	  10    Unknown                (always zero)
	  11-31 Baudrate Timer         (15-21 bit timer, decrementing at 33MHz)

	  Bit 0 gets set after sending the start bit
	  Bit 2 is set after sending all bits including the stop bit if any.
	  On SIO0, DSR is wired to the /ACK pin on the controller and memory card ports;
	  bit 7 is thus set when /ACK is low (asserted) and cleared when it is high.
	  Bits 4-6 and 8 are always zero.

	  The number of bits actually used by the baud rate timer is probably affected by the reload factor set in SIO_MODE.

	  https://psx-spx.consoledev.net/serialinterfacessio/#1f801044hn10h-sio_stat-r
	*/
	struct STAT
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 TXFIFOHasSpace : 1;       // 0
				u32 RXFIFOHasData : 1;        // 1
				u32 TXIdle : 1;               // 2
				u32 RXParityError : 1;        // 3
				u32 SIO1RXFIFOOverrun : 1;    // 4
				u32 SIO1RXBadStopBit : 1;     // 5
				u32 SIO1RXInputLevel : 1;     // 6
				u32 DSRInputLevel : 1;        // Bit 7
				u32 SIO1CTSInputLevel : 1;    // 8
				u32 InterruptRequest : 1;     // 9
				u32 UnknownBit10 : 1;         // 10
				u32 BaudrateTimer : 21;       // 31:11  21-bit Baudrate Timer
			};
		};
	};

	/*
	  1F801048h+N*10h - SIO#_MODE (R/W)

	  0-1   Baudrate Reload Factor     (1=MUL1, 2=MUL16, 3=MUL64) (or 0=MUL1 on SIO0, STOP on SIO1)
	  2-3   Character Length           (0=5 bits, 1=6 bits, 2=7 bits, 3=8 bits)
	  4     Parity Enable              (0=No, 1=Enable)
	  5     Parity Type                (0=Even, 1=Odd) (seems to be vice-versa...?)
	  6-7   SIO1 stop bit length       (0=Reserved/1bit, 1=1bit, 2=1.5bits, 3=2bits)
	  8     SIO0 clock polarity (CPOL) (0=High when idle, 1=Low when idle)
	  9-15  Not used (always zero)

	  Bits 6-7 on SIO0 and bit 8 on SIO1 are always zero.

	  On SIO0 the character length shall be set to 8, the clock polarity should be set to high-when-idle
	  and parity should be disabled, as all controllers and memory cards expect these settings.
	*/
	struct MODE
	{
		union {
			u16 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u16 BaudrateReloadFactor : 2; // 1:0
				u16 CharacterLength : 2;      // 3:2
				u16 ParityEnable : 1;         // 4
				u16 ParityType : 1;           // 5
				u16 SIO1StopBitLength : 2;    // 7:6
				u16 SIO0ClockPolarity : 1;    // 8
				u16 unusedBits15_9 : 7;       // 15:9
			};
		};
	};

	/*
	  1F80104Ah+N*10h - SIO#_CTRL (R/W)

	  0     TX Enable (TXEN)      (0=Disable, 1=Enable)
	  1     DTR Output Level      (0=Off, 1=On)  Wired to the /CS pin on the controller and memory card ports. When DTR on it will pull /CS low (make active).
	  2     RX Enable (RXEN)      SIO0: 0=only receive when /CS low (asserted), 1=force receiving single byte
	                              SIO1: 0=Disable, 1=Enable  ;Disable also clears RXFIFO
	  3     SIO1 TX Output Level  (0=Normal, 1=Inverted, during Inactivity & Stop bits)
	  4     Acknowledge           (0=No change, 1=Reset SIO_STAT.Bits 3,4,5,9)      (W)
	  5     SIO1 RTS Output Level (0=Off, 1=On)
	  6     Reset                 (0=No change, 1=Reset most registers to zero) (W)
	  7     SIO1 unknown?         (read/write-able when FACTOR non-zero) (otherwise always zero)
	  8-9   RX Interrupt Mode     (0..3 = IRQ when RX FIFO contains 1,2,4,8 bytes)
	  10    TX Interrupt Enable   (0=Disable, 1=Enable) ;when SIO_STAT.0-or-2 ;Ready
	  11    RX Interrupt Enable   (0=Disable, 1=Enable) ;when N bytes in RX FIFO
	  12    DSR Interrupt Enable  (0=Disable, 1=Enable) ;when SIO_STAT.7  ;DSR high or /ACK low
	  13    SIO0 port select      (0=port 1, 1=port 2) (/CS pulled low when bit 1 set)
	  14-15 Not used              (always zero)

	  https://psx-spx.consoledev.net/serialinterfacessio/#1f80104ahn10h-sio_ctrl-rw
	*/
	struct CTRL
	{
		union {
			u16 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u16 TXEnable : 1;             // 0
				u16 DTROutputLevel : 1;       // 1  Wired to the /CS pin on the controller and memory card ports. When DTR on it will pull /CS low (make active).
				u16 RXEnable : 1;             // 2
				u16 SIO1TXOutputLevel : 1;    // 3
				u16 Acknowledge : 1;          // 4
				u16 SIO1RTSOutputLevel : 1;   // 5
				u16 Reset : 1;                // 6
				u16 SIO1UnknownBit7 : 1;      // 7
				u16 RXInterruptMode : 2;      // 9:8
				u16 TXInterruptEnable : 1;    // 10
				u16 RXInterruptEnable : 1;    // 11
				u16 DSRInterruptEnable : 1;   // 12
				u16 SIO0PortSelect : 1;       // 13
				u16 unusedBits15_14 : 2;      // 15:14
			};
		};
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	// SIO0 Joypad/Memory Card registers
	// #TODO: Rename to m_joy_mode etc if ever implement SIO1
	STAT m_stat{};

	MODE m_mode{};
	unsigned int m_baudrateReloadMultiplier = 1; // derived from m_mode.BaudrateReloadFactor on write to avoid repeated calculations

	CTRL m_ctrl{};

	u16 m_baud{};
	u32 m_baudTimerEventID{};

	// RX FIFO
	// The hardware can hold 8 bytes in the RX direction.
	// When receiving further byte(s) while the RX FIFO is full, then the last FIFO entry will by overwritten by
	// the new byte and SIO_STAT.4 gets set.
	// The hardware does NOT automatically disable RTS when the FIFO becomes full. #TODO: What does this mean?
	// The RX FIFO overrun flag is not accessible on SIO0.
	// https://psx-spx.consoledev.net/serialinterfacessio/#sio_rx_data-notes
	u8 m_rxFIFO[8]{};
	unsigned int m_rxWriteIndex = 0;
	unsigned int m_rxReadIndex = 0;
	unsigned int m_rxFifoCount = 0;

	ControllerPort m_ports[kNumPorts];

	INTC& m_intc; // interrupt controller for raising SIO IRQ 8
	Scheduler& m_scheduler;
};
