#include "Timers.h"

#include "INTC.h"

#include "core/Log.h"
#include "core/Helpers.h"
#include "core/hp_assert.h"

static const char* kTimerRegNames[] =
{
	"Value",
	"Mode",
	"Target",
	"Reserved",
};

//
// 4-bit source code varies per timer.
//
static const char* kTimerSourceNames[3][4] =
{
	{"System Clock", "Dotclock", "System Clock", "Dotclock"},
	{"System Clock", "Hblank", "System Clock", "Hblank"},
	{"System Clock", "System Clock", "System Clock/8", "System Clock/8"},
};

//
// 4-bit sync mode varies per timer.
//
static const char* kTimerSyncModeNames[3][4] =
{
	// Timer 0
	{
		"Pause counter during Hblank(s)",
		"Reset counter to 0000h at Hblank(s)",
		"Reset counter to 0000h at Hblank(s) and pause outside of Hblank",
		"Pause until Hblank occurs once, then switch to Free Run"
	},
	// Timer 1
	{
		"Pause counter during Vblank(s)",
		"Reset counter to 0000h at Vblank(s)",
		"Reset counter to 0000h at Vblank(s) and pause outside of Vblank",
		"Pause until Vblank occurs once, then switch to Free Run"
	},
	// Timer 2
	{
		"Stop counter at current value (forever, no h/v-blank start)",
		"Free Run",
		"Free Run",
		"Stop counter at current value (forever, no h/v-blank start)"
	}
};

void Timers::Reset()
{
	for (unsigned int i = 0; i < 3; i++)
	{
		m_timers[i] = {};
	}

	m_inVblank = false; // the first scanline in the display is the start of the visible area
	m_inHblank = true; // the start of a scanline is in the HBLANK
}

void Timers::WriteReg(unsigned int regIndex, u32 val)
{
	// Address has been divided by 4 (shifted right 2) to get reg index, so timer index is now in bits 3:2
	unsigned int timerIndex = (regIndex >> 2) & 3;
	if (timerIndex > 2)
	{
		HP_DEBUG_FATAL_ERROR("Write to invalid timer ignored");
		return;
	}

	Timer& timer = m_timers[timerIndex];

	unsigned int timerRegIndex = regIndex & 3;
	if (timerRegIndex >= 3)
	{
		HP_DEBUG_FATAL_ERROR("Each timer only has 3 registers");
		return;
	}

	val &= 0xffff; // discard upper 16-bits

	if (s_logTimers)
		LOG_INFO("[Timers] Write timer %u %s %08X\n", timerIndex, kTimerRegNames[timerRegIndex], val);

	timer.reg[timerRegIndex] = val; 

	if (timerRegIndex == (u32)TimerReg::Value)
	{
		// When writing the Current value, it will stay at the written value for 2 clock cycles before counting up or checking against Target overflows.
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		timer.incrementDelay = 2;

		// Reset any stale state
		timer.resetNextCycle = false;
	}
	else if (timerRegIndex == (u32)TimerReg::Mode)
	{
		// Counter value is reset on any write to the mode register.
		// https://psx-spx.consoledev.net/timers/#1f801100hn10h-timer-02-current-counter-value-rw
		timer.value = 0;
		timer.accumulator = 0;

		// When resetting the Counter by writing the Mode register, it will stay at 0000h for 2 clock cycles before counting up.
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		timer.incrementDelay = 2;

		// Reset any stale state
		timer.resetNextCycle = false;

		// Mode Bit 10 is set after writing to the Mode register to acknowledge any IRQ.
		// n.b. This bit is active low, so 1 = no IRQ
		timer.mode.irq = 1;
		
		if (timer.mode.syncEnable)
		{
			if (s_logTimers)
				LOG_INFO("[Timers] Timer %u source: %s. Sync mode: %s\n", timerIndex, kTimerSourceNames[timerIndex][timer.mode.source], kTimerSyncModeNames[timerIndex][timer.mode.syncMode]);

			if (timerIndex == 0)
			{
				// Timer 0 can synchronise on HBLANK

				switch (timer.mode.syncMode)
				{
					case 0: // Pause counter during Hblank(s)
						timer.paused = m_inHblank;
						break;
					case 1: // Reset counter to 0000h at Hblank(s)
						timer.paused = false;
						break;
					case 2: // Reset counter to 0000h at Hblank(s) and pause outside of Hblank
						timer.paused = !m_inHblank;
						break;
					case 3: // Pause until Hblank occurs once, then switch to Free Run
						timer.paused = true;
						break;
				}
			}
			else if (timerIndex == 1)
			{
				// Timer 1 can synchronise on VBLANK

				switch (timer.mode.syncMode)
				{
					case 0: // Pause counter during Vblank(s)
						timer.paused = m_inVblank;
						break;
					case 1: // Reset counter to 0000h at Vblank(s)
						timer.paused = false;
						break;
					case 2: // Reset counter to 0000h at Vblank(s) and pause outside of Vblank
						timer.paused = !m_inVblank;
						break;
					case 3: // Pause until Vblank occurs once, then switch to Free Run
						timer.paused = true;
						break;
				}
			}
			else // if (timerIndex == 2)
			{
				switch (timer.mode.syncMode)
				{
					// 0 or 3 = Stop counter at current value (forever, no h/v-blank start)
					case 0:
					case 3:
						timer.paused = true;
						break;

						// 1 or 2 = Free Run (same as when Synchronization Disabled)
					case 1:
					case 2:
						timer.paused = false;
						break;
				}
			}
		}
		else // !syncEnable
		{
			if (s_logTimers)
				LOG_INFO("[Timers] Timer %u source: %s. Sync disabled (free run)\n", timerIndex, kTimerSourceNames[timerIndex][timer.mode.source]);

			// free run
			timer.paused = false;
		}

		// Allow IRQs. Will be reset after first IRQ if bit 6 IRQ Once/Repeat Mode is 0 meaning One-shot (1 means repeat)
		timer.irqEnabled = true;

		timer.pulseCountdown = 0;

		// Reading mode bits 15:13 seem to be zero in no$psx, so let's keep them at zero.
		timer.mode.unknownBits15_13 = 0;
	}
}

