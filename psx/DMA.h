// PSX DMA controller

#pragma once

#include "core/Helpers.h" // ENUM_COUNT
#include "core/Types.h"

class CDROM;
class GPU;
class INTC;
class MDEC;
class RAM;
class SPU;

inline bool s_logDMARegisterAccess = false;
inline bool s_logDMA = false;
inline unsigned int s_logDMAChannelBits = 0b000'0000; // Log specific channel(s). See DMAChannel enum below.

/*
PSX has 7 DMA channels:

- DMA0  MDECin (RAM to MDEC)
- DMA1  MDECout (MDEC to RAM)
- DMA2  GPU (lists + image data)
- DMA3  CDROM (CDROM to RAM)
- DMA4  SPU
- DMA5  PIO (Expansion Port)
- DMA6  OTC (GPU depth Ordering Table clear)
*/
enum class DMAChannel
{
	MDECin,
	MDECout,
	GPU,
	CDROM,
	SPU,
	PIO,
	OTC,

	Max = OTC
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

//
// DMA Controller
//
class DMAC
{
public:

	struct Channel
	{
		u32 madr = 0; // Memory Address Register

		// Block Control Register
		// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
		u32 bcr = 0;

		enum class TransferDirection : u32
		{
			DeviceToRAM = 0,
			RAMToDevice = 1,

			Max = RAMToDevice
		};

		enum class AddressIncrement : u32
		{
			Increment = 0, // +4 per step
			Decrement = 1, // -4 per step
		};

		enum class SyncMode : u32
		{
			Burst = 0, // Used for OTC and CDROM
			Slice = 1, // aka Block transfer mode. Used for MDEC, SPU, and GPU-vram-data
			LinkedList = 2, // used for GPU-command-lists
			Reserved = 3,

			Max = Reserved
		};

		// 1F801088h+N*10h - D#_CHCR - DMA Channel Control (Channel 0..6) (R/W)
		//
		// Bits   Description
		//  0     Transfer direction (0=device to RAM, 1=RAM to device)
		//  1     MADR increment per step (0=+4, 1=-4)
		//  2-7   Unused
		//  8     When 1:
		//        -Burst mode: enable "chopping" (cycle stealing by CPU)
		//        -Slice mode: Causes DMA to hang
		//        -Linked-list mode: Transfer header before data?
		//  9-10  Transfer mode (SyncMode)
		//        0=Burst (transfer data all at once after DREQ is first asserted)
		//        1=Slice (split data into blocks, transfer next block whenever DREQ is asserted)
		//        2=Linked-list mode
		//        3=Reserved
		//  11-15 Unused
		//  16-18 Chopping DMA window size (1 << N words)
		//  19    Unused
		//  20-22 Chopping CPU window size (1 << N cycles)
		//  23    Unused
		//  24    Start transfer (0=stopped/completed, 1=start/busy)
		//  25-27 Unused
		//  28    Force transfer start without waiting for DREQ. 0 = automatic. 1 = manual
		//  29    In forced-burst mode, pauses transfer while set.
		//        In other modes, stops bit 28 from being cleared after a slice is transferred.
		//        No effect when transfer was caused by a DREQ.
		//  30    Perform bus snooping (allows DMA to read from -nonexistent- cache?)
		//  31    Unused
		// 
		// https://psx-spx.consoledev.net/dmachannels/#1f801088hn10h-d_chcr-dma-channel-control-channel-06-rw
		struct ControlRegister
		{
			union {
				u32 val;

				// n.b. Bits are ordered from least to most significant
				struct {
					TransferDirection transferDirection : 1; // Bit 0
					AddressIncrement addressIncrement : 1; // Bit 1
					u32 unusedBits7to2 : 6;               // Bits 7:2
					u32 choppingOrHeaderEnable : 1;      // Bit 8
					SyncMode syncMode : 2;            // Bits 10:9
					u32 unusedBits15to11 : 5;            // Bits 15:11
					u32 choppingDMAWindowSize : 3;      // Bits 18:16
					u32 unusedBit19 : 1;                 // Bit 19
					u32 choppingCPUWindowSize : 3;      // Bits 22:20
					u32 unusedBit23 : 1;                 // Bit 23
					u32 startTransfer : 1;               // Bit 24
					u32 unusedBits27to25 : 3;            // Bits 27:25
					u32 startTrigger : 1;         // Bit 28
					u32 keepTransferActive : 1;         // Bit 29
					u32 busSnoopingEnable : 1;          // Bit 30
					u32 unusedBit31 : 1;                 // Bit 31
				};
			};
		};

