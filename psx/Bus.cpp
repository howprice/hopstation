#include "Bus.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED
#include "core/ArrayHelpers.h" // COUNTOF_ARRAY

// R3000 memory regions
//
// 32-bit address space = 4 GB = 8 * 512 MB regions
// 
//   Address   Name   Size   Privilege    Code-Cache  Data-Cache
//   00000000h KUSEG  2048M  Kernel/User  Yes         (Scratchpad)
//   80000000h KSEG0  512M   Kernel       Yes         (Scratchpad)
//   A0000000h KSEG1  512M   Kernel       No          No
//   C0000000h KSEG2  1024M  Kernel       (No code)   No
//
// Kernel Memory:
//
// KSEG1 is the normal physical memory (uncached), KSEG0 is a mirror thereof (but with cache enabled).
// KSEG2 is usually intended to contain virtual kernel memory, in the PSX it's containing Cache Control I/O Ports.
//
// User Memory:
// 
// KUSEG is intended to contain 2GB virtual memory (on extended MIPS processors), the PSX has no MMU so doesn't support virtual
// memory, and KUSEG simply contains a mirror of KSEG0/KSEG1 (in the first 512MB) (trying to access memory in the
// remaining 1.5GB causes an exception).
// 
// PlayStation memory map (CPU bus)
//
//  KUSEG      KSEG0      KSEG1
//  00000000h  80000000h  A0000000h  2048K  Main RAM (first 64K reserved for BIOS)
//  1F000000h  9F000000h  BF000000h  8192K  Expansion Region 1 (ROM/RAM)
//  1F800000h  9F800000h     --      1K     Scratchpad (D-Cache used as Fast RAM)
//  1F801000h  9F801000h  BF801000h  8K     I/O Ports (hardware registers)
//  1F802000h  9F802000h  BF802000h  8K     Expansion Region 2 (I/O Ports)
//  1FA00000h  9FA00000h  BFA00000h  2048K  Expansion Region 3 (whatever purpose)
//  1FC00000h  9FC00000h  BFC00000h  512K   BIOS ROM (Kernel) (4096K max)
//        FFFE0000h (KSEG2)          0.5K   I/O Ports (Cache Control)
// 
// https://problemkaputt.de/psx-spx.htm#memorymap
//

static constexpr u32 kRamStartAddress = 0; // 2 MB = 0x20'0000
static constexpr u32 kRamEndAddress = 8 * 1024 * 1024; // 8 MB region in which RAM is mirrored = 0x80'0000
static constexpr u32 kRamSizeBytes = 2 * 1024 * 1024; // PSX has 2 MB = 0x20'0000. Mirrored up to 8 MB
static constexpr u32 kRamMask = kRamSizeBytes - 1; // To implement mirroring

// Memory mapped I/O

// Expansion Region 1, 512 KB
static constexpr u32 kExpansionRegion1Address = 0x1F000000;
static constexpr u32 kExpansionRegion1SizeBytes = 0x80000; // 512 KB
static constexpr u32 kExpansionRegion1EndAddress = kExpansionRegion1Address + kExpansionRegion1SizeBytes;

// Scratchpad, 1 KiB fast RAM (repurposed data cache)
static constexpr u32 kScratchpadAddress = 0x1F800000;
static constexpr u32 kScratchpadSizeBytes = 0x400; // 1 KB
static constexpr u32 kScratchpadEndAddress = kScratchpadAddress + kScratchpadSizeBytes;

// Memory Control 1
static constexpr u32 kMemoryControl1Address = 0x1F801000;
static constexpr u32 kMemoryControl1SizeBytes = 9 * 4; // 9 32-bit registers
static constexpr u32 kMemoryControl1EndAddress = kMemoryControl1Address + kMemoryControl1SizeBytes;

// Peripheral I/O Ports
// https://psx-spx.consoledev.net/iomap/#peripheral-io-ports
static constexpr u32 kPeripheralStartAddress = 0x1F801040;
static constexpr u32 kPeripheralEndAddress = 0x1F801060;