u32 Timers::ReadReg(unsigned int regIndex)
{
	// Address has been divided by 4 (shifted right 2) to get reg index, so timer index is now in bits 3:2
	unsigned int timerIndex = (regIndex >> 2) & 3;
	if (timerIndex > 2)
	{
		HP_DEBUG_FATAL_ERROR("Read from invalid timer ignored");
		return 0;
	}

	Timer& timer = m_timers[timerIndex];

	unsigned int timerRegIndex = regIndex & 3;
	if (timerRegIndex >= 3)
	{
		HP_DEBUG_FATAL_ERROR("Each timer only has 3 registers");
		return 0;
	}

	const u32 val = timer.reg[timerRegIndex];
	if (s_logTimerReads)
		LOG_INFO("[Timers] Read timer %u %s %08X\n", timerIndex, kTimerRegNames[timerRegIndex], val);

	// Mode reg bits 11 and 12 are reset after reading.
	if (timerRegIndex == (u32)TimerReg::Mode)
	{
		timer.mode.reachedTarget = false;
		timer.mode.reachedFFFF = false;
	}

	return val;
}

void Timers::Update(unsigned int systemClockCycleCount, unsigned int horizontalResolution, unsigned int cpuCyclesPerInstruction)
{
	// For timer 0, source 0 or 2 = System Clock,  1 or 3 = Dotclock (pixel clock)
	// Source is a 2 bit value, so this is equivalent to having lsb clear.
	Timer& t0 = m_timers[0];
	if (t0.resetNextCycle)
	{
		t0.value = 0;
		t0.resetNextCycle = false;

		// When being reset to 0000h by reaching the Target value (Mode Bit3 set), it will stay at 0000h for 2 clock cycles.
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		if (t0.mode.resetWhenReachTarget)
			t0.incrementDelay = 1; // Implementation note: one cycle already accounted for here.
	}
	else // Don't increment if just reset
	{
		if (t0.incrementDelay > 0)
		{
			t0.incrementDelay--;
		}
		else
		{
			if ((t0.mode.source & 1) == 0)
			{
				incrementTimer(t0, systemClockCycleCount);
			}
			else
			{
				// The PSX has 6 different pixel clocks
				//  - PSX.256-pix Dotclock =  5.322240MHz (44100Hz*300h*11/7/10 = CPU clock * 11/7/10)
				//  - PSX.320-pix Dotclock =  6.652800MHz (44100Hz*300h*11/7/8)
				//  - PSX.368-pix Dotclock =  7.603200MHz (44100Hz*300h*11/7/7)
				//  - PSX.512-pix Dotclock = 10.644480MHz (44100Hz*300h*11/7/5)
				//  - PSX.640-pix Dotclock = 13.305600MHz (44100Hz*300h*11/7/4)
				//  - Namco GunCon 385-pix =  8.000000MHz (from 8.00MHz on lightgun PCB) (not emulated)
				
				// There is < 1 dotclock per CPU cycle, so store values in fixed point.
				static constexpr u32 kDotClocksPerCpuCycle256_16_16 = 0x10000 * 11 / 7 / 10;
				static constexpr u32 kDotClocksPerCpuCycle320_16_16 = 0x10000 * 11 / 7 / 8;
				static constexpr u32 kDotClocksPerCpuCycle368_16_16 = 0x10000 * 11 / 7 / 7;
				static constexpr u32 kDotClocksPerCpuCycle512_16_16 = 0x10000 * 11 / 7 / 5;
				static constexpr u32 kDotClocksPerCpuCycle640_16_16 = 0x10000 * 11 / 7 / 4;

				u32 dotClockCycleCount16_16 = 0x0000'4000; // 0.25 dotclocks. This value should never be used.

				// #TODO: Optimise this. Store dotClockCycleCount16_16 as state, and update only when GPU stat written to.
				if (horizontalResolution == 256)
					dotClockCycleCount16_16 = cpuCyclesPerInstruction * kDotClocksPerCpuCycle256_16_16;
				else if (horizontalResolution == 320)
					dotClockCycleCount16_16 = cpuCyclesPerInstruction * kDotClocksPerCpuCycle320_16_16;
				else if (horizontalResolution == 368)
					dotClockCycleCount16_16 = cpuCyclesPerInstruction * kDotClocksPerCpuCycle368_16_16;
				else if (horizontalResolution == 512)
					dotClockCycleCount16_16 = cpuCyclesPerInstruction * kDotClocksPerCpuCycle512_16_16;
				else if (horizontalResolution == 640)
					dotClockCycleCount16_16 = cpuCyclesPerInstruction * kDotClocksPerCpuCycle640_16_16;

				t0.accumulator += dotClockCycleCount16_16;
				unsigned int dotClocks = t0.accumulator >> 16;
				if (dotClocks > 0)
				{
					incrementTimer(t0, dotClocks);
					t0.accumulator -= dotClocks << 16;
				}
			}
		}
	}

	// For timer 1, source 0 or 2 = System Clock,  1 or 3 = Hblank
	// Source is a 2 bit value, so this is equivalent to having lsb clear.
	Timer& t1 = m_timers[1];
	if (t1.resetNextCycle)
	{
		t1.value = 0;
		t1.resetNextCycle = false;

		// When being reset to 0000h by reaching the Target value (Mode Bit3 set), it will stay at 0000h for 2 clock cycles.
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		if (t1.mode.resetWhenReachTarget)
			t1.incrementDelay = 1; // Implementation note: one cycle already accounted for here.
	}
	else // Don't increment if just reset
	{
		if (t1.incrementDelay > 0)
		{
			t1.incrementDelay--;
		}
		else
		{
			if ((t1.mode.source & 1) == 0)
			{
				incrementTimer(t1, systemClockCycleCount);
			}
			else
			{
				// HBLANK source for timer 1. All logic handled in Hblank functions.
			}
		}
	}

	// For timer 2, source 0 or 1 = System Clock,  2 or 3 = System Clock/8 
	Timer& t2 = m_timers[2];
	if (t2.resetNextCycle)
	{
		t2.value = 0;
		t2.resetNextCycle = false;
		
		// When being reset to 0000h by reaching the Target value (Mode Bit3 set), it will stay at 0000h for 2 clock cycles.
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		if (t2.mode.resetWhenReachTarget)
			t2.incrementDelay = 1; // Implementation note: one cycle already accounted for here.
	}
	else // Don't increment if just reset
	{
		if (t2.incrementDelay > 0)
		{
			t2.incrementDelay--;
		}
		else
		{
			if ((t2.mode.source & 2) == 0)
			{
				incrementTimer(t2, systemClockCycleCount);
			}
			else
			{
				// System clock / 8 source for timer 2
				t2.accumulator += systemClockCycleCount;
				unsigned int timerTicks = t2.accumulator >> 3; // divide by 8
				if (timerTicks > 0)
				{
					incrementTimer(t2, timerTicks);
					t2.accumulator -= timerTicks << 3; // multiply by 8
				}
			}
		}
	}

	// Pulse mode logic
	for (unsigned int timerIndex = 0; timerIndex < 3; timerIndex++)
	{
		Timer& timer = m_timers[timerIndex];
		if (timer.pulseCountdown > 0)
		{
			timer.pulseCountdown -= systemClockCycleCount;
			if (timer.pulseCountdown <= 0)
			{
				timer.pulseCountdown = 0;
				timer.mode.irq = 1; // disable IRQ (active low)
			}
		}
	}
}

