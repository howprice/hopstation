#pragma once

#include "core/Types.h"

class R3000;

inline bool s_logInterruptRegisterAccess = false;
inline bool s_logInterrupts = false;
inline unsigned int s_logIRQTypeBits = 0b000'0000'0000;

// PSX interrupts
// 
// https://psx-spx.consoledev.net/interrupts/#1f801070h-i_stat-interrupt-status-register-rstatus-wacknowledge
//
enum class IRQ
{
	IRQ0_VBLANK, // (PAL=50Hz, NTSC=60Hz)
	IRQ1_GPU, //  Can be requested via GP0(1Fh) command (rarely used)
	IRQ2_CDROM,
	IRQ3_DMA,
	IRQ4_TMR0,  // Timer 0 aka Root Counter 0 (Sysclk or Dotclk)
	IRQ5_TMR1,  // Timer 1 aka Root Counter 1 (Sysclk or H-blank)
	IRQ6_TMR2,  // Timer 2 aka Root Counter 2 (Sysclk or Sysclk/8)
	IRQ7_ControllerAndMemoryCardByteReceived,
	IRQ8_SIO,
	IRQ9_SPU,
	IRQ10_ControllerLightpen, // Also shared by PIO and DTL cards.

	Max = IRQ10_ControllerLightpen
};

//
// PSX Interrupt Controller
//
// https://psx-spx.consoledev.net/interrupts/
//
// COP0 additionally has two software interrupt bits, cop0r13.bit8-9, which do exist in the PSX, too.
// 
class INTC
{
public:

	INTC(R3000& cpu) : m_cpu(cpu) {}

	void Reset();

	u32 ReadISTAT() const { return m_istat; }
	void WriteISTAT(u32 val, u32 pc);

	u32 ReadIMASK() const { return m_imask; }
	void WriteIMASK(u32 val);

	void SetIRQ(IRQ irq);

private:

	// Interrupt status register I_STAT
	//  Bit
	//   0     IRQ0 VBLANK (PAL=50Hz, NTSC=60Hz)
	//   1     IRQ1 GPU   Can be requested via GP0(1Fh) command (rarely used)
	//   2     IRQ2 CDROM
	//   3     IRQ3 DMA
	//   4     IRQ4 TMR0  Timer 0 aka Root Counter 0 (Sysclk or Dotclk)
	//   5     IRQ5 TMR1  Timer 1 aka Root Counter 1 (Sysclk or H-blank)
	//   6     IRQ6 TMR2  Timer 2 aka Root Counter 2 (Sysclk or Sysclk/8)
	//   7     IRQ7 Controller and Memory Card - Byte Received Interrupt
	//   8     IRQ8 SIO
	//   9     IRQ9 SPU
	//   10    IRQ10 Controller - Lightpen Interrupt. Also shared by PIO and DTL cards.
	//   11-15 Not used (always zero)
	//   16-31 Garbage
	// 
	// https://psx-spx.consoledev.net/interrupts/#1f801070h-i_stat-interrupt-status-register-rstatus-wacknowledge
	//
	u32 m_istat = 0;

	// Interrupt mask register I_MASK
	//
	// Bits same as I_STAT
	// 
	// https://psx-spx.consoledev.net/interrupts/#1f801074h-i_mask-interrupt-mask-register-rw
	// 
	u32 m_imask = 0;

	R3000& m_cpu;

	void updateCpuInterruptPins();
};