// Memory Control 2
static constexpr u32 kMemoryControl2_RAM_SIZE_Address = 0x1F801060; // 32-bit register

// Interrupt control
static constexpr u32 I_STAT = 0x1F801070; // interrupt status register. R=Status, W=Acknowledge
static constexpr u32 I_MASK = 0x1F801074; // interrupt mask register R/W

// DMA registers
static constexpr u32 kDMARegistersStartAddress = 0x1F801080;
static constexpr u32 kDMARegistersEndAddress = 0x1F801100;

// Timer registers (aka Root Counters)
static constexpr u32 kTimerRegistersStartAddress = 0x1F801100;
static constexpr u32 kTimerRegistersEndAddress = 0x1F801130;

// CDROM registers
// 4 banks of 4 byte-sized registers
//  Address.Read/Write.Index
//  1F801800h.x.x   1   CD Index/Status Register (Bit0-1 R/W, Bit2-7 Read Only)
//  1F801801h.R.x   1   CD Response Fifo (R) (usually with Index1)
//  1F801802h.R.x   1/2 CD Data Fifo - 8bit/16bit (R) (usually with Index0..1)
//  1F801803h.R.0   1   CD Interrupt Enable Register (R) HINTMASK
//  1F801803h.R.1   1   CD Interrupt Flag Register (R/W) HINTSTS
//  1F801803h.R.2   1   CD Interrupt Enable Register (R) (Mirror) HINTMASK
//  1F801803h.R.3   1   CD Interrupt Flag Register (R/W) (Mirror) HINTSTS
//  1F801801h.W.0   1   CD Command Register (W)
//  1F801802h.W.0   1   CD Parameter Fifo (W)
//  1F801803h.W.0   1   CD Request Register (W) aka HCHPCTL (host chip control) register
//  1F801801h.W.1   1   Unknown/unused
//  1F801802h.W.1   1   CD Interrupt Enable Register (W) HINTMASK
//  1F801803h.W.1   1   CD Interrupt Flag Register (R/W) HINTSTS
//  1F801801h.W.2   1   Unknown/unused
//  1F801802h.W.2   1   CD Audio Volume for Left-CD-Out to Left-SPU-Input (W)
//  1F801803h.W.2   1   CD Audio Volume for Left-CD-Out to Right-SPU-Input (W)
//  1F801801h.W.3   1   CD Audio Volume for Right-CD-Out to Right-SPU-Input (W)
//  1F801802h.W.3   1   CD Audio Volume for Right-CD-Out to Left-SPU-Input (W)
//  1F801803h.W.3   1   CD Audio Volume Apply Changes (by writing bit5=1)
//
// https://psx-spx.consoledev.net/iomap/#cdrom-registers-addressreadwriteindex
//
static constexpr u32 kCDROMRegistersStartAddress = 0x1F801800;
static constexpr u32 kCDROMRegistersEndAddress = 0x1F801804;

// GPU registers
static constexpr u32 GP0     = 0x1F801810; // Write  Send GP0 Commands/Packets (Rendering and VRAM Access)
static constexpr u32 GP1     = 0x1F801814; // Write  GP1 Send GP1 Commands (Display Control)
static constexpr u32 GPUREAD = 0x1F801810; // Read   GPUREAD Read responses to GP0(C0h) and GP1(10h) commands
static constexpr u32 GPUSTAT = 0x1F801814; // Read   GPUSTAT Read GPU Status Register

// MDEC Registers
static constexpr u32 kMDEC0 = 0x1F801820; // Write: MDEC Command/Parameter Register. Read: MDEC Data/Response Register
static constexpr u32 kMDEC1 = 0x1F801824; // Write: MDEC Control/Reset Register. Read: MDEC Status Register

// SPU registers
static constexpr u32 kSpuRegistersStartAddress = 0x1F801C00;
static constexpr u32 kSpuRegistersEndAddress = 0x1F802000;

