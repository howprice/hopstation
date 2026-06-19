#pragma once

#include "BIOS.h"
#include "GPU.h"
#include "RAM.h"
#include "R3000.h"
#include "DMA.h"
#include "INTC.h"
#include "Timers.h"
#include "CDROM.h"
#include "SPU.h"
#include "SIO.h"
#include "MDEC.h"
#include "Scheduler.h"
#include "Timing.h"

class Bus
{
public:

	Bus();
	~Bus();

	void Reset();

	void StepInstruction();
	void StepCycles(unsigned int cycles); // step N cycles

	// For the R3000, a word is 32-bits.
	// #TODO: Hide this API or make it private if possible.
	u8 ReadByte(u32 address);
	u16 ReadHalfWord(u32 address);
	u32 ReadWord(u32 address);
	void WriteByte(u32 address, u8 val);
	void WriteHalfWord(u32 address, u16 val);
	void WriteWord(u32 address, u32 val);

	R3000& GetCPU() { return m_cpu; }
	BIOS& GetBIOS() { return m_bios; }
	const BIOS& GetBIOS() const { return m_bios; }
	RAM& GetRAM() { return m_ram; }
	GPU& GetGPU() { return m_gpu; }
	const GPU& GetGPU() const { return m_gpu; }
	CDROM& GetCDROM() { return m_cdrom; }
	SPU& GetSPU() { return m_spu; }
	SIO& GetSIO() { return m_sio; }
	const DMAC& GetDMAC() const { return m_dmac; }

	void SetCpuCyclesPerInstruction(unsigned int val) { m_cpuCyclesPerInstruction = val; }

	u64 GetCycleCount() const { return m_cycleCount; }

private:
	u64 m_cycleCount = 0; // system cycle counter

	// video beam counters
	u32 m_horizontalCpuCounter = 0; // horizontal position in CPU cycles
	bool m_hblank = true; // video beam will start in HBLANK
	u32 m_vpos = 0; // vertical beam position i.e. scanline index

	// Two cycles per instruction (CPI) is apparantly a good approximation and will allow many games to run.
	// However, Jakub's timers test assumes one cycle per instruction.
	unsigned int m_cpuCyclesPerInstruction = 2;

	// Ensure the scheduler is initialised before components that may want to schedule events during construction.
	Scheduler m_scheduler;

	R3000 m_cpu;
	BIOS m_bios;
	RAM m_ram;
	RAM m_scratchpad;
	GPU m_gpu;
	INTC m_intc;
	DMAC m_dmac;
	Timers m_timers;
	CDROM m_cdrom;
	SPU m_spu;
	SIO m_sio;
	MDEC m_mdec;
};

inline bool s_logMemoryControlRegisterAccess = false;
inline bool s_logCacheControlRegisterAccess = false;
inline bool s_logExpansion2RegisterAccess = false; // All PSX devkit debugging stuff, so just ignore
