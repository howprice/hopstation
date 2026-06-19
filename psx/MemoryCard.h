// PlayStation memory card
//
// https://psx-spx.consoledev.net/controllersandmemorycards/

#pragma once

#include "core/Types.h"

inline bool g_logMemoryCard = false;

class MemoryCard
{
public:

	// https://psx-spx.consoledev.net/controllersandmemorycards/#memory-card-data-format
	static const unsigned int kSectorSizeBytes = 0x80; // 128 bytes
	static const unsigned int kSectorsPerBlock = 0x40; // 64
	static constexpr unsigned int kBlockSizeBytes = kSectorSizeBytes * kSectorsPerBlock; // 0x2000 = 8192 = 8 KiB
	static const unsigned int kNumBlocks = 0x10; // 16
	static constexpr unsigned int kNumSectors = kSectorsPerBlock * kNumBlocks; // 1024
	static constexpr unsigned int kSizeBytes = kBlockSizeBytes * kNumBlocks; // 0x20000 = 131072 = 128 KiB

	// Directory Frames (Block 0, Frame 1..15)
	// 
	//  00h-03h Block Allocation State
	//            00000051h - In use ;first-or-only block of a file
	//            00000052h - In use ;middle block of a file (if 3 or more blocks)
	//            00000053h - In use ;last block of a file   (if 2 or more blocks)
	//            000000A0h - Free   ;freshly formatted
	//            000000A1h - Free   ;deleted (first-or-only block of file)
	//            000000A2h - Free   ;deleted (middle block of file)
	//            000000A3h - Free   ;deleted (last block of file)
	//  04h-07h Filesize in bytes (2000h..1E000h; in multiples of 8Kbytes)
	//  08h-09h Pointer to the NEXT block number (minus 1) used by the file
	//            (ie. 0..14 for Block Number 1..15) (or FFFFh if last-or-only block)
	//  0Ah-1Eh Filename in ASCII, terminated by 00h (max 20 chars, plus ending 00h)
	//  1Fh     Zero (unused)
	//  20h-7Eh Garbage (usually 00h-filled)
	//  7Fh     Checksum (all above bytes XORed with each other)
	// 
	// https://psx-spx.consoledev.net/controllersandmemorycards/#directory-frames-block-0-frame-115
	//
	struct DirectoryFrame
	{
		u32 blockAllocationState;
		u32 fileSizeBytes;
		u16 nextBlockNumberMinus1;
		char filename[0x1e - 0xa + 1];
		u8 unused = 0;
		u8 garbage[0x7e - 0x20 + 1];
		u8 checksum;
	};
	static_assert(sizeof(DirectoryFrame) == kSectorSizeBytes);

	enum class State
	{
		Idle,
		Read,
		Write,

		Max = Write
	};

	MemoryCard(unsigned int portIndex);
	~MemoryCard();

	void Reset();

	void OnInserted();

	void Select();
	void Deselect();

	void Format();
	bool LoadFromFile(const char* path);
	bool SaveToFile(const char* path);

	// Synchronous I/O
	// The data is transferred in units of bytes, via separate input and output lines.
	// So, when sending byte, the hardware does simultaneously receive a response byte.
	// 
	// https://psx-spx.consoledev.net/controllersandmemorycards/#synchronous-io
	//
	// Returns true if the byte is recevied by the selected peripheral i.e. /ACK is asserted
	bool Write8(u8 val, u8& response);

	State GetState() const { return m_state; }
	const u8* GetData() const { return m_data; }

private:

	bool write8_Idle(u8 val, u8& response);
	bool write8_Read(u8 val, u8& response);
	bool write8_Write(u8 val, u8& response);

	[[maybe_unused]]
	unsigned int m_portIndex = 0; // 0=left port, 1=right port. Debugging only.

	State m_state = State::Idle;
	unsigned int m_step = 0;

	// The initial value of the FLAG byte on power-up (and when re-inserting the memory card) is 08h
	u8 m_flag = 0x08;

	u8 m_previousCommandByte = 0;
	u16 m_sectorIndex = 0;
	unsigned int m_dataByteIndex = 0; // For read/write of the 128 bytes in a sector
	u8 m_checksum = 0; // Checksum is XOR of MSB and LSB of sector index and data bytes
	bool m_checksumOK = true;

	u8* m_data = nullptr;
};

inline const char* kMemoryCardStateNames[] = { "Idle", "Read", "Write" };