// Expansion Region 2, 512 KB
// All debugging stuff.
static constexpr u32 kExpansionRegion2Address = 0x1F802000;
static constexpr u32 kExpansionRegion2SizeBytes = 0x2000; // 8 KB
static constexpr u32 kExpansionRegion2EndAddress = kExpansionRegion2Address + kExpansionRegion2SizeBytes;

static constexpr u32 kBIOSAddress = 0x1FC00000;
static constexpr u32 kBIOSEndAddress = kBIOSAddress + kBiosSizeBytes;

// Memory Control 3
static constexpr u32 kMemoryControl3_CACHE_CONTROL_Address = 0xFFFE0130;

static u8 cpuReadByteCallback(uint32_t address, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	return pBus->ReadByte(address);
}

static u16 cpuReadHalfWordCallback(uint32_t address, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	return pBus->ReadHalfWord(address);
}

static u32 cpuReadWordCallback(uint32_t address, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	return pBus->ReadWord(address);
}

static void cpuWriteByteCallback(uint32_t address, u8 val, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	pBus->WriteByte(address, val);
}

static void cpuWriteHalfWordCallback(uint32_t address, u16 val, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	pBus->WriteHalfWord(address, val);
}

static void cpuWriteWordCallback(uint32_t address, u32 val, void* userdata)
{
	Bus* pBus = (Bus*)userdata;
	pBus->WriteWord(address, val);
}

Bus::Bus()
	: m_ram(kRamSizeBytes)
	, m_scratchpad(kScratchpadSizeBytes)
	, m_intc(m_cpu)
	, m_dmac(m_ram, m_gpu, m_spu, m_cdrom, m_mdec, m_intc)
	, m_timers(m_intc)
	, m_cdrom(m_intc, m_scheduler)
	, m_spu(m_intc, m_scheduler, m_cdrom)
	, m_sio(m_intc, m_scheduler)
{
	m_cpu.SetCallbacks(
		cpuReadByteCallback,
		cpuReadHalfWordCallback,
		cpuReadWordCallback,
		cpuWriteByteCallback,
		cpuWriteHalfWordCallback,
		cpuWriteWordCallback,
		this);
}

Bus::~Bus()
{
	
}

void Bus::Reset()
{
	m_cycleCount = 0;
	m_horizontalCpuCounter = 0;
	m_hblank = true; // assume HBLANK starts at CPU hpos 0
	m_vpos = 0;

	// Reset scheduler before components, which may want to schedule updates in their Reset() e.g. SPU
	m_scheduler.Reset();

	m_cpu.Reset();
	m_ram.Reset();
	m_scratchpad.Reset();
	m_gpu.Reset();
	m_intc.Reset();
	m_dmac.Reset();
	m_timers.Reset();
	m_cdrom.Reset();
	m_spu.Reset();
	m_sio.Reset();
	m_mdec.Reset();
}

void Bus::StepInstruction()
{
	m_scheduler.Tick(m_cpuCyclesPerInstruction);
	m_cpu.ExecuteInstruction();

	m_cycleCount += m_cpuCyclesPerInstruction;

	// #TODO[#opt]: Schedule timers. They are very high on the debug profile.
	unsigned int horizontalResolution = m_gpu.GetHorizontalResolution();
	m_timers.Update(m_cpuCyclesPerInstruction, horizontalResolution, m_cpuCyclesPerInstruction);

	// #TODO[#opt]: Schedule HBLANK/VBLANK
	m_horizontalCpuCounter += m_cpuCyclesPerInstruction;

	if (m_horizontalCpuCounter >= kCpuCyclesPerScanline)
	{
		m_horizontalCpuCounter -= kCpuCyclesPerScanline;
		m_vpos++;

		if (m_vpos == kVblankStart)
		{
			m_intc.SetIRQ(IRQ::IRQ0_VBLANK);

			m_timers.VblankStart();
		}
		else if (m_vpos == kVTOT)
		{
			m_vpos = 0;

			// VBLANK ends at scanline zero when about to draw first visible line
			m_timers.VblankEnd();
		}
	}

	// The hblank period is either side of the visible portion of the line.
	// Scanline start -> | hblank  |     visible region    | hblank | <- scanline end
	if (m_hblank)
	{
		if (m_horizontalCpuCounter >= kHStartCpuCycles && m_horizontalCpuCounter < kHStopCpuCycles)
		{
			m_hblank = false;
			m_timers.HblankEnd();
		}
	}
	else
	{
		if (m_horizontalCpuCounter >= kHStopCpuCycles)
		{
			m_hblank = true;
			m_timers.HblankStart();
		}
	}
}

