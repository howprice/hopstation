#include "DMA.h"

#include "CDROM.h"
#include "RAM.h"
#include "GPU.h"
#include "SPU.h"
#include "MDEC.h"
#include "INTC.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/ArrayHelpers.h"

static_assert(COUNTOF_ARRAY(kDMAChannelNames) == ENUM_COUNT(DMAChannel));

static const char* kTransferDirectionNames[] =
{
	"DeviceToRAM",
	"RAMToDevice",
};
static_assert(COUNTOF_ARRAY(kTransferDirectionNames) == ENUM_COUNT(DMAC::Channel::TransferDirection));

static const char* kSyncModeNameDesc[] =
{
	"Burst (OTC and CDROM)",
	"Slice (Block transfer mode. MDEC, SPU, and GPU-vram-data)",
	"LinkedList (GPU-command-lists)",
	"Reserved",
};
static_assert(COUNTOF_ARRAY(kSyncModeNameDesc) == ENUM_COUNT(DMAC::Channel::SyncMode));

// Memory mapped I/O
static constexpr u32 kDMARegistersStartAddress = 0x1F801080;
static constexpr u32 kDMARegistersEndAddress = 0x1F801100;
static constexpr unsigned int kDMAControllerRegisterCount = (kDMARegistersEndAddress - kDMARegistersStartAddress) / 4; // 4 bytes per register

// DMA channel base address (Channel 0..6) (R/W)
// D#_MADR
// Addresses: 1F801080h+N*10h
// Offsets:     0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60
// Reg indices: 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18

// DMA Block Control registers (Channel 0..6) (R/W)
// D#_BCR
// Addresses: 1F801084h+N*10h
// Offsets:     0x04, 0x14, 0x24, 0x34, 0x44, 0x54, 0x64
// Reg indices: 0x01, 0x05, 0x09, 0x0D, 0x11, 0x15, 0x19
// 
// DMA Channel Control registers (Channel 0..6) (R/W)
// D#_CHCR
// Addresses: 1F801088h+N*10h
// Offsets:     0x08, 0x18, 0x28, 0x38, 0x48, 0x58, 0x68
// Reg indices: 0x02, 0x06, 0x0A, 0x0E, 0x12, 0x16, 0x1A

// DPCR - DMA Control Register (R/W)
// Address: 1F8010F0h
// Offset: 70h
static constexpr u32 kDMAControlRegisterAddress = 0x1F8010F0;
static constexpr u32 kDMAControlRegIndex = (kDMAControlRegisterAddress - kDMARegistersStartAddress) / 4; // 0x1C, 28

// DICR - DMA Interrupt Register (R/W)
// Address: 1F8010F4h
// Offset: 74h
static constexpr u32 kDMAInterruptRegisterAddress = 0x1F8010F4; 
static constexpr u32 kDMAInterruptRegIndex = (kDMAInterruptRegisterAddress - kDMARegistersStartAddress) / 4; // 0x1D, 29
static constexpr u32 kDICROffset = (0x1F8010F4 - kDMARegistersStartAddress); // 0x74

// Unknown register
// Address: 1F8010F8h
// Offset: 78h
// Usually 7FFAC68Bh? or 0BFAC688h)
// https://psx-spx.consoledev.net/dmachannels/#1f8010f8h-usually-7ffac68bh-or-0bfac688h
static constexpr u32 kDMAUnknownRegister1Index = (0x1F8010F8 - kDMARegistersStartAddress) / 4; // 0x1E, 30

// Unknown register
// Address: 1F8010FCh
// Offset: 7Ch
// Usually 00FFFFF7h) (...maybe OTC fill-value)
// https://psx-spx.consoledev.net/dmachannels/#1f8010fch-usually-00fffff7h-maybe-otc-fill-value
static constexpr u32 kDMAUnknownRegister2Index = (0x1F8010FC - kDMARegistersStartAddress) / 4; // 0x1F, 31

// n.b. Deliberately do not use Bus interface or its getPhysicalAddress() helper here; DMA accesses memory directly by definition.
static inline u32 getPhysicalAddress(u32 va)
{
	u32 pa = va & 0x1fffffff; // strip segment bits

	// PSX RAM is 2 MB = 0x20'0000. Mirrored up to 8 MB 0x80'0000
	// This is required for Tomb Raider V1.1
	HP_DEBUG_ASSERT(pa < 0x80'0000);
	pa &= 0x1f'ffff;
	return pa;
}

void DMAC::Reset()
{
	// Control register initial value on reset is 07654321h
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f0h-dpcr-dma-control-register-rw
	m_dpcr.val = 0x07654321;

	m_dicr.val = 0;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_channels); i++)
	{
		m_channels[i] = {};
	}

	m_stats = {};
}

u32 DMAC::ReadReg(unsigned int regIndex, u32 pc)
{
	HP_DEBUG_ASSERT(regIndex < kDMAControllerRegisterCount);

	if (regIndex == kDMAControlRegIndex) // 0x1C, 28
	{
		if (s_logDMARegisterAccess)
			LOG_INFO("[DMA] DPCR control register read value %08X PC: %08X\n", m_dpcr.val, pc);
		return m_dpcr.val;
	}
	else if (regIndex == kDMAInterruptRegIndex) // 0x1D, 29
	{
		if (s_logDMARegisterAccess)
			LOG_INFO("[DMA] DICR interrupt register read value %08X PC: %08X\n", m_dicr.val, pc);
		return m_dicr.val;
	}
	else if (regIndex == kDMAUnknownRegister1Index) // 0x1E, 30
	{
		HP_DEBUG_FATAL_ERROR("[DMA] Unknown register 30 read address %08X\n", kDMARegistersStartAddress + (4 * kDMAUnknownRegister1Index));

		// #TODO: See https://psx-spx.consoledev.net/dmachannels/#1f8010f8h-usually-7ffac68bh-or-0bfac688h
		return 0;
	}
	else if (regIndex == kDMAUnknownRegister2Index) // 0x1F, 31
	{
		HP_DEBUG_FATAL_ERROR("[DMA] Unknown register 31 read address %08X\n", kDMARegistersStartAddress + (4 * kDMAUnknownRegister2Index));

		// #TODO: See https://psx-spx.consoledev.net/dmachannels/#1f8010fch-usually-00fffff7h-maybe-otc-fill-value
		return 0;
	}
	else // channel registers
	{
		unsigned int channelIndex = regIndex >> 2; // 4 registers per channel, [0,6]
		if (channelIndex > 6)
		{
			HP_DEBUG_FATAL_ERROR("[DMA] Read invalid channel %u address %08X\n", channelIndex, kDMARegistersStartAddress + (4 * regIndex));
			return 0;
		}

		unsigned int field = regIndex & 0x03; // register within channel, [0,3]
		if (field == 0) // channel base address (MADR)
		{
			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA%u (%s) D%u_MADR read value %08X PC: %08X\n", channelIndex, kDMAChannelNames[channelIndex], channelIndex, m_channels[channelIndex].madr, pc);
			return m_channels[channelIndex].madr;
		}
		else if (field == 1) // Block Control registers (BCR)
		{
			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA%u (%s) D%u_BCR read value %08X PC: %08X\n", channelIndex, kDMAChannelNames[channelIndex], channelIndex, m_channels[channelIndex].bcr, pc);
			return m_channels[channelIndex].bcr;
		}
		else if (field == 2) // Channel Control registers (CHCR)
		{
			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA%u (%s) D%u_CHCR read value %08X PC: %08X\n", channelIndex, kDMAChannelNames[channelIndex], channelIndex, m_channels[channelIndex].chcr.val, pc);
			return m_channels[channelIndex].chcr.val;
		}
	}

	HP_DEBUG_FATAL_ERROR("Read from invalid DMA register %u address %08X\n", regIndex, kDMARegistersStartAddress + (4 * regIndex));
	return 0;
}