void Timers::HblankStart()
{
	// Timer 0 can synchronise on HBLANK
	Timer& timer0 = m_timers[0];
	if (timer0.mode.syncEnable)
	{
		switch (timer0.mode.syncMode)
		{
			case 0: // Pause counter during Hblank(s)
			{
				if (!timer0.paused)
				{
					timer0.paused = true;

					if (s_logTimers)
						LOG_INFO("[Timers] Timer 0 paused at HBLANK start\n");
				}
				break;
			}
			case 1: // Reset counter to 0000h at Hblank(s). Assume this means HBLANK start.
			{
				timer0.value = 0;

				if (s_logTimers)
					LOG_INFO("[Timers] Reset timer 0 value at HBLANK start\n");
				break;
			}
			case 2: // Reset counter to 0000h at Hblank(s) and pause outside of Hblank
			{
				timer0.value = 0;

				if (s_logTimers)
					LOG_INFO("[Timers] Reset timer 0 value at HBLANK start\n");

				if (timer0.paused)
				{
					timer0.paused = false;

					if (s_logTimers)
						LOG_INFO("[Timers] Timer 0 unpaused at HBLANK start\n");
				}
				
				break;
			}
			case 3: // Pause until Hblank occurs once, then switch to Free Run
			{
				if (timer0.paused)
				{
					timer0.paused = false;

					if (s_logTimers)
						LOG_INFO("[Timers] Timer 0 unpaused at HBLANK start\n");
				}
				break;
			}
		}
	}

	// For timer 1, source 0 or 2 = System Clock,  1 or 3 = Hblank.
	// Source is a 2 bit value, so can just test lsb.
	Timer& t1 = m_timers[1];
	if ((t1.mode.source & 1) == 1) 
		incrementTimer(t1, 1);

	m_inHblank = true;
}