void Bus::StepCycles(unsigned int cycles)
{
	const u64 targetCycleCount = m_cycleCount + cycles;
	while (m_cycleCount < targetCycleCount)
		StepInstruction();
}

//
// Helper function to map address-space address to physical address.
// This allows e.g. RAM to be accessed in either KUSEG, KSEG0 or KSEG1 ranges conveniently.
// #TODO: Respect caching.
//
static inline u32 getPhysicalAddress(u32 address)
{
	static const u32 kRegionMask[] =
	{
		// KUSEG: 2048 MB = 4 x 512 MB regions
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,

		// KSEG0: 1 x 512 MB regions
		// Strip MSb to map to physical address
		0x7fffffff,

		// KSEG1: 1 x 512 MB region
		// Strip 3 most significant bit to map to physical address
		0x1fffffff,

		// KSEG2: 1024 MB = 2 x 512 MB regions
		0xffffffff, 0xffffffff
		
	};
	static_assert(COUNTOF_ARRAY(kRegionMask) == 8, "Expect 8 x 512 MB regions");

	unsigned int regionIndex = address >> 29; // 3 MSbs
	return address & kRegionMask[regionIndex];
}

u8 Bus::ReadByte(u32 address)
{
	u32 physicalAddress = getPhysicalAddress(address);

	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		return m_ram.ReadU8(offset);
	}
	else if (physicalAddress >= kExpansionRegion1Address && physicalAddress < kExpansionRegion1EndAddress)
	{
		// Hardware pull-up resistors on data bus return a high value by default.
		return 0xff;
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		return m_scratchpad.ReadU8(offset);
	}
	else if (physicalAddress >= kPeripheralStartAddress && physicalAddress < kPeripheralEndAddress)
	{
		u32 offset = physicalAddress - kPeripheralStartAddress;
		return m_sio.Read8(offset);
	}
	else if (physicalAddress >= kDMARegistersStartAddress && physicalAddress < kDMARegistersEndAddress)
	{
		u32 offset = physicalAddress - kDMARegistersStartAddress;
		return m_dmac.Read8(offset, m_cpu.GetCurrentPC());
	}
	else if (physicalAddress >= kCDROMRegistersStartAddress && physicalAddress < kCDROMRegistersEndAddress)
	{
		// CDROM has 4 consecutive 1 byte registers
		unsigned int regIndex = (physicalAddress - kCDROMRegistersStartAddress); // [0,3]
		return m_cdrom.ReadReg(regIndex);
	}
	else if (physicalAddress >= kBIOSAddress && physicalAddress < kBIOSEndAddress)
	{
		u32 offset = physicalAddress - kBIOSAddress;
		return m_bios.ReadU8(offset);
	}
	else
	{
		HP_FATAL_ERROR("Read byte from unmapped address %08X", address);
		return 0;
	}
}

