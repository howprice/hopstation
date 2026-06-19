// PSX Timers aka Root Counters
//
// https://psx-spx.consoledev.net/timers/

#pragma once

#include "core/Types.h"

inline bool s_logTimers = false;
inline bool s_logTimerReads = false;

class INTC;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

class Timers
{
public:

	Timers(INTC& intc)
		: m_intc(intc)
	{}

	void Reset();

	void WriteReg(unsigned int index, u32 val);
	u32 ReadReg(unsigned int index);

	// Both params are deltas since last call.
	// This is currently called once per instruction, so system clock cycle count is 1 or 2.
	// Dotclock frequency is less than CPU freqency, so will be < 1 per call, so pass as fixed point 16:16 value.
	void Update(unsigned int systemClockCycleCount, unsigned int horizontalResolution, unsigned int cpuCyclesPerInstruction);

	void HblankStart();
	void HblankEnd();

	void VblankStart();
	void VblankEnd();

private:

	enum class TimerReg
	{
		Value, // current counter value
		Mode,
		Target,

		Max = Target
	};

	// 1F801104h+N*10h - Timer 0..2 Counter Mode (R/W)
	// 
	//  0     Synchronization Enable (0=Free Run, 1=Synchronize via Bit1-2)
	//  1-2   Synchronization Mode   (0-3, see lists below)
	//         Synchronization Modes for Counter 0:
	//           0 = Pause counter during Hblank(s)
	//           1 = Reset counter to 0000h at Hblank(s)
	//           2 = Reset counter to 0000h at Hblank(s) and pause outside of Hblank
	//           3 = Pause until Hblank occurs once, then switch to Free Run
	//         Synchronization Modes for Counter 1:
	//           Same as above, but using Vblank instead of Hblank
	//         Synchronization Modes for Counter 2:
	//           0 or 3 = Stop counter at current value (forever, no h/v-blank start)
	//           1 or 2 = Free Run (same as when Synchronization Disabled)
	//  3     Reset counter to 0000h  (0=After Counter=FFFFh, 1=After Counter=Target)
	//  4     IRQ when Counter=Target (0=Disable, 1=Enable)
	//  5     IRQ when Counter=FFFFh  (0=Disable, 1=Enable)
	//  6     IRQ Once/Repeat Mode    (0=One-shot, 1=Repeatedly)
	//  7     IRQ Pulse/Toggle Mode   (0=Short Bit 10=0 Pulse, 1=Toggle Bit10 on/off)
	//  8-9   Clock Source (0-3, see list below)
	//         Counter 0:  0 or 2 = System Clock,  1 or 3 = Dotclock (pixel clock)
	//         Counter 1:  0 or 2 = System Clock,  1 or 3 = Hblank
	//         Counter 2:  0 or 1 = System Clock,  2 or 3 = System Clock/8
	//  10    Interrupt Request       (0=Yes, 1=No) (Set after Writing)    (W=1) (R)
	//  11    Reached Target Value    (0=No, 1=Yes) (Reset after Reading)        (R)
	//  12    Reached FFFFh Value     (0=No, 1=Yes) (Reset after Reading)        (R)
	//  13-15 Unknown (seems to be always zero)
	//  16-31 Garbage (next opcode)
	// 
	// https://psx-spx.consoledev.net/timers/#1f801104hn10h-timer-02-counter-mode-rw
	//
	struct Mode
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 syncEnable : 1; // Bit 0 0=Free Run, 1=Synchronize via syncMode bits [2:1]
				u32 syncMode : 2; // Bits 2:1
				u32 resetWhenReachTarget : 1; // Bit 3  Reset counter to 0000h:  0=After Counter=FFFFh, 1=After Counter=Target
				u32 irqOnTarget : 1; // Bit 4
				u32 irqOnFFFF : 1; // Bit 5
				u32 repeat : 1; // Bit 6. IRQ Once/Repeat Mode: 0=One-shot, 1=Repeatedly
				u32 toggle : 1; // Bit 7. Else pulse bit 10
				u32 source : 2; // Bits 9:8. Implementation note: Can't use enum because source varies per counter.

				// IRQ
				// n.b. 0=Yes, 1=No.
				// Always set after writing the register.
				// Usually only low for a few cycles when an interrupt occurs.
				u32 irq : 1; // Bit 10. n.b. 0=Yes, 1=No.

				u32 reachedTarget : 1; // Bit 11
				u32 reachedFFFF : 1; // Bit 12
				u32 unknownBits15_13 : 3;

				u32 unusedBits31_16 : 16;
			};
		};
	};

	struct Timer
	{
		union {
			u32 reg[3];

			struct {
				u32 value;
				Mode mode;
				u32 target;
			};
		};

		u32 accumulator = 0; // for timer2 source = system clock / 8
		bool paused = false;
		bool irqEnabled = false; // state for mode bit 6 IRQ Once/Repeat Mode (0=One-shot, 1=Repeatedly)
		int pulseCountdown = 0;

		// Delay countdown to implement this behaviour:
		//   1. When resetting the Counter by writing the Mode register, it will stay at 0000h for 2 clock cycles before counting up.
		//   2. When writing the Current value, it will stay at the written value for 2 clock cycles before counting up or checking against Target overflows.
		//   3. When being reset to 0000h by reaching the Target value (Mode Bit3 set), it will stay at 0000h for 2 clock cycles.
		// Note:
		//   When wrapping around at FFFFh (Mode Bit3 not set), it will stay at 0000h for only 1 clock cycle. i.e. no delay
		// https://psx-spx.consoledev.net/timers/#reset-and-wrap
		int incrementDelay = 0;

		// Flag to reset the counter on the next cycle.
		// Used to implement behaviour:
		// - When the Target flag is set (Bit3 of the Control register), the counter increments up to (including) the selected target value, and does then restart at 0000h.
		// https://psx-spx.consoledev.net/timers/#1f801108hn10h-timer-02-counter-target-value-rw
		bool resetNextCycle = false;
	};

	Timer m_timers[3]{};

	bool m_inVblank = false; // the first scanline in the display is the start of the visible area
	bool m_inHblank = true; // the start of a scanline is in the HBLANK

	INTC& m_intc; // interrupt controller

	void incrementTimer(Timer& timer, unsigned int delta);
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