void Timers::HblankEnd()
{
	// Timer 0 can synchronise on HBLANK
	Timer& timer0 = m_timers[0];
	if (timer0.mode.syncEnable)
	{
		switch (timer0.mode.syncMode)
		{
			case 0: // Pause counter during Hblank(s)
			{
				if (timer0.paused)
				{
					timer0.paused = false;

					if (s_logTimers)
						LOG_INFO("[Timers] Timer 0 unpaused at HBLANK end\n");
				}
				break;
			}
			case 1: // Reset counter to 0000h at Hblank(s). Assume this means HBLANK start.
				break;
			case 2: // Reset counter to 0000h at Hblank(s) and pause outside of Hblank
			{
				if (!timer0.paused)
				{
					timer0.paused = true;

					if (s_logTimers)
						LOG_INFO("[Timers] Timer 0 paused at HBLANK end\n");
				}
				break;
			}
			case 3: // Pause until Hblank occurs once, then switch to Free Run
				// All logic in HblankStart
				break;
		}
	}

	m_inHblank = false;
}

void Timers::VblankStart()
{
	// Timer 1 can synchronise on VBLANK
	Timer& timer1 = m_timers[1];
	if (timer1.mode.syncEnable)
	{
		switch (timer1.mode.syncMode)
		{
			case 0: // Pause counter during Vblank(s)
				timer1.paused = true;
				break;
			case 1: // Reset counter to 0000h at Vblank(s). Assume this means VBLANK start.
				timer1.value = 0;
				timer1.accumulator = 0;
				break;
			case 2: // Reset counter to 0000h at Vblank(s) and pause outside of Vblank
				timer1.value = 0;
				timer1.accumulator = 0;
				timer1.paused = false;
				break;
			case 3: // Pause until Vblank occurs once, then switch to Free Run
				if (timer1.paused)
					timer1.paused = false;
				break;
		}
	}

	m_inVblank = true;
}