u16 Bus::ReadHalfWord(u32 address)
{
	HP_ASSERT((address & 1) == 0, "Unaligned read half word from address 0x%08X", address);

	u32 physicalAddress = getPhysicalAddress(address);

	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		return m_ram.ReadU16(offset);
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		return m_scratchpad.ReadU16(offset);
	}
	else if (physicalAddress >= kPeripheralStartAddress && physicalAddress < kPeripheralEndAddress)
	{
		u32 offset = physicalAddress - kPeripheralStartAddress;
		return m_sio.Read16(offset);
	}
	else if (physicalAddress == I_STAT)
	{
		// Halfword read of 32-bit register returns the lower 16-bits.
		// I think it has to from the spec because only the lower 16-bits contain useful data. https://psx-spx.consoledev.net/interrupts/#1f801074h-i_mask-interrupt-mask-register-rw
		u16 val = m_intc.ReadISTAT() & 0xffff;
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Read I_STAT halfword address %08X value %04X\n", address, val);
		return val;
	}
	else if (physicalAddress == I_MASK)
	{
		// Halfword read of 32-bit register returns the lower 16-bits.
		// I think it has to from the spec because only the lower 16-bits contain useful data. https://psx-spx.consoledev.net/interrupts/#1f801074h-i_mask-interrupt-mask-register-rw
		u16 val = m_intc.ReadIMASK() & 0xffff;
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Read I_MASK halfword address %08X value %04X\n", address, val);
		return val;
	}
	else if (physicalAddress >= kSpuRegistersStartAddress && physicalAddress < kSpuRegistersEndAddress)
	{
		u32 offset = physicalAddress - kSpuRegistersStartAddress;
		return m_spu.Read16(offset);
	}
	else if (physicalAddress >= kTimerRegistersStartAddress && physicalAddress < kTimerRegistersEndAddress)
	{
		// Word-sized registers. Assume least significant 2 bits are ignored.
		unsigned int regIndex = (physicalAddress - kTimerRegistersStartAddress) >> 2; // [0,31]

		// Upper 16-bits of all timer registers are documented as garbage, so they are probably just 16-bit registers internally and the upper 16-bits is bus noise.
		return (u16)m_timers.ReadReg(regIndex);
	}
	else if (physicalAddress >= kBIOSAddress && physicalAddress < kBIOSEndAddress)
	{
		u32 offset = physicalAddress - kBIOSAddress;
		return m_bios.ReadU16(offset);
	}
	else if (physicalAddress == kTimerRegistersEndAddress)
	{
		// Gran Turismo performs a 16-bit read from address 1F801130, which is just past the end of timer address space.
		return 0; // Not sure what to return here. Wrap to timers start, high-Z or last bus value?
	}
	else
	{
		HP_FATAL_ERROR("Read half word from unmapped address %08X", address);
		return 0;
	}
}

u32 Bus::ReadWord(u32 address)
{
	HP_ASSERT((address & 3) == 0, "Unaligned read from address 0x%08X", address);

	u32 physicalAddress = getPhysicalAddress(address);

	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		return m_ram.ReadU32(offset);
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		return m_scratchpad.ReadU32(offset);
	}
	else if (physicalAddress >= kMemoryControl1Address && physicalAddress < kMemoryControl1EndAddress)
	{
		// Hopefully can ignore all other Memory Control 1 register writes for non-cycle-exact emulation.
		u32 offset = physicalAddress - kMemoryControl1Address;
		HP_UNUSED(offset);

		if (s_logMemoryControlRegisterAccess)
			LOG_INFO("Unimplemented read from I/O Memory Control 1 register address %08X\n", address);
		return 0;
	}
	else if (physicalAddress == I_STAT)
	{
		u32 istat = m_intc.ReadISTAT();
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Read I_STAT address %08X value %08X\n", address, istat); // #TODO: Print summary of which IRQ bits are set
		return istat;
	}
	else if (physicalAddress == I_MASK)
	{
		u32 imask = m_intc.ReadIMASK();
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Read I_MASK address %08X value %08X\n", address, imask);
		return imask;
	}
	else if (physicalAddress >= kPeripheralStartAddress && physicalAddress < kPeripheralEndAddress)
	{
		u32 offset = physicalAddress - kPeripheralStartAddress;
		return m_sio.Read32(offset);
	}
	else if (physicalAddress >= kDMARegistersStartAddress && physicalAddress < kDMARegistersEndAddress)
	{
		static_assert((kDMARegistersEndAddress - kDMARegistersStartAddress) / 4 == 32, "Expect max 32 DMA registers");
		// Assume least significant 2 bits are ignored
		unsigned int regIndex = (physicalAddress - kDMARegistersStartAddress) / 4; // [0,31]
		return m_dmac.ReadReg(regIndex, m_cpu.GetCurrentPC());
	}
	else if (physicalAddress >= kTimerRegistersStartAddress && physicalAddress < kTimerRegistersEndAddress)
	{
		// Word-sized registers. Assume least significant 2 bits are ignored.
		unsigned int regIndex = (physicalAddress - kTimerRegistersStartAddress) >> 2; // [0,31]
		return m_timers.ReadReg(regIndex);
	}
	else if (physicalAddress == GPUREAD)
	{
		return m_gpu.GetGPUREAD();
	}
	else if (physicalAddress == GPUSTAT)
	{
		return m_gpu.GetGPUSTAT();
	}
	else if (physicalAddress == kMDEC0)
	{
		return m_mdec.ReadMDEC0();
	}
	else if (physicalAddress == kMDEC1)
	{
		return m_mdec.ReadMDEC1();
	}
	else if (physicalAddress >= kBIOSAddress && physicalAddress < kBIOSEndAddress)
	{
		u32 offset = physicalAddress - kBIOSAddress;
		return m_bios.ReadU32(offset);
	}
	else if (physicalAddress == kMemoryControl2_RAM_SIZE_Address)
	{
		return 0xB88; // Expect RAM_SIZE register to be set to BB8h on PSX
	}
	else
	{
		HP_FATAL_ERROR("Read word from unmapped address %08X", address);
		return 0;
	}
}