		ControlRegister chcr{};
	};

	struct Stats
	{
		u64 transferCount[ENUM_COUNT(DMAChannel)]{};
	};

	DMAC(RAM& ram, GPU& gpu, SPU& spu, CDROM& cdrom, MDEC& mdec, INTC& intc)
		: m_ram(ram)
		, m_gpu(gpu)
		, m_spu(spu)
		, m_cdrom(cdrom)
		, m_mdec(mdec)
		, m_intc(intc)
	{
	}

	// This is not a Reset command, rather resets internal state to known values.
	void Reset();

	// regIndex [0,31]
	u32 ReadReg(unsigned int regIndex, u32 pc);
	void WriteReg(unsigned int regIndex, u32 val, u32 pc);

	// Used during MDEC decoding
	u8 Read8(unsigned int offset, u32 pc);
	void Write8(unsigned int offset, u8 val, u32 pc);

	const Stats& GetStats() const { return m_stats; }

private:

	// 1F8010F0h - DPCR - DMA Control Register (R/W)
	//
	//   Bit   Chan  Description
	//   0-2   DMA0, MDECin  Priority      (0..7; 0=Highest, 7=Lowest)
	//   3     DMA0, MDECin  Master Enable (0=Disable, 1=Enable)
	//   4-6   DMA1, MDECout Priority      (0..7; 0=Highest, 7=Lowest)
	//   7     DMA1, MDECout Master Enable (0=Disable, 1=Enable)
	//   8-10  DMA2, GPU     Priority      (0..7; 0=Highest, 7=Lowest)
	//   11    DMA2, GPU     Master Enable (0=Disable, 1=Enable)
	//   12-14 DMA3, CDROM   Priority      (0..7; 0=Highest, 7=Lowest)
	//   15    DMA3, CDROM   Master Enable (0=Disable, 1=Enable)
	//   16-18 DMA4, SPU     Priority      (0..7; 0=Highest, 7=Lowest)
	//   19    DMA4, SPU     Master Enable (0=Disable, 1=Enable)
	//   20-22 DMA5, PIO     Priority      (0..7; 0=Highest, 7=Lowest)
	//   23    DMA5, PIO     Master Enable (0=Disable, 1=Enable)
	//   24-26 DMA6, OTC     Priority      (0..7; 0=Highest, 7=Lowest)
	//   27    DMA6, OTC     Master Enable (0=Disable, 1=Enable)
	//   28-30 CPU memory access priority  (0..7; 0=Highest, 7=Lowest)
	//   31    No effect, should be CPU memory access enable (R/W)
	// 
	// Initial value on reset is 07654321h
	// 
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f0h-dpcr-dma-control-register-rw
	//
	// If two or more channels have the same priority setting, then the priority is determined by the channel number (DMA0=Lowest, DMA6=Highest, CPU=higher than DMA6?).
	// https://psx-spx.consoledev.net/dmachannels/#1f801088hn10h-d_chcr-dma-channel-control-channel-06-rw
	// 
	// Implementation note: DMA channel priority is not currently repected because DMA operations complete instantaneously so there is never any contention.
	//
	struct ControlRegister
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 MDECinPriority : 3;          // Bits 2:0   DMA0, MDECin  Priority      (0..7; 0=Highest, 7=Lowest)
				u32 MDECinMasterEnable : 1;      // Bit 3      DMA0, MDECin  Master Enable (0=Disable, 1=Enable)
				u32 MDECoutPriority : 3;         // Bits 6:4   DMA1, MDECout Priority      (0..7; 0=Highest, 7=Lowest)
				u32 MDECoutMasterEnable : 1;     // Bit 7      DMA1, MDECout Master Enable (0=Disable, 1=Enable)
				u32 GPUPriority : 3;             // Bits 10:8  DMA2, GPU     Priority      (0..7; 0=Highest, 7=Lowest)
				u32 GPUMasterEnable : 1;         // Bit 11     DMA2, GPU     Master Enable (0=Disable, 1=Enable)
				u32 CDROMPriority : 3;           // Bits 14:12 DMA3, CDROM   Priority      (0..7; 0=Highest, 7=Lowest)
				u32 CDROMMasterEnable : 1;       // Bit 15     DMA3, CDROM   Master Enable (0=Disable, 1=Enable)
				u32 SPUPriority : 3;             // Bits 18:16 DMA4, SPU     Priority      (0..7; 0=Highest, 7=Lowest)
				u32 SPUMasterEnable : 1;         // Bit 19     DMA4, SPU     Master Enable (0=Disable, 1=Enable)
				u32 PIOPriority : 3;             // Bits 22:20 DMA5, PIO     Priority      (0..7; 0=Highest, 7=Lowest)
				u32 PIOMasterEnable : 1;         // Bit 23     DMA5, PIO     Master Enable (0=Disable, 1=Enable)
				u32 OTCPriority : 3;             // Bits 26:24 DMA6, OTC     Priority      (0..7; 0=Highest, 7=Lowest)
				u32 OTCMasterEnable : 1;         // Bit 27     DMA6, OTC     Master Enable (0=Disable, 1=Enable)
				u32 CPUMemoryAccessPriority : 3; // Bits 30:28 CPU memory access priority  (0..7; 0=Highest, 7=Lowest)
				u32 CPUMemoryAccessEnable : 1;   // Bit 31     No effect, should be CPU memory access enable (R/W)
			};
		};
	};
	ControlRegister m_dpcr{0x07654321};

	// 1F8010F4h - DICR - DMA Interrupt Register (R/W)
	//
	//   Bits  Description
	//   0-6   Controls channel 0-6 completion interrupts in bits 24-30.
	//         When 0, an interrupt only occurs when the entire transfer completes.
	//         When 1, interrupts can occur for every slice and linked-list transfer.
	//         No effect if the interrupt is masked by bits 16-22.
	//   7-14  Unused
	//   15    Bus error flag. Raised when transferring to/from an address outside of RAM. Forces bit 31. (R/W)
	//   16-22 Channel 0-6 interrupt mask. If enabled, channels cause interrupts as per bits 0-6.
	//   23    Master channel interrupt enable.
	//   24-30 Channel 0-6 interrupt flags. (R, write 1 to reset)
	//   31    Master interrupt flag (R)
	//
	// #TODO: IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n)
	//
	struct InterruptRegister
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				// Bits 6:0
				// Controls channel 0-6 completion interrupts in bits 24-30.
				// When 0, an interrupt only occurs when the entire transfer completes.
				// When 1, interrupts can occur for every slice and linked-list transfer.
				// No effect if the interrupt is masked by bits 16-22.
				u32 channelCompletionBehaviour : 7;

				// Bits 14:7 unused
				u32 unusedBits14to7 : 8;

				// Bit 15 Bus error flag.
				// Raised when transferring to/from an address outside of RAM. Forces bit 31. (R/W)
				u32 busError : 1;

				// Bits 22:16 Channel 0-6 interrupt mask.
				// If enabled, channels cause interrupts as per bits 0-6.
				u32 channelInterruptMask : 7;

				// Bit 23 Master channel interrupt enable.
				u32 masterChannelInterruptEnable : 1;

				// Bits 30:24 Channel 0-6 interrupt flags.
				// (Read-only; write 1 to reset)
				u32 channelInterruptFlags : 7;

				// Bit 31 Master interrupt flag (R)
				u32 masterInterruptFlag : 1;
			};
		};
	};

	// DICR - DMA Interrupt Register (R/W)
	InterruptRegister m_dicr{};

	Channel m_channels[7]; // 7 DMA channels

	Stats m_stats;

	// DMA accesses memory directly by definition.
	// Deliberately do not use Bus interface or its getPhysicalAddress() helper here. Doing so would also create a cyclic dependency.
	RAM& m_ram;

	GPU& m_gpu;
	SPU& m_spu;
	CDROM& m_cdrom;
	MDEC& m_mdec;
	INTC& m_intc; // interrupt controller

	void writeDICR(u32 value, u32 pc);

	// Helpers to read memory, stripping segment bits
	u32 readU32(u32 va) const;
	void writeU32(u32 va, u32 val);

	void transferRAMtoMDEC(u32 pc); // DMA0 - MDECin (RAM to MDEC)
	void transferMDECtoRAM(u32 pc); // DMA1 - MDECout (MDEC to RAM)
	void gpuTransfer(u32 pc); // DMA2 - GPU
	void cdromTransfer(u32 pc); // DMA3 - CDROM to RAM
	void spuTransfer(u32 pc); // DMA4 - SPU
	void orderingTableClear(u32 pc); // DMA6 - OTC

	void updateMasterInterruptFlag();
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline const char* kDMAChannelNames[] =
{
	"MDECin",
	"MDECout",
	"GPU",
	"CDROM",
	"SPU",
	"PIO",
	"OTC",
};