void Timers::VblankEnd()
{
	// Timer 1 can synchronise on VBLANK
	Timer& timer1 = m_timers[1];
	if (timer1.mode.syncEnable)
	{
		switch (timer1.mode.syncMode)
		{
			case 0: // Pause counter during Vblank(s)
				timer1.paused = false;
				break;
			case 1: // Reset counter to 0000h at Vblank(s). Assume this means VBLANK start
				break;
			case 2: // Reset counter to 0000h at Vblank(s) and pause outside of Vblank
				timer1.paused = true;
				break;
			case 3: // Pause until Vblank occurs once, then switch to Free Run
				// All logic in VblankStart
				break;
		}
	}

	m_inVblank = false;
}

void Timers::incrementTimer(Timer& timer, unsigned int delta)
{
	if (timer.paused)
		return;

	const u32 valuePrev = timer.value;
	timer.value += delta;

	bool irq = false;
	if (timer.value >= 0xffff)
	{
		timer.mode.reachedFFFF = 1;

		timer.value = 0xffff; // max value in case of overflow when delta > 0
		timer.resetNextCycle = true;

		// Counter value is reset when reaches 0xffff;
		// if (timer.mode.resetMode == 0) // zero means reset when counter reaches 0xffff. However, this conditional results in same result in both cases!
		timer.value &= 0xffff; // reset to zero and include any overshoot due to delta > 1

		if (timer.mode.irqOnFFFF)
			irq = timer.irqEnabled;
	}

	// Take care not to step past target value if delta > 1
	if (valuePrev < timer.target && timer.value >= timer.target) // reached (or exceeded in this implementation) target value
	{
		timer.mode.reachedTarget = true; // set bit 11
		timer.value = timer.target; // set to exact target value in case of overflow when delta > 0

		// The counter increments up to (including) the selected target value.
		// Counter value is reset when reaches target value, but is visible (to CPU) at target value for one cycle.
		if (timer.mode.resetWhenReachTarget)
			timer.resetNextCycle = true;

		if (timer.mode.irqOnTarget)
			irq = timer.irqEnabled;
	}

	if (irq)
	{
		if (timer.mode.toggle)
		{
			// IRQ is active low
			if (timer.mode.irq == 1)
			{
				timer.mode.irq = 0;
				m_intc.SetIRQ((IRQ)((unsigned int)IRQ::IRQ4_TMR0 + (unsigned int)(&timer - m_timers)));
			}
			else // timer.mode.irq == 0
				timer.mode.irq = 1;
		}
		else
		{
			// 0=Short Bit 10=0 Pulse (IRQ bit is active low)
			// #TODO: How long should the pulse duration be?
			static constexpr unsigned int kPulseDurationCpuCycles = 16;

			timer.mode.irq = 0; // active low
			m_intc.SetIRQ((IRQ)((unsigned int)IRQ::IRQ4_TMR0 + (unsigned int)(&timer - m_timers)));

			timer.pulseCountdown = kPulseDurationCpuCycles;
		}

		if (!timer.mode.repeat) // one-shot?
			timer.irqEnabled = false;
	}
}