void Bus::WriteWord(u32 address, u32 val)
{
	HP_ASSERT((address & 3) == 0, "Unaligned write to address 0x%08X", address);

	u32 physicalAddress = getPhysicalAddress(address);

	// https://problemkaputt.de/psx-spx.htm#iomap
	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		m_ram.WriteU32(offset, val);
	}
	else if (physicalAddress >= kExpansionRegion1Address && physicalAddress < kExpansionRegion1EndAddress)
	{
		HP_FATAL_ERROR("Unimplemented write to Expansion Region 1 address %08X, value %08X", address, val);
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		m_scratchpad.WriteU32(offset, val);
	}
	else if (physicalAddress >= kMemoryControl1Address && physicalAddress < kMemoryControl1EndAddress)
	{
		u32 offset = physicalAddress - kMemoryControl1Address;
		if (offset == 0)
		{
			// 1F801000 Expansion 1 Base Address
			HP_DEBUG_ASSERT(val == 0x1F000000, "Expect Expansion 1 base address to be 1F000000 on PSX");
		}
		else if (offset == 4)
		{
			// 1F801004 Expansion 2 Base Address
			HP_DEBUG_ASSERT(val == 0x1F802000, "Expect Expansion 1 base address to be 1F802000 on PSX");
		}
		else
		{
			// Hopefully can ignore all other Memory Control 1 register writes for non-cycle-exact emulation.
			if (s_logMemoryControlRegisterAccess)
				LOG_INFO("Unimplemented write to KUSEG I/O Memory Control 1 register address %08X, value %08X\n", address, val);
		}
	}
	else if (physicalAddress == kMemoryControl2_RAM_SIZE_Address)
	{
		if (val != 0xB88)
			LOG_WARN("Memory Control 2 RAM_SIZE register set to %Xh. Exepect BB8h on PSX.\n", val); // EWJ2 writes 0x888
	}
	else if (physicalAddress == kMemoryControl3_CACHE_CONTROL_Address)
	{
		// #TODO: Implement caches
		if (s_logCacheControlRegisterAccess)
			LOG_INFO("Unimplemented write to KSEG2 I/O Memory Control 3 CACHE_CONTROL register address %08X, value %08X\n", address, val);
	}
	else if (physicalAddress == I_STAT)
	{
		m_intc.WriteISTAT(val, m_cpu.GetCurrentPC());

		if (s_logInterruptRegisterAccess)
			LOG_INFO("Write I_STAT address %08X, value %08X\n", address, val); // #TODO: Print which bits have been acknowledged (0 bits in val)
	}
	else if (physicalAddress == I_MASK)
	{
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Write I_MASK address %08X value %04X%s%s%s%s%s%s%s%s%s%s%s\n", address, val, // Print summary of which IRQs have been enabled
				(val & 0x0001) ? " VBLANK" : "",
				(val & 0x0002) ? " GPU" : "",
				(val & 0x0004) ? " CDROM" : "",
				(val & 0x0008) ? " DMA" : "",
				(val & 0x0010) ? " TMR0" : "",
				(val & 0x0020) ? " TMR1" : "",
				(val & 0x0040) ? " TMR2" : "",
				(val & 0x0080) ? " Controller/card" : "",
				(val & 0x0100) ? " SIO" : "",
				(val & 0x0200) ? " SPU" : "",
				(val & 0x0400) ? " Controller" : "");
		m_intc.WriteIMASK(val);
	}
	else if (physicalAddress >= kTimerRegistersStartAddress && physicalAddress < kTimerRegistersEndAddress)
	{
		// Word-sized registers. Assume least significant 2 bits are ignored.
		unsigned int regIndex = (physicalAddress - kTimerRegistersStartAddress) >> 2; // [0,31]
		return m_timers.WriteReg(regIndex, val);
	}
	else if (physicalAddress >= kDMARegistersStartAddress && physicalAddress < kDMARegistersEndAddress)
	{
		static_assert((kDMARegistersEndAddress - kDMARegistersStartAddress) / 4 == 32, "Expect max 32 DMA registers");
		// Assume least significant 2 bits are ignored
		unsigned int regIndex = (physicalAddress - kDMARegistersStartAddress) / 4; // [0,31]
		m_dmac.WriteReg(regIndex, val, m_cpu.GetCurrentPC());
	}
	else if (physicalAddress == GP0)
	{
		m_gpu.WriteGP0(val);
	}
	else if (physicalAddress == GP1)
	{
		m_gpu.WriteGP1(val);
	}
	else if (physicalAddress == kMDEC0)
	{
		m_mdec.WriteMDEC0(val, m_cpu.GetCurrentPC());
	}
	else if (physicalAddress == kMDEC1)
	{
		m_mdec.WriteMDEC1(val);
	}
	else if (physicalAddress >= kSpuRegistersStartAddress && physicalAddress < kSpuRegistersEndAddress)
	{
		u32 offset = physicalAddress - kSpuRegistersStartAddress;
		m_spu.Write32(offset, val);
	}
	else
	{
		HP_FATAL_ERROR("Unhandled write to address %08X, value %08X", address, val);
	}
}