void DMAC::WriteReg(unsigned int regIndex, u32 val, u32 pc)
{
	HP_DEBUG_ASSERT(regIndex < kDMAControllerRegisterCount);

	if (regIndex == kDMAControlRegIndex) // 0x1C, 28
	{
		m_dpcr.val = val;

		if (s_logDMARegisterAccess)
			LOG_INFO(
				"[DMA] DPCR control register write value %08X PC: %08X\n"
				"[DMA]   MDECin   Priority: %u  Enable: %u\n"
				"[DMA]   MDECout  Priority: %u  Enable: %u\n"
				"[DMA]   GPU      Priority: %u  Enable: %u\n"
				"[DMA]   CDROM    Priority: %u  Enable: %u\n"
				"[DMA]   SPU      Priority: %u  Enable: %u\n"
				"[DMA]   PIO      Priority: %u  Enable: %u\n"
				"[DMA]   OTC      Priority: %u  Enable: %u\n"
				"[DMA]   CPU      Priority: %u  Enable: %u\n",
				val, pc,
				m_dpcr.MDECinPriority, m_dpcr.MDECinMasterEnable,
				m_dpcr.MDECoutPriority, m_dpcr.MDECoutMasterEnable,
				m_dpcr.GPUPriority, m_dpcr.GPUMasterEnable,
				m_dpcr.CDROMPriority, m_dpcr.CDROMMasterEnable,
				m_dpcr.SPUPriority, m_dpcr.SPUMasterEnable,
				m_dpcr.PIOPriority, m_dpcr.PIOMasterEnable,
				m_dpcr.OTCPriority, m_dpcr.OTCMasterEnable,
				m_dpcr.CPUMemoryAccessPriority, m_dpcr.CPUMemoryAccessEnable);

		// If any channels are enabled and have an outstanding DMA request then they should start immediately.
		// This is to support a write to Dx_CHCR to start DMA while channel disabled, then write to DPCR to enable channel and start DMA immediately without needing to write to Dx_CHCR again.
		if (m_dpcr.MDECinMasterEnable && m_channels[(int)DMAChannel::MDECin].chcr.startTransfer)
			transferRAMtoMDEC(pc);

		if (m_dpcr.MDECoutMasterEnable && m_channels[(int)DMAChannel::MDECout].chcr.startTransfer)
			transferMDECtoRAM(pc);

		if (m_dpcr.GPUMasterEnable && m_channels[(int)DMAChannel::GPU].chcr.startTransfer)
			gpuTransfer(pc);

		if (m_dpcr.CDROMMasterEnable && m_channels[(int)DMAChannel::CDROM].chcr.startTransfer)
			cdromTransfer(pc);

		if (m_dpcr.SPUMasterEnable && m_channels[(int)DMAChannel::SPU].chcr.startTransfer)
			spuTransfer(pc);

		if (m_dpcr.PIOMasterEnable && m_channels[(int)DMAChannel::PIO].chcr.startTransfer)
		{
			HP_FATAL_ERROR("DMA5 PIO not implemented");
		}

		if (m_dpcr.OTCMasterEnable && m_channels[(int)DMAChannel::OTC].chcr.startTransfer)
			orderingTableClear(pc);

		return;
	}
	else if (regIndex == kDMAInterruptRegIndex) // 0x1D, 29
	{
		writeDICR(val, pc);
		return;
	}
	else if (regIndex == kDMAUnknownRegister1Index) // 0x1E, 30
	{
		HP_DEBUG_FATAL_ERROR("Write to unknown DMA register 30 address %08X value %08X\n", kDMARegistersStartAddress + (regIndex * 4), val);

		// #TODO: See https://psx-spx.consoledev.net/dmachannels/#1f8010f8h-usually-7ffac68bh-or-0bfac688h
		return;
	}
	else if (regIndex == kDMAUnknownRegister2Index) // 0x1F, 31
	{
		HP_DEBUG_FATAL_ERROR("Write to unknown DMA register 31 address %08X value %08X\n", kDMARegistersStartAddress + (regIndex * 4), val);

		// #TODO: See https://psx-spx.consoledev.net/dmachannels/#1f8010fch-usually-00fffff7h-maybe-otc-fill-value
		return;
	}
	else // channel registers
	{
		unsigned int channelIndex = regIndex >> 2; // 4 registers per channel, [0,6]
		if (channelIndex > 6)
		{
			HP_DEBUG_FATAL_ERROR("Write to invalid DMA channel %u address %08X value %08X\n", channelIndex, kDMARegistersStartAddress + (4 * regIndex), val);
			return;
		}

		unsigned int field = regIndex & 0x03; // register within channel, [0,3]
		if (field == 0) // channel base address (MADR)
		{
			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA%u (%s) D%u_MADR write value %08X PC: %08X\n", channelIndex, kDMAChannelNames[channelIndex], channelIndex, val, pc);
			m_channels[channelIndex].madr = val;
			return;
		}
		else if (field == 1) // Block Control registers (BCR)
		{
			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA%u (%s) D%u_BCR write value %08X: %04X words, %04X blocks PC: %08X\n", channelIndex, kDMAChannelNames[channelIndex], channelIndex, val, val & 0xffff, val >> 16, pc);
			m_channels[channelIndex].bcr = val;
			return;
		}
		else if (field == 2) // Channel Control registers (CHCR)
		{
			Channel::ControlRegister& chcr = m_channels[channelIndex].chcr;
			chcr.val = val;

			if (s_logDMARegisterAccess || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO(
					"[DMA] DMA%u (%s) D%u_CHCR write value %08X PC: %08X\n"
					"[DMA]   Direction: %u %s\n"
					"[DMA]   MADR increment: %u = %s\n"
					"[DMA]   Chopping/header enable: %u %s\n"
					"[DMA]   Sync mode: %u %s\n"
					"[DMA]   Chopping DMA Window Size: %u = %u words\n"
					"[DMA]   Chopping CPU Window Size: %u = %u cycles\n"
					"[DMA]   Start transfer: %u\n"
					"[DMA]   Force start without DREQ: %u\n"
					"[DMA]   Keep transfer active: %u\n"
					"[DMA]   Snoop bus: %u\n",
					channelIndex, kDMAChannelNames[channelIndex], channelIndex, val, pc,
					(int)chcr.transferDirection, kTransferDirectionNames[(int)chcr.transferDirection],
					chcr.addressIncrement, chcr.addressIncrement == Channel::AddressIncrement::Increment ? "+4" : "-4",
					chcr.choppingOrHeaderEnable, chcr.choppingOrHeaderEnable == 0 ? "" : "???",
					(int)chcr.syncMode, kSyncModeNameDesc[(int)chcr.syncMode],
					chcr.choppingDMAWindowSize, 1 << chcr.choppingDMAWindowSize,
					chcr.choppingCPUWindowSize, 1 << chcr.choppingCPUWindowSize,
					chcr.startTransfer,
					chcr.startTrigger,
					chcr.keepTransferActive,
					chcr.busSnoopingEnable);

			// Some bits are hardwired to 0 and 1 for DMA6 OTC
			if (channelIndex == 6)
			{
				// Bits cleared: 0, 8,9,10, 16, 17, 18, 20,21,22, 25
				// See Jakub/otc-test/testOtcWhichBitsAreHardwiredToZero
				chcr.transferDirection = Channel::TransferDirection::DeviceToRAM; // bit 0
				chcr.choppingOrHeaderEnable = 0; // bit 8
				chcr.syncMode = Channel::SyncMode::Burst; // bits 10:9
				chcr.choppingDMAWindowSize = 0; // bits 18:16
				chcr.choppingCPUWindowSize = 0; // bits 22:20
				chcr.keepTransferActive = 0; // bit 29

				// Bits set: 1
				// See Jakub/otc-test/testOtcWhichBitsAreHardwiredToOne
				chcr.addressIncrement = Channel::AddressIncrement::Decrement; // bit 1 <- 1

				// Unused bits are always zero on read
				// 0b10001110'10001000'11111000'11111100
				// bits 31 27:25  23 19 15::11, 7:2,
				chcr.val &= ~0b10001110'10001000'11111000'11111100;
			}

			if (chcr.startTransfer)
			{
				// #TODO: Should chcr.forceTransferStart be respected here? What should the behaviour be when CHCR bit 28 forcestart is not set? Perhaps no difference for immediate DMA implementation.

				switch (channelIndex)
				{
					case 0:// DMA0 - MDECin (RAM to MDEC)
					{
						if (m_dpcr.MDECinMasterEnable)
							transferRAMtoMDEC(pc);
						break;
					}

					case 1:// DMA1 - MDECout (MDEC to RAM)
					{
						if (m_dpcr.MDECoutMasterEnable)
							transferMDECtoRAM(pc);
						break;
					}

					case 2:// DMA2 - GPU
					{
						if (m_dpcr.GPUMasterEnable)
							gpuTransfer(pc);
						break;
					}

					case 3:// DMA3 - CDROM (CDROM to RAM)
					{
						if (m_dpcr.CDROMMasterEnable)
							cdromTransfer(pc);
						break;
					}

					case 4:// DMA4 - SPU
					{
						if (m_dpcr.SPUMasterEnable)
							spuTransfer(pc);
						break;
					}

					case 6: // DMA6 - OTC (Ordering Table Clear)
					{
						// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#initializing-an-empty-ot-via-dma6
						if (m_dpcr.OTCMasterEnable)
							orderingTableClear(pc);
						break;
					}

					default:
						HP_DEBUG_FATAL_ERROR("DMA%u transfer not implemented", channelIndex);
						break;
				}
			}

			return;
		}
	}

	HP_DEBUG_FATAL_ERROR("Write to invalid DMA register %u address %08X value %08X PC: %08X", regIndex, kDMARegistersStartAddress + (4 * regIndex), val, pc);
}

u8 DMAC::Read8(unsigned int offset, u32 pc)
{
	// DICR is 1F8010F4h (offset 0x74) and MMX4 reads a byte from 0x76 during MDEC decoding.
	if (offset == kDICROffset + 2)
	{
		u8 val = (m_dicr.val >> 16) & 0xff; // little-endian
		if (s_logDMARegisterAccess)
			LOG_INFO("[DMA] DICR interrupt register 8-bit read value %02X (%08X) PC: %08X\n", val, m_dicr.val, pc);
		return val;
	}
	else
	{
		HP_FATAL_ERROR("Unhandled DMA 8-bit read from offset %02X", offset);
		return 0;
	}
}

void DMAC::Write8(unsigned int offset, u8 val, u32 pc)
{
	// DICR is 1F8010F4h (offset 0x74) and MMX4 writes a byte to 0x76 during MDEC decoding.
	if (offset == kDICROffset + 2)
	{
		writeDICR((u32)val << 16, pc); // little-endian
	}
	else
	{
		HP_FATAL_ERROR("Unhandled DMA 8-bit write from offset %02X", offset);
	}
}

//
// DMA0 - MDECin (RAM to MDEC)
//
void DMAC::transferRAMtoMDEC(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::MDECin; // DMA0
	Channel& channel = m_channels[channelIndex];
	Channel::ControlRegister& chcr = channel.chcr;

	// CHCR bit 28 is cleared when transfer starts
	chcr.startTrigger = 0;

	switch (chcr.syncMode)
	{
		case Channel::SyncMode::Burst:
		{
			HP_FATAL_ERROR("Don't expect sync mode 0 (burst) DMA to be used for DMA0 MDECin");
			break;
		}

		case Channel::SyncMode::Slice: // aka block transfer mode
		{
			//   0-15  BS    Blocksize (words) ;for GPU/SPU max 10h, for MDEC max 20h
			//   16-31 BA    Amount of blocks  ;ie. total length = BS*BA words
			// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
			unsigned int blockSizeWords = channel.bcr & 0xffff;
			if (blockSizeWords == 0)
				blockSizeWords = 0x10000;

			HP_DEBUG_ASSERT(blockSizeWords <= 0x20, "#TODO: Should MDECin BS be clamped to 0x20 ?");
			unsigned int blockCount = channel.bcr >> 16;
			if (blockCount == 0)
				blockCount = 0x10000;

			unsigned int wordCount = blockSizeWords * blockCount;

			if (s_logDMA || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA0 (MDECin) block transfer MADR %08X, block size %X words (hex) * block count %X (hex) = total %X words (hex) PC: %08X\n", channel.madr, blockSizeWords, blockCount, wordCount, pc);

			// Transfer all the data; DMAC currently execute transfers/commands immediately and to completion, so no need to batch up for FIFO.

			u32 address = channel.madr;
			u32 deltaAddress = channel.chcr.addressIncrement == Channel::AddressIncrement::Increment ? 4 : -4;

			HP_ASSERT(chcr.transferDirection == Channel::TransferDirection::RAMToDevice, "Expect DMA0 to be used to transfer from RAM to MDEC (MDEC to RAM is DMA1)");

			// Implementation note: Currently DMA is performed immediately rather than running in real time.
			// So instead of DMAing data word by word we can copy in a single operation if enough data is available.
			HP_ASSERT(channel.chcr.addressIncrement == Channel::AddressIncrement::Increment, "Current implementation only supports address increment"); // #TODO: Support decrement mode, but that is not expected to be used for MDECin
			u32 pa = getPhysicalAddress(address);
			HP_ASSERT(pa < m_ram.GetSizeBytes());
			unsigned int byteCount = 4 * wordCount;
			unsigned int bytesAvailable = m_ram.GetSizeBytes() - pa;
			if (byteCount <= bytesAvailable)
			{
				// block copy
				m_mdec.WriteDataBlock(m_ram.GetData() + pa, wordCount);
				address += byteCount; // update address to end of transfer for write back to MADR
			}
			else // copy word by word
			{
				HP_FATAL_ERROR("Not tested");
				// #TODO: Some games such as Doom and Wipeout seem to set BA to 0 meaning 0x10000.
				// For slice mode on real hardware this would transfer a block at a time ongoing I think, but for immediate DMA this results in byteCount == 0x80'0000
				// which results  in this path being taken.
				// #TODO: May make more sense to write block by block instead i.e. 0x20 words at a time.
				for (unsigned int i = 0; i < wordCount; i++)
				{
					u32 value = readU32(address);
					m_mdec.WriteMDEC0(value, /*pc*/0);
					address += deltaAddress;
				}
			}

			// In SyncMode=1 and SyncMode=2, the hardware does update MADR.
			// It will contain the start address of the currently transferred block; at transfer end, it'll hold the end-address in SyncMode=1, or the end marker in SyncMode=2)
			// Address bits 0-1 are writeable, but any updated current/end addresses are word-aligned with bits 0-1 forced to zero.
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			channel.madr = address;
			channel.madr &= ~3; // word-align

			// SyncMode=1 decrements BA to zero
			channel.bcr &= 0x0000ffff;

			break;
		}
		case Channel::SyncMode::LinkedList:
		{
			HP_FATAL_ERROR("Don't expect sync mode 2 (linked list) DMA to be used for MDECin DMA");
			break;
		}
		case Channel::SyncMode::Reserved:
			HP_DEBUG_FATAL_ERROR("MDECin DMA with reserved sync mode 3");
			break;
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	constexpr u32 channelFlag = 1 << channelIndex;
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

//
// DMA1 - MDECout (MDEC to RAM)
//
// The psx-spx "spec" seems to imply that decompressed pixel transferred from MDEC to RAM via DMA1 needs to be
// reordered from 16x16 to 8x8. However, this doesn't seems to be the case.
// See https://psx-spx.consoledev.net/macroblockdecodermdec/#1f801820hread-mdec-dataresponse-register-r
//
void DMAC::transferMDECtoRAM(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::MDECout; // DMA1
	Channel& channel = m_channels[channelIndex];
	Channel::ControlRegister& chcr = channel.chcr;

	// CHCR bit 28 is cleared when transfer starts
	chcr.startTrigger = 0;

	switch (chcr.syncMode)
	{
		case Channel::SyncMode::Burst:
		{
			HP_FATAL_ERROR("Don't expect sync mode 0 (burst) DMA to be used for DMA1 MDECout");
			break;
		}

		case Channel::SyncMode::Slice: // aka block transfer mode
		{
			//   0-15  BS    Blocksize (words) ;for GPU/SPU max 10h, for MDEC max 20h
			//   16-31 BA    Amount of blocks  ;ie. total length = BS*BA words
			// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
			unsigned int blockSizeWords = channel.bcr & 0xffff;
			if (blockSizeWords == 0)
				blockSizeWords = 0x10000;

			HP_DEBUG_ASSERT(blockSizeWords <= 0x20, "#TODO: Should MDECout BS be clamped to 0x20 ?");
			unsigned int blockCount = channel.bcr >> 16;
			if (blockCount == 0)
				blockCount = 0x10000;

			unsigned int wordCount = blockSizeWords * blockCount;

			if (s_logDMA || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA1 (MDECout) block transfer MADR %08X, block size %X words (hex) * block count %X (hex) = total %X words (hex) PC: %08X\n", channel.madr, blockSizeWords, blockCount, wordCount, pc);

			// Transfer all the data; DMAC currently execute transfers/commands immediately and to completion, so no need to batch up for FIFO.

			u32 address = channel.madr;
			u32 deltaAddress = channel.chcr.addressIncrement == Channel::AddressIncrement::Increment ? 4 : -4;

			HP_ASSERT(chcr.transferDirection == Channel::TransferDirection::DeviceToRAM, "Expect DMA1 to be used to transfer from MDEC to RAM (RAM to MDEC is DMA0)");

			// Implementation note: Currently DMA is performed immediately rather than running in real time.
			// So instead of DMAing data word by word we can copy in a single operation if enough data is available.
			HP_ASSERT(channel.chcr.addressIncrement == Channel::AddressIncrement::Increment, "Current implementation only supports address increment"); // #TODO: Support decrement mode, but that is not expected to be used for MDECin
			u32 pa = getPhysicalAddress(address);
			HP_ASSERT(pa < m_ram.GetSizeBytes());
			unsigned int byteCount = 4 * wordCount;
			unsigned int ramRangeAvailable = m_ram.GetSizeBytes() - pa;
			if (byteCount <= ramRangeAvailable)
			{
				// Read from MDEC memory directly into RAM.
				m_mdec.ReadDataBlock(m_ram.GetData() + pa, byteCount);
				address += byteCount; // update address to end of transfer for write back to MADR
			}
			else // copy word by word
			{
				HP_FATAL_ERROR("Not tested");
				for (unsigned int i = 0; i < wordCount; i++)
				{
					u32 value = m_mdec.ReadMDEC0(); // Read MDEC0 Data/Response Register
					writeU32(address, value);
					address += deltaAddress;
				}
			}

			// In SyncMode=1 and SyncMode=2, the hardware does update MADR.
			// It will contain the start address of the currently transferred block; at transfer end, it'll hold the end-address in SyncMode=1, or the end marker in SyncMode=2)
			// Address bits 0-1 are writeable, but any updated current/end addresses are word-aligned with bits 0-1 forced to zero.
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			channel.madr = address;
			channel.madr &= ~3; // word-align

			// SyncMode=1 decrements BA to zero
			channel.bcr &= 0x0000ffff;

			break;
		}
		case Channel::SyncMode::LinkedList:
		{
			HP_FATAL_ERROR("Don't expect sync mode 2 (linked list) DMA to be used for MDECout DMA");
			break;
		}
		case Channel::SyncMode::Reserved:
			HP_DEBUG_FATAL_ERROR("MDECout DMA with reserved sync mode 3");
			break;
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	constexpr u32 channelFlag = 1 << channelIndex;
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

// DMA2 - GPU
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#sending-the-ot-to-the-gpu-via-dma2-linked-list-mode
//
void DMAC::gpuTransfer(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::GPU; // DMA2
	Channel& channel = m_channels[channelIndex]; 
	Channel::ControlRegister& chcr = channel.chcr;

	// CHCR bit 28 is cleared when transfer starts
	// #TODO: Move this into individual transfer implementations below?
	chcr.startTrigger = 0;

	switch (chcr.syncMode)
	{
		case Channel::SyncMode::Burst:
		{
			HP_FATAL_ERROR("Not implemented");
			// #TODO: Docs seem to indicate that burst mode would only be used for OTC and CDROM https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
			// #TODO: Transfer all the data

			// MADR is not updated in Burst SyncMode (0); it will contain the start address even during and after the transfer.
			// Unless Chopping is enabled, in that case it does update MADR, same does probably also happen when getting interrupted by a higher priority DMA channel).
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw

			break;
		}

		case Channel::SyncMode::Slice: // aka block transfer mode
		{
			//   0-15  BS    Blocksize (words) ;for GPU/SPU max 10h, for MDEC max 20h
			//   16-31 BA    Amount of blocks  ;ie. total length = BS*BA words
			// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
			unsigned int blockSizeWords = channel.bcr & 0xffff;
			if (blockSizeWords == 0)
				blockSizeWords = 0x10000;

			HP_DEBUG_ASSERT(blockSizeWords <= 0x10, "#TODO: Should this be clamped to 0x10 ?");
			unsigned int blockCount = channel.bcr >> 16;
			if (blockCount == 0)
				blockCount = 0x10000;

			// Calculate block size.
			// - https://jsgroth.dev/blog/posts/ps1-diamond/#block
			unsigned int wordCount = blockSizeWords * blockCount;

			if (s_logDMA || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA2 (GPU) block transfer MADR %08X, block size %X words (hex) * block count %X (hex) = total %X words (hex) PC: %08X\n", channel.madr, blockSizeWords, blockCount, wordCount, pc);

			// Transfer all the data; both DMAC and GPU currently execute transfers/commands immediately and to completion, so no need to batch up for FIFO.

			u32 address = channel.madr;
			u32 deltaAddress = channel.chcr.addressIncrement == Channel::AddressIncrement::Increment ? 4 : -4;

			if (chcr.transferDirection == Channel::TransferDirection::RAMToDevice)
			{
				for (unsigned int i = 0; i < wordCount; i++)
				{
					u32 commandWord = readU32(address);
					m_gpu.WriteGP0(commandWord);
					address += deltaAddress;
				}
			}
			else // chcr.transferDirection == Channel::TransferDirection::DeviceToRAM
			{
				for (unsigned int i = 0; i < wordCount; i++)
				{
					u32 value = m_gpu.GetGPUREAD();
					writeU32(address, value);
					address += deltaAddress;
				}
			}

			// In SyncMode=1 and SyncMode=2, the hardware does update MADR.
			// It will contain the start address of the currently transferred block; at transfer end, it'll hold the end-address in SyncMode=1, or the end marker in SyncMode=2)
			// Address bits 0-1 are writeable, but any updated current/end addresses are word-aligned with bits 0-1 forced to zero.
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			channel.madr = address;
			channel.madr &= ~3; // word-align

			// SyncMode=1 decrements BA to zero
			channel.bcr &= 0x0000ffff;

			break;
		}
		case Channel::SyncMode::LinkedList:
		{
			// https://jsgroth.dev/blog/posts/ps1-diamond/#linked-list

			HP_DEBUG_ASSERT(chcr.transferDirection == Channel::TransferDirection::RAMToDevice, "Only expect linked list mode to be used on PSX to copy commands from RAM to GPU");

			u32 nodeAddress = channel.madr; // pointer to *last* table entry

			HP_DEBUG_ASSERT((nodeAddress & 3) == 0, "Node address not word aligned."); // #TODO: Think lower 3 bits should be masked off.

			const u32 startNodeAddress = nodeAddress;

			HP_DEBUG_ASSERT(chcr.addressIncrement == Channel::AddressIncrement::Increment); // #TODO: Perhaps Decrement is valid and delta address could be -4 ?!?

			// Prevent infinite loop if circular linked list detection fails.
			unsigned int maxLoops = 0x10000;
			while (maxLoops)
			{
				u32 header = readU32(nodeAddress);
				unsigned int dataWordCount = header >> 24; // upper byte contains number of GPU command data words in the node, which follow the header

				u32 dataAddress = nodeAddress + 4;
				for (unsigned int i = 0; i < dataWordCount; i++)
				{
					u32 commandWord = readU32(dataAddress);
					m_gpu.WriteGP0(commandWord);

					dataAddress += 4;
				}

				u32 nextNodeAddress = header & 0xfffffc; /// bits 23:0 are the pointer to next node n.b. Mask off lower two bits to keep address word-aligned (fixes Tekken 2)
				if (nextNodeAddress & (1 << 23)) // Spec states that last node pointer is 0xffffff, but apparently some silicon check bit 23 only.
					break;

				// Check for "inifinite" (circular?) linked lists in DMA2 linked-list transfer to GPU (depth ordering table).
				// https://jsgroth.dev/blog/posts/ps1-diamond/#linked-list mention that Tekken 2 and Syphon Filter use this technique.
				if (nextNodeAddress == startNodeAddress || nextNodeAddress == nodeAddress)
				{
					//HP_DEBUG_FATAL_ERROR("Circular linked list.");
					LOG_WARN("[DMA] DMA2 - GPU circular linked list\n");
					break;
				}

				nodeAddress = nextNodeAddress;
				maxLoops--;
			}

			HP_DEBUG_ASSERT(maxLoops > 0, "DMA2 - GPU circular linked list detection failed");

			// In SyncMode=1 and SyncMode=2, the hardware does update MADR.
			// It will contain the start address of the currently transferred block; at transfer end, it'll hold the end-address in SyncMode=1, or the end marker in SyncMode=2)
			// Address bits 0-1 are writeable, but any updated current/end addresses are word-aligned with bits 0-1 forced to zero.
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			// 
			// On real hardware, this register would be updated as the transfer proceeded in real-time.
			// 
			channel.madr = nodeAddress & ~3; // word-align
			break;
		}
		case Channel::SyncMode::Reserved:
			HP_DEBUG_FATAL_ERROR("GPU DMA with reserved sync mode 3");
			break;
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	constexpr u32 channelFlag = 1 << channelIndex;
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

// DMA3 - CDROM to RAM
//
void DMAC::cdromTransfer(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::CDROM; // DMA3
	Channel& channel = m_channels[channelIndex];
	Channel::ControlRegister& chcr = channel.chcr;
	constexpr u32 channelFlag = 1u << channelIndex;

	// CHCR bit 28 is cleared when transfer starts
	// #TODO: Move this into individual transfer implementations below?
	chcr.startTrigger = 0;

	switch (chcr.syncMode)
	{
		case Channel::SyncMode::Burst:
		{
			// 0-15  BC    Number of words (0001h..FFFFh) (or 0=10000h words)
			// 16-31 0     Not used (usually 0 for OTC, or 1 ("one block") for CDROM)
			// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw

			unsigned int blockSizeWords = channel.bcr & 0xffff;
			if (blockSizeWords == 0)
				blockSizeWords = 0x10000;

			// The block count field is not respected by the hardware for CDROM DMA.
//			unsigned int blockCount = channel.bcr >> 16;
			unsigned int wordCount = blockSizeWords;

			if (s_logDMA || (s_logDMAChannelBits & channelFlag))
				LOG_INFO("[DMA] DMA3 block transfer from CDROM to MADR %08X, block size %X words (hex) PC: %08X Sector data buffer contains: %s\n",
					channel.madr, blockSizeWords, pc, m_cdrom.DebugGetSectorDataBufferDescription());

			// Transfer all the data
			u32 address = channel.madr;
			u32 deltaAddress = channel.chcr.addressIncrement == Channel::AddressIncrement::Increment ? 4 : -4;

			HP_ASSERT(chcr.transferDirection == Channel::TransferDirection::DeviceToRAM, "Only expect to transfer from CDROM to RAM, never RAM to CDROM");

			// Implementation note: Currently DMA is performed immediately rather than running in real time.
			// So instead of DMAing data word by word we can copy in a single operation if enough data is available.
			unsigned int wordsAvailable = m_cdrom.GetSectorDataSizeBytes() / 4;
			unsigned int byteCount = wordCount * 4;
			u32 pa = getPhysicalAddress(address);
			if (wordsAvailable >= wordCount && pa + byteCount <= m_ram.GetSizeBytes())
			{
				// Copy directly from CDROM into RAM
				m_cdrom.ReadDataBlock(m_ram.GetData() + pa, byteCount);
				address += byteCount; // update address to end of transfer in case is written back to MADR
			}
			else
			{
				// Copy word by word
				for (unsigned int i = 0; i < wordCount; i++)
				{
					u32 value = m_cdrom.ReadRDDATA();
					writeU32(address, value);
					address += deltaAddress;
				}
			}

			// MADR is not updated in Burst SyncMode (0); it will contain the start address even during and after the transfer.
			// Unless Chopping is enabled, in that case it does update MADR, same does probably also happen when getting interrupted by a higher priority DMA channel).
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			if (chcr.choppingOrHeaderEnable)
			{
				// HP_FATAL_ERROR("Not implemented");
				// #TODO: Update MADR
				// #TODO: SyncMode = 0 with chopping enabled decrements BC to zero (aside from that two cases, D#_BCR isn't changed during/after transfer).
				channel.madr = address; // #TODO: Check not off by one. Should this be the address of the last word copied or one past?
			}

			break;
		}

		case Channel::SyncMode::Slice: // aka block transfer mode
		{
			HP_DEBUG_FATAL_ERROR("Don't expect CDROM DMA to use sync mode 1 (slice aka block transfer)");
			break;
		}
		case Channel::SyncMode::LinkedList:
		{
			HP_DEBUG_FATAL_ERROR("Don't expect CDROM DMA to use sync mode 2 (linked-list)");
			break;
		}
		case Channel::SyncMode::Reserved:
			HP_DEBUG_FATAL_ERROR("CDROM DMA with reserved sync mode 3");
			break;
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		if (s_logDMA || (s_logDMAChannelBits & channelFlag))
			LOG_INFO("[DMA] DMA3 DICR interrupt flag set\n");

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

// DMA4 - SPU
void DMAC::spuTransfer(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::SPU; // DMA4
	Channel& channel = m_channels[channelIndex];
	Channel::ControlRegister& chcr = channel.chcr;

	// Fake SPU DMA for now. Don't copy any data, just act as if have done. This may allow MK2 loading to progress.

	// CHCR bit 28 is cleared when transfer starts
	chcr.startTrigger = 0;

	switch (chcr.syncMode)
	{
		case Channel::SyncMode::Burst:
		{
			HP_FATAL_ERROR("Don't expect sync mode 0 (burst) DMA to be used for SPU");
			break;
		}

		case Channel::SyncMode::Slice: // aka block transfer mode
		{
			//   0-15  BS    Blocksize (words) ;for GPU/SPU max 10h, for MDEC max 20h
			//   16-31 BA    Amount of blocks  ;ie. total length = BS*BA words
			// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
			unsigned int blockSizeWords = channel.bcr & 0xffff;
			if (blockSizeWords == 0)
				blockSizeWords = 0x10000;

			HP_DEBUG_ASSERT(blockSizeWords <= 0x10, "#TODO: Should this be clamped to 0x10 ?");
			unsigned int blockCount = channel.bcr >> 16;
			if (blockCount == 0)
				blockCount = 0x10000;

			// Calculate block size.
			// - https://jsgroth.dev/blog/posts/ps1-diamond/#block
			unsigned int wordCount = blockSizeWords * blockCount;

			if (s_logDMA || (s_logDMAChannelBits & (1u << channelIndex)))
				LOG_INFO("[DMA] DMA4 (SPU) block transfer MADR %08X, block size %X words (hex) * block count %X (hex) = total %X words (hex) PC: %08X\n", channel.madr, blockSizeWords, blockCount, wordCount, pc);

			// Implementation note: Currently DMA is performed immediately rather than running in real time.
			// So instead of DMAing data word by word we can copy in a single operation if enough data is available.
			u32 address = channel.madr;
			u32 pa = getPhysicalAddress(address);
			HP_ASSERT(pa < m_ram.GetSizeBytes());
			unsigned int byteCount = 4 * wordCount;

			if (chcr.transferDirection == Channel::TransferDirection::RAMToDevice)
			{
				HP_ASSERT(channel.chcr.addressIncrement == Channel::AddressIncrement::Increment, "Current implementation only supports address increment"); // #TODO: Support decrement mode if required
				unsigned int bytesAvailable = m_ram.GetSizeBytes() - pa;
				if (byteCount <= bytesAvailable)
				{
					// block copy
					m_spu.WriteDataBlock(m_ram.GetData() + pa, wordCount);
					address += byteCount; // update address to end of transfer
				}
				else // copy word by word
				{
					HP_FATAL_ERROR("Not tested");
					for (unsigned int i = 0; i < wordCount; i++)
					{
						u32 value = readU32(address);
						m_spu.WriteData32(value);
						address += 4;
					}
				}
			}
			else // chcr.transferDirection == Channel::TransferDirection::DeviceToRAM
			{
				HP_ASSERT(channel.chcr.addressIncrement == Channel::AddressIncrement::Increment, "Current implementation only supports address increment"); // #TODO: Support decrement mode if required
				unsigned int bytesAvailable = m_ram.GetSizeBytes() - pa;
				if (byteCount <= bytesAvailable)
				{
					// block copy
					m_spu.ReadDataBlock(m_ram.GetData() + pa, wordCount);
					address += byteCount; // update address to end of transfer
				}
				else // copy word by word
				{
					HP_FATAL_ERROR("Not implemented");
				}
			}

			// In SyncMode=1 and SyncMode=2, the hardware does update MADR.
			// It will contain the start address of the currently transferred block; at transfer end, it'll hold the end-address in SyncMode=1, or the end marker in SyncMode=2)
			// Address bits 0-1 are writeable, but any updated current/end addresses are word-aligned with bits 0-1 forced to zero.
			// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
			channel.madr = address;
			channel.madr &= ~3; // word-align

			// SyncMode=1 decrements BA to zero
			channel.bcr &= 0x0000ffff;

			break;
		}
		case Channel::SyncMode::LinkedList:
		{
			HP_FATAL_ERROR("Don't expect sync mode 2 (linked list) DMA to be used for SPU");
			break;
		}
		case Channel::SyncMode::Reserved:
			HP_DEBUG_FATAL_ERROR("SPU DMA with reserved sync mode 3");
			break;
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	constexpr u32 channelFlag = 1 << channelIndex;
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

// DMA6 - OTC (Ordering Table Clear)
//
// The PSX GPU does not have depth buffer. To draw polygons in depth order, software insertes GPU commands into a linked list.
// DMA2 can traverse this linked list and transfer the draw commands to the GPU.
// This routine initialises an empty linked list.
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#depth-ordering-table-ot
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#initializing-an-empty-ot-via-dma6
//
void DMAC::orderingTableClear(u32 pc)
{
	constexpr u32 channelIndex = (u32)DMAChannel::OTC; // DMA6
	Channel& channel = m_channels[channelIndex];
	Channel::ControlRegister& chcr = channel.chcr;
	HP_DEBUG_ASSERT(chcr.syncMode == Channel::SyncMode::Burst, "Expect DMA6 OTC syncmode to be hardcoded to 0 (burst)");

	// https://psx-spx.consoledev.net/dmachannels/#1f801084hn10h-d_bcr-dma-block-control-channel-06-rw
	unsigned int nodeCount = channel.bcr & 0xffff; // also word count because empty node is just a single word header
	if (nodeCount == 0)
		nodeCount = 0x10000;

	if (chcr.startTrigger == 0)
		return;

	// CHCR bit 28 is cleared when transfer starts
	chcr.startTrigger = 0;

	u32 nodeAddress = channel.madr; // pointer to *last* table entry

	HP_UNUSED(chcr.addressIncrement); // AddressIncrement is ignored for DMA6 OTC. Same result regardless of value.
	for (unsigned int i = 0; i < nodeCount; i++)
	{
		bool isLastNode = (i == nodeCount - 1);
		u32 nextNodeAddress = isLastNode ? 0xffffff : nodeAddress - 4; // linked-list is in reverse order
		constexpr u32 dataWordCount = 0x00;
		u32 header = (dataWordCount << 24) | (nextNodeAddress & 0x00ffffff); // Important not to stomp encoed data word count in upper 8 bits with upper bits of address!
		writeU32(nodeAddress, header); // end-of-list marker

		nodeAddress -= 4; // list is built in reverse order
	}

	// CHCR bit 24 is cleared when DMA completes (transfer is instantaneous in this implementation)
	chcr.startTransfer = false;

	// MADR is not updated in Burst SyncMode (0); it will contain the start address even during and after the transfer.
	// Unless Chopping is enabled, in that case it does update MADR, same does probably also happen when getting interrupted by a higher priority DMA channel).
	// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw
	if (chcr.syncMode != Channel::SyncMode::Burst)
		channel.madr = nodeAddress;

	if (s_logDMA || (s_logDMAChannelBits & (1u << channelIndex)))
		LOG_INFO("[DMA] DMA6 OTC performed from address %08X back to address %08X, %u nodes PC: %08X\n", channel.madr, nodeAddress, nodeCount, pc);

	// Set DMA InterruptRegister channelInterruptFlags when DMA completes, and 
	// IRQ flags in bit (24+n) are set upon DMAn completion - but caution - they are set ONLY if enabled in bit (16+n).
	// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
	constexpr u32 channelFlag = 1 << channelIndex;
	if (m_dicr.channelInterruptMask & channelFlag)
	{
		m_dicr.channelInterruptFlags |= channelFlag;

		// Update master interrupt flag, which depends on the channel interrupt flags.
		updateMasterInterruptFlag();
	}

	m_stats.transferCount[channelIndex]++;
}

// Updates DICR (DMA Interrupt Register) master interrupt flag bit 31.
// 
// Call this whenever any dependent data changes i.e. channelInterruptFlags when DMA completes
// 
//     IF b15=1 OR (b23=1 AND (b16-22 AND b24-30)>0) THEN b31=1 ELSE b31=0
//
// See https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw
// 
void DMAC::updateMasterInterruptFlag()
{
	const u32 flagPrev = m_dicr.masterInterruptFlag;

	// Important. From psx-spx:
	//    Note that the per-channel enable bits 16-22 (the channelInterruptMask) do not factor into the bit 31 calculation.
	//    They only gate whether a DMA completion sets the corresponding flag bit (b24-30).
	//    Once a flag bit is set, it contributes to the master flag regardless of whether the channel enable is still on.
	//    Flag bits persist until explicitly acknowledged by writing 1 to them.
	// If the enable bits *are* erroneously used here, lots of games still work, but the MDEC instros for Tomb Raider, Wipeout and Doom all fail.
	m_dicr.masterInterruptFlag = (m_dicr.busError == 1) || (m_dicr.masterChannelInterruptEnable && (m_dicr.channelInterruptFlags != 0));

	if (!flagPrev && m_dicr.masterInterruptFlag)
	{
		if (s_logDMA || s_logDMAChannelBits != 0)
			LOG_INFO("[DMA] Firing master interrupt IRQ3\n");

		// Set Interrupt Status Register (I_STAT) IRQ3 flag upon 0-to-1 transition of DICR bit 31
		// https://psx-spx.consoledev.net/dmachannels/#1f8010f4h-dicr-dma-interrupt-register-rw"
		m_intc.SetIRQ(IRQ::IRQ3_DMA);
	}
}

void DMAC::writeDICR(u32 val, u32 pc)
{
	// Only set subset of bits directly
	InterruptRegister dicr;
	dicr.val = val;
	m_dicr.channelCompletionBehaviour = dicr.channelCompletionBehaviour; // bits 6:0
	m_dicr.busError = dicr.busError; // bit 15 #TODO: Can the bus error bit be set directly like this? Docs say R/W...
	m_dicr.channelInterruptMask = dicr.channelInterruptMask; // bits 22:16
	m_dicr.masterChannelInterruptEnable = dicr.masterChannelInterruptEnable; // bit 23

	// Clear any channel 0-6 interrupt flags in bits 30:24 if a 1 is written.
	const u32 prevChannelInterruptFlags = m_dicr.channelInterruptFlags;
	m_dicr.channelInterruptFlags &= ~dicr.channelInterruptFlags;

	updateMasterInterruptFlag();

	if (s_logDMARegisterAccess)
	{
		u32 clearedFlags = prevChannelInterruptFlags & dicr.channelInterruptFlags;

		LOG_INFO(
			"[DMA] DICR interrupt register write value %08X PC: %08X :\n"
			"[DMA]   Master interrupt %s\n"
			"[DMA]   DMA0 (MDECin) %s%s%s\n"
			"[DMA]   DMA1 (MDECout) %s%s%s\n"
			"[DMA]   DMA2 (GPU) %s%s%s\n"
			"[DMA]   DMA3 (CDROM) %s%s%s\n"
			"[DMA]   DMA4 (SPU) %s%s%s\n"
			"[DMA]   DMA5 (PIO) %s%s%s\n"
			"[DMA]   DMA6 (OTC) %s%s%s\n",
			val, pc,
			dicr.masterChannelInterruptEnable ? "enable" : "disable",
			(dicr.channelInterruptMask & 0x01) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x01) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x01) ? (clearedFlags & 0x01) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x02) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x02) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x02) ? (clearedFlags & 0x02) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x04) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x04) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x04) ? (clearedFlags & 0x04) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x08) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x08) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x08) ? (clearedFlags & 0x08) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x10) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x10) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x10) ? (clearedFlags & 0x10) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x20) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x20) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x20) ? (clearedFlags & 0x20) ? " (was pending)" : " (was not pending)" : "",
			(dicr.channelInterruptMask & 0x40) ? "enable" : "disable", (dicr.channelInterruptFlags & 0x40) ? " ack IRQ" : "", (dicr.channelInterruptFlags & 0x40) ? (clearedFlags & 0x40) ? " (was pending)" : " (was not pending)" : "");
	}
}

u32 DMAC::readU32(u32 va) const
{
	u32 pa = getPhysicalAddress(va);
	return m_ram.ReadU32(pa);
}

void DMAC::writeU32(u32 va, u32 val)
{
	u32 pa = getPhysicalAddress(va);

	// #TODO: Ignore writes outside of physical RAM range? This seems to be implied by:
	//     The address counter wraps around when counting down from 000000h to FFFFFCh, leading to words after wraparound not being written to RAM (as FFFFFCh is past the default 8 MB main RAM region).
	// https://psx-spx.consoledev.net/dmachannels/#1f801080hn10h-d_madr-dma-base-address-channel-06-rw

	m_ram.WriteU32(pa, val);
}
