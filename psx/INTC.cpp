#include "INTC.h"

#include "R3000.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/ArrayHelpers.h" // COUNTOF_ARRAY
#include "core/Helpers.h" // ENUM_COUNT

#if defined(_MSC_VER)
#include <intrin.h> // _tzcnt_u32 for COUNT_TRAILING_ZEROS
#endif

static const char* kIRQNames[] =
{
	"IRQ0_VBLANK",
	"IRQ1_GPU",
	"IRQ2_CDROM",
	"IRQ3_DMA",
	"IRQ4_TMR0",
	"IRQ5_TMR1",
	"IRQ6_TMR2",
	"IRQ7_ControllerAndMemoryCardByteReceived",
	"IRQ8_SIO",
	"IRQ9_SPU",
	"IRQ10_ControllerLightpen", // Also shared by PIO and DTL cards.
};
static_assert(COUNTOF_ARRAY(kIRQNames) == ENUM_COUNT(IRQ));

void INTC::Reset()
{
	// #TODO: What are the initial values of I_STAT and I_MASK?
	m_istat = 0;
	m_imask = 0;
}

void INTC::WriteISTAT(u32 val, u32 pc)
{
	// Acknowledge interrupts with bitfield. 0=Clear Bit, 1=No change
	const u32 prev = m_istat;
	m_istat &= val; // clear bits where val bits are zero

	u32 changed = prev ^ m_istat;
	if (s_logInterrupts || (s_logIRQTypeBits & changed))
	{
		while (changed)
		{
			unsigned int irqIndex = COUNT_TRAILING_ZEROS(changed);
			changed &= (changed - 1); // resetLowestSetBit();
			LOG_INFO("[INTC] %s acknowledged PC: %08X\n", kIRQNames[irqIndex], pc);
		}
	}

	updateCpuInterruptPins();
}

void INTC::WriteIMASK(u32 val)
{
	if (m_imask != val)
	{
		m_imask = val;
		updateCpuInterruptPins();
	}
}

void INTC::SetIRQ(IRQ irq)
{
	if (s_logInterrupts || (s_logIRQTypeBits & (1u << (unsigned int)irq)))
		LOG_INFO("[INTC] %s\n", kIRQNames[(int)irq]);

	const u32 flag = 1 << (u32)irq;

	// The interrupt request bits in I_STAT are edge-triggered, ie. the get set ONLY if the corresponding interrupt source changes from "false to true".
	// https://psx-spx.consoledev.net/interrupts/#interrupt-request-execution
	if (!(m_istat & flag))
	{
		m_istat |= flag; // set I_STAT flag for this IRQ

		if (s_logInterrupts || (s_logIRQTypeBits & (1u << (unsigned int)irq)))
			LOG_INFO("[INTC] Set I_STAT %s\n", kIRQNames[(int)irq]);

		updateCpuInterruptPins();
	}
}

//
// The CPU has 6 external interrupt pins, but the PSX only uses the first one.
// The COP0 CAUSE register 13 has six hardware interrupt bits in the IP field (bits 15:10).
// The PSX only uses the least significant bit (bit 10). The remaining bits 15:11 are always zero.
//
void INTC::updateCpuInterruptPins()
{
	// The IP bits are not latches; they are automatically cleared as soon as "(I_STAT AND I_MASK)=zero", so there's no need to do an acknowledge at the cop0 side.
	// https://psx-spx.consoledev.net/interrupts/#psx-specific-cop0-notes
	// Clear COP0 CAUSE register IP bit 10 as soon as there are no pending interrupts
	// All interrupts are maskable at the PSX level via the I_MASK register
	unsigned int val = (m_istat & m_imask) ? 1 : 0; // 1 corresponds to cop0r13 (CAUSE) bit 10 (least significant bit of IP field).
	if (m_cpu.GetInterruptPins() != val) // Implementation note: Set only if changed.
	{
		m_cpu.SetInterruptPins(val);

		if (s_logInterrupts)
			LOG_INFO("[INTC] Set CPU interrupt pins %u\n", val);
	}
}