void Bus::WriteHalfWord(u32 address, u16 val)
{
	// #TODO: Support half-word writes to other devices.

	u32 physicalAddress = getPhysicalAddress(address);

	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		m_ram.WriteU16(offset, val);
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		m_scratchpad.WriteU16(offset, val);
	}
	else if (physicalAddress >= kPeripheralStartAddress && physicalAddress < kPeripheralEndAddress)
	{
		u32 offset = physicalAddress - kPeripheralStartAddress;
		return m_sio.Write16(offset, val);
	}
	else if (physicalAddress == I_STAT)
	{
		// Halfword write to 32-bit register writes the lower 16-bits.
		// I think it has to from the spec because only the lower 16-bits contain useful data. https://psx-spx.consoledev.net/interrupts/#1f801074h-i_mask-interrupt-mask-register-rw
		// Retain upper 16-bits, even though documented as garbage
		u32 istat = m_intc.ReadISTAT();
		m_intc.WriteISTAT((istat & 0xffff0000) | val, m_cpu.GetCurrentPC());

		if (s_logInterruptRegisterAccess)
			LOG_INFO("Write I_STAT address %08X, value %04X\n", address, val); // #TODO: Print which bits have been acknowledged (0 bits in val)
	}
	else if (physicalAddress == I_MASK)
	{
		// Halfword write to 32-bit register writes the lower 16-bits.
		// I think it has to from the spec because only the lower 16-bits contain useful data. https://psx-spx.consoledev.net/interrupts/#1f801074h-i_mask-interrupt-mask-register-rw
		if (s_logInterruptRegisterAccess)
			LOG_INFO("Write I_MASK halfword address %08X value %04X%s%s%s%s%s%s%s%s%s%s%s\n", address, val, // Print summary of which IRQs have been enabled
				(val & 0x0001) ? " VBLANK" : "",
				(val & 0x0002) ? " GPU" : "",
				(val & 0x0004) ? " CDROM" : "",
				(val & 0x0008) ? " DMA" : "",
				(val & 0x0010) ? " TMR0" : "",
				(val & 0x0020) ? " TMR1" : "",
				(val & 0x0040) ? " TMR2" : "",
				(val & 0x0080) ? " Controller/card" : "",
				(val & 0x0100) ? " SIO" : "",
				(val & 0x0200) ? " SPU" : "",
				(val & 0x0400) ? " Controller" : "");

		// retain upper 16-bits, even though documented as garbage
		u32 imask = m_intc.ReadIMASK();
		m_intc.WriteIMASK((imask & 0xffff0000) | val);
	}
	else if (physicalAddress >= kTimerRegistersStartAddress && physicalAddress < kTimerRegistersEndAddress)
	{
		// Word-sized registers. Assume least significant 2 bits are ignored.
		unsigned int regIndex = (physicalAddress - kTimerRegistersStartAddress) >> 2; // [0,31]
		return m_timers.WriteReg(regIndex, (u32)val);
	}
	else if (physicalAddress >= kSpuRegistersStartAddress && physicalAddress < kSpuRegistersEndAddress)
	{
		u32 offset = physicalAddress - kSpuRegistersStartAddress;
		m_spu.Write16(offset, val);
	}
	else if (physicalAddress >= kExpansionRegion2Address && physicalAddress < kExpansionRegion2EndAddress)
	{
		// All debugging stuff, so just ignore
		if (s_logExpansion2RegisterAccess)
			LOG_INFO("Unimplemented write to Expansion Region 2 address %08X value %04X\n", address, val);
	}
	else
	{
		HP_FATAL_ERROR("Unhandled write to address %08X, value %08X", address, val);
	}
}

void Bus::WriteByte(u32 address, u8 val)
{
	u32 physicalAddress = getPhysicalAddress(address);

	if (physicalAddress < kRamEndAddress)
	{
		u32 offset = physicalAddress & kRamMask;
		m_ram.WriteU8(offset, val);
	}
	else if (physicalAddress >= kScratchpadAddress && physicalAddress < kScratchpadEndAddress)
	{
		u32 offset = physicalAddress - kScratchpadAddress;
		m_scratchpad.WriteU8(offset, val);
	}
	else if (physicalAddress >= kPeripheralStartAddress && physicalAddress < kPeripheralEndAddress)
	{
		u32 offset = physicalAddress - kPeripheralStartAddress;
		return m_sio.Write8(offset, val);
	}
	else if (physicalAddress >= kDMARegistersStartAddress && physicalAddress < kDMARegistersEndAddress)
	{
		u32 offset = physicalAddress - kDMARegistersStartAddress;
		m_dmac.Write8(offset, val, m_cpu.GetCurrentPC());
	}
	else if (physicalAddress >= kCDROMRegistersStartAddress && physicalAddress < kCDROMRegistersEndAddress)
	{
		// CDROM has 4 consecutive 1 byte registers
		unsigned int regIndex = (physicalAddress - kCDROMRegistersStartAddress); // [0,3]
		m_cdrom.WriteReg(regIndex, val);
	}
	else if (physicalAddress >= kExpansionRegion2Address && physicalAddress < kExpansionRegion2EndAddress)
	{
		// All debugging stuff, so just ignore
		if (s_logExpansion2RegisterAccess)
			LOG_INFO("Unimplemented write to Expansion Region 2 address %08X value %02X\n", address, val);
	}
	else
	{
		HP_FATAL_ERROR("Unhandled write to address %08X, value %08X", address, val);
	}
}
