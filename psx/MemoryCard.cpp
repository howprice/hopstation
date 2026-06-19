#include "MemoryCard.h"

#include "core/hp_assert.h"
#include "core/Log.h"
#include "core/Helpers.h" // HP_UNUSED
#include "core/ArrayHelpers.h"

#include <string.h> // memset
#include <stdio.h>

static_assert(COUNTOF_ARRAY(kMemoryCardStateNames) == ENUM_COUNT(MemoryCard::State));

MemoryCard::MemoryCard(unsigned int portIndex)
	: m_portIndex(portIndex)
{
	m_data = new u8[kSizeBytes];
	memset(m_data, 0, kSizeBytes);
	Format();
}

MemoryCard::~MemoryCard()
{
	delete[] m_data;
}

void MemoryCard::Format()
{
	// The first block is used as Directory, the remaining 15 blocks contain file data.

	// Header Frame (Block 0, Frame 0)
	//   00h-01h Memory Card ID (ASCII "MC")
	//   02h-7Eh Unused (zero)
	//   7Fh     Checksum (all above bytes XORed with each other) (usually 0Eh)
	memset(m_data, 0, kSectorSizeBytes);
	m_data[0] = 'M';
	m_data[1] = 'C';
	m_data[0x7f] = 0x0e;

	// Directory Frames (Block 0, Frame 1..15)
	//   00h-03h Block Allocation State
	//              00000051h - In use ;first-or-only block of a file
	//              00000052h - In use ;middle block of a file (if 3 or more blocks)
	//              00000053h - In use ;last block of a file   (if 2 or more blocks)
	//              000000A0h - Free   ;freshly formatted
	//              000000A1h - Free   ;deleted (first-or-only block of file)
	//              000000A2h - Free   ;deleted (middle block of file)
	//              000000A3h - Free   ;deleted (last block of file)
	//   04h-07h Filesize in bytes (2000h..1E000h; in multiples of 8Kbytes)
	//   08h-09h Pointer to the NEXT block number (minus 1) used by the file
	//             (ie. 0..14 for Block Number 1..15) (or FFFFh if last-or-only block)
	//   0Ah-1Eh Filename in ASCII, terminated by 00h (max 20 chars, plus ending 00h)
	//   1Fh     Zero (unused)
	//   20h-7Eh Garbage (usually 00h-filled)
	//   7Fh     Checksum (all above bytes XORed with each other)
	for (unsigned int frameIndex = 1; frameIndex < 16; frameIndex++)
	{
		u8* frameData = m_data + (frameIndex * kSectorSizeBytes);
		memset(frameData, 0, kSectorSizeBytes);

		frameData[0x00] = 0xa0; // Free   ;freshly formatted
		frameData[0x08] = 0xff; // Pointer to the NEXT block number (minus 1) used by the file (FFFFh if last-or-only block)
		frameData[0x09] = 0xff; // "
		frameData[0x7f] = 0xa0; // Checksum (all above bytes XORed with each other) A0^FF^FF=A0
	}

	// Broken Sector List (Block 0, Frame 16..35)
	//   00h-03h Broken Sector Number (Block*64+Frame) (FFFFFFFFh=None)
	//   04h-7Eh Garbage (usually 00h-filled) (some cards have [08h..09h]=FFFFh)
	//   7Fh     Checksum (all above bytes XORed with each other)
	for (unsigned int frameIndex = 16; frameIndex < 36; frameIndex++)
	{
		u8* frameData = m_data + (frameIndex * kSectorSizeBytes);
		memset(frameData, 0, kSectorSizeBytes);
		frameData[0x00] = 0xff; // Broken Sector Number (Block*64+Frame) (FFFFFFFFh=None)
		frameData[0x01] = 0xff; // "
		frameData[0x02] = 0xff; // "
		frameData[0x03] = 0xff; // "
		frameData[0x08] = 0xff;
		frameData[0x09] = 0xff;
		frameData[0x7f] = 0x00; // Checksum: FF^FF^FF^FF^FF^FF = 0x00
	}

	// Broken Sector Replacement Data (Block 0, Frame 36..55)
	//   00h-7Fh Data (usually FFh-filled, if there's no broken sector)
	// 
	// Unused Frames (Block 0, Frame 56..62)
	//   00h-7Fh Unused (usually FFh-filled)
	// 
	// Duckstation fills this with zero, so I will too
	memset(m_data + (36 * kSectorSizeBytes), 0, (62 - 36 + 1) * kSectorSizeBytes);

	// Write Test Frame (Block 0, Frame 63)
	// Reportedly "write test". Usually same as Block 0 ("MC", 253 zero-bytes, plus checksum 0Eh).
	u8* testFrameData = m_data + (63 * kSectorSizeBytes);
	memset(testFrameData, 0, kSectorSizeBytes);
	testFrameData[0] = 'M';
	testFrameData[1] = 'C';
	testFrameData[0x7f] = 0x0e; // Checksum (all above bytes XORed with each other) (usually 0Eh)

	// Remaining blocks of card is FF filled
	memset(m_data + kBlockSizeBytes, 0xff, kSizeBytes - kBlockSizeBytes);

	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u formatted\n", m_portIndex);
}

bool MemoryCard::LoadFromFile(const char* path)
{
	FILE* fp = fopen(path, "rb");
	HP_ASSERT(fp, "[MemCard] Failed to open file: %s\n", path);

	// Check file size matches expected memory card size
	fseek(fp, 0, SEEK_END);
	long fileSize = ftell(fp);
	if (fileSize != kSizeBytes)
	{
		LOG_ERROR("[MemCard] File size does not match expected memory card size: %s\n", path);
		fclose(fp);
		return false;
	}

	fseek(fp, 0, SEEK_SET); // Seek back to beginning before reading

	unsigned int bytesRead = (unsigned int)fread(m_data, 1, kSizeBytes, fp);
	if (bytesRead != kSizeBytes)
	{
		LOG_ERROR("[MemCard] Failed to read file: %s\n", path);
		fclose(fp);
		return false;
	}

	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Loaded memory card from file: %s\n", path);

	fclose(fp);
	fp = nullptr;

	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u FLAG bit 3 set, indicating directory not yet read.\n", m_portIndex);

	m_flag = 0x08; // Set FLAG bit 3 to indicate directory not yet read, as if the card was just inserted.

	return true;
}

bool MemoryCard::SaveToFile(const char* path)
{
	HP_ASSERT(path && path[0]);
	FILE* fp = fopen(path, "wb");
	if (!fp)
	{
		LOG_ERROR("[MemCard] Failed to open file to save card %u: %s\n", m_portIndex, path);
		return false;
	}

	if (fwrite(m_data, 1, kSizeBytes, fp) != kSizeBytes)
	{
		LOG_ERROR("[MemCard] Failed to write file to save card %u: %s\n", m_portIndex, path);
		fclose(fp);
		return false;
	}
	fclose(fp);
	fp = nullptr;

	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u saved to %s\n", m_portIndex, path);

	return true;
}

void MemoryCard::Reset()
{
	m_state = State::Idle;
	m_step = 0;

	// The initial value of the FLAG byte on power-up (and when re-inserting the memory card) is 08h
	m_flag = 0x08;

	m_previousCommandByte = 0;
	m_sectorIndex = 0;
	m_dataByteIndex = 0;
	m_checksum = 0;
	m_checksumOK = true;
}

void MemoryCard::OnInserted()
{
	// The initial value of the FLAG byte on power-up and when re-inserting the memory card is 08h.
	// Bit3=1 is indicating that the directory wasn't read yet, allowing to sense memory card changes.
	m_flag = 0x08;

	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u inserted. FLAG bit 3 set\n", m_portIndex);
}

void MemoryCard::Select()
{
	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u selected. Resetting state machine\n", m_portIndex);

	// Reset state machine to Idle when selected, so that it can properly process the first command byte.
	// This is important, because the BIOS starts a Read, just to check the ID from the first few response
	// bytes, then deselects/selects the port to abort the read.
	m_state = State::Idle;
	m_step = 0;

	// n.b. FLAG bit 3 should *not* be set here.
}

void MemoryCard::Deselect()
{
	if (g_logMemoryCard)
		LOG_INFO("[MemCard] Card %u deselected\n", m_portIndex);
}

bool MemoryCard::Write8(u8 val, u8& response)
{
	bool ack = false;
	response = 0xff; // Default response in the case of no selected device, or address byte is high impedance value.

	switch (m_state)
	{
		case State::Idle:
			ack = write8_Idle(val, response);
			break;
		case State::Read:
			ack = write8_Read(val, response);
			break;
		case State::Write:
			ack = write8_Write(val, response);
			break;
	}

	m_previousCommandByte = val;
	return ack;
}

bool MemoryCard::write8_Idle(u8 val, u8& response)
{
	switch (val)
	{
		// Read
		// https://psx-spx.consoledev.net/controllersandmemorycards/#memory-card-readwrite-commands
		case 0x52: // 0x52 is ASCII 'R' for Read
		{
			m_state = State::Read;
			m_step = 0;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u idle received %02X. Transition to Read.\n", m_portIndex, val);

			return write8_Read(val, response);
		}
		case 0x57: // 0x57 is ASCII 'W' for Write
		{
			m_state = State::Write;
			m_step = 0;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u idle received %02X. Transition to Write.\n", m_portIndex, val);

			return write8_Write(val, response);
		}
		case 0x53: // 0x53 is ASCII 'S' for Status
		{
			// https://psx-spx.consoledev.net/controllersandmemorycards/#get-memory-card-id-command
			HP_FATAL_ERROR("Memory Card ID Command");
			response = 0xff;
			return false;
		}
		default:
		{
			HP_FATAL_ERROR("Unhandled memory card command %02X", val);
			response = 0xff;
			return false;
		}
	}
}

// Reading Data from Memory Card
// 
//   Index Send Reply Comment
//   -     81h  N/A   Memory card address                                    Implementation note: This is handled by the Controller Port
//   0     52h  FLAG  Send Read Command (ASCII "R"), Receive FLAG Byte
//   1     00h  5Ah   Receive Memory Card ID1
//   2     00h  5Dh   Receive Memory Card ID2
//   3     MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)    (00h) means result doesn't matter, but usually zero
//   4     LSB  (pre) Send Address LSB  ;/                           (pre) means result doesn't matter, but is usually equal to the previous command byte i.e. MSB
//   5     00h  5Ch   Receive Command Acknowledge 1  ;<-- late /ACK after this byte-pair
//   6     00h  5Dh   Receive Command Acknowledge 2
//   7     00h  MSB   Receive Confirmed Address MSB
//   8     00h  LSB   Receive Confirmed Address LSB
//   9     00h  ...   Receive Data Sector (128 bytes)
//   A     00h  CHK   Receive Checksum (MSB xor LSB xor Data bytes)
//   B     00h  47h   Receive Memory End Byte (should be always 47h="G"=Good for Read)
//
// https://psx-spx.consoledev.net/controllersandmemorycards/#reading-data-from-memory-card
//
bool MemoryCard::write8_Read(u8 val, u8& response)
{
	switch (m_step)
	{
		case 0:
		{
			HP_DEBUG_ASSERT(val == 0x52, "Expected memory card read command 0x52"); // 0x52 is ASCII 'R' for Read

			response = m_flag;
			m_dataByteIndex = 0; // Reset data byte index for the new read command
			m_checksum = 0; // Reset checksum for the new read command

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (FLAG)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 1:
		{
			response = 0x5a; // Memory Card ID1

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (Memory Card ID1)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 2:
		{
			response = 0x5d; // Memory Card ID2

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (Memory Card ID2)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 3:
		{
			u8 addressMSB = val;
			m_sectorIndex = (u16)addressMSB << 8; // MSB of sector index
			response = 0x00; // The value sent by the host is ignored, but is usually zero
			m_checksum = addressMSB; // Checksum starts with MSB of sector index

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X (MSB) response %02X\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 4:
		{
			m_sectorIndex |= val; // LSB of sector index

			response = m_previousCommandByte; // The value sent by the host is ignored, but is usually equal to the previous command byte i.e. MSB
			m_checksum ^= val; // Checksum is XOR of MSB and LSB of sector index and data bytes

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X (LSB) response %02X (pre, MSB). Sector %04X%s\n",
					m_portIndex, m_step, val, response, m_sectorIndex, m_sectorIndex >= kNumSectors ? " INVALID" : "");

			m_step++;
			return true;
		}
		case 5:
		{
			response = 0x5c; // Command Acknowledge 1

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (Command Acknowledge 1)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 6:
		{
			response = 0x5d; // Command Acknowledge 2

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (Command Acknowledge 2)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 7:
		{
			// When sending an invalid sector number, original Sony memory cards respond with FFFFh as Confirmed Address
			// and then abort the transfer without sending any data, checksum, or end flag
			if (m_sectorIndex < kNumSectors)
			{
				response = (m_sectorIndex >> 8) & 0xff; // Confirmed Address MSB

				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (address MSB)\n", m_portIndex, m_step, val, response);
			}
			else
			{
				response = 0xff; // Invalid sector index, so respond with 0xffff as Confirmed Address

				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (due to invalid sector index)\n", m_portIndex, m_step, val, response);
			}

			m_step++;
			return true;
		}
		case 8:
		{
			if (m_sectorIndex < kNumSectors)
			{
				response = m_sectorIndex & 0xff; // Confirmed Address LSB

				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (address LSB)\n", m_portIndex, m_step, val, response);

				m_step++;
			}
			else
			{
				response = 0xff; // See notes above for case 7

				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X. Aborting transfer due to invalid sector index.\n", m_portIndex, m_step, val, response);

				m_state = State::Idle; // Return to idle state after completing the read command
				m_step = 0;

				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Read aborted due to invalid sector index %04X\n", m_portIndex, m_sectorIndex);
			}
			return true;
		}
		case 9:
		{
			// Data Sector (128 bytes)
			response = m_data[(m_sectorIndex * kSectorSizeBytes) + m_dataByteIndex];
			m_checksum ^= response; // Checksum is XOR of MSB and LSB of sector index and data bytes

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read received %02X responding with data byte index %u value %02X\n", m_portIndex, val, m_dataByteIndex, response);

			m_dataByteIndex++;
			if (m_dataByteIndex == kSectorSizeBytes)
			{
				m_step++;
				m_dataByteIndex = 0;
			}

			return true;
		}
		case 0xA:
		{
			response = m_checksum;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X (checksum)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 0xB:
		{
			response = 0x47; // Memory End Byte (should be always 47h="G"=Good for Read)

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read step %u received %02X response %02X. Read complete - transitioning to idle.\n", m_portIndex, m_step, val, response);

			m_state = State::Idle; // Return to idle state after completing the read command
			m_step = 0;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Read complete\n", m_portIndex);

			return true;
		}
		default:
		{
			HP_FATAL_ERROR("Unreachable code");
			response = 0xff;
			return false;
		}
	}
}

// Writing Data to Memory Card
// 
// Index  Send Reply Comment
// -      81h  N/A   Memory card address                                      Implementation note: This is handled by the Controller Port
// 0      57h  FLAG  Send Write Command (ASCII "W"), Receive FLAG Byte
// 1      00h  5Ah   Receive Memory Card ID1
// 2      00h  5Dh   Receive Memory Card ID2
// 3      MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
// 4      LSB  (pre) Send Address LSB  ;/
// 5      ...  (pre) Send Data Sector (128 bytes)
// 6      CHK  (pre) Send Checksum (MSB xor LSB xor Data bytes)
// 7      00h  5Ch   Receive Command Acknowledge 1
// 8      00h  5Dh   Receive Command Acknowledge 2
// 9      00h  4xh   Receive Memory End Byte (47h=Good, 4Eh=BadChecksum, FFh=BadSector)
// 
// https://psx-spx.consoledev.net/controllersandmemorycards/#writing-data-to-memory-card
//
bool MemoryCard::write8_Write(u8 val, u8& response)
{
	switch (m_step)
	{
		case 0:
		{
			HP_DEBUG_ASSERT(val == 0x57, "Expected memory card write command 0x57"); // 0x57 is ASCII 'W' for Write

			response = m_flag;
			m_dataByteIndex = 0; // Reset data byte index for the new write command
			m_checksum = 0; // Reset checksum for the new write command

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X (FLAG)%s\n", m_portIndex, m_step, val, response,
					(m_flag & 0x08) ? " FLAG bit 3 reset" : "");

			// FLAG Byte
			// The initial value of the FLAG byte on power-up (and when re-inserting the memory card) is 08h.
			// Bit3=1 is indicating that the directory wasn't read yet (allowing to sense memory card changes).
			// For some strange reason, bit3 is NOT reset when reading from the card, but rather when writing to it.
			// To reset the flag, games are usually issuing a dummy write to sector number 003Fh, more or less
			// unneccessarily stressing the lifetime of that sector.
			//
			// #TODO: When should the flag be reset? Before returning value, immediately after, or later on?
			if (m_flag & 0x08)
				m_flag &= ~0x08; // Reset bit3 of flag when writing to the card, as this is how games detect that the card was read and is ready for changes

			m_step++;
			return true;
		}
		case 1:
		{
			response = 0x5a; // Memory Card ID1

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X (ID1)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 2:
		{
			response = 0x5d; // Memory Card ID2

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X (ID2)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 3:
		{
			u8 addressMSB = val;
			m_sectorIndex = (u16)addressMSB << 8; // MSB of sector index
			response = 0x00; // The value sent by the host is ignored, but is usually zero
			m_checksum = addressMSB; // Checksum starts with MSB of sector index

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X (address MSB) response %02X\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 4:
		{
			m_sectorIndex |= val; // LSB of sector index
			response = m_previousCommandByte; // The value sent by the host is ignored, but is usually equal to the previous command byte i.e. MSB
			m_checksum ^= val; // Checksum is XOR of MSB and LSB of sector index and data bytes

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X (address LSB) response %02X. Sector index: %04X%s\n",
					m_portIndex, m_step, val, response, m_sectorIndex, m_sectorIndex >= kNumSectors ? " INVALID" : "");

			m_step++;
			return true;
		}
		case 5:
		{
			// Data Sector (128 bytes)
			if (m_sectorIndex < kNumSectors) // igmore writes to invalid sector index #TODO: Is this correct hardware behaviour?
				m_data[(m_sectorIndex * kSectorSizeBytes) + m_dataByteIndex] = val;
			m_checksum ^= val; // Checksum is XOR of MSB and LSB of sector index and data bytes
			response = m_previousCommandByte;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write received data byte index %u value %02X. Response %02X\n", m_portIndex, m_dataByteIndex, val, response);

			m_dataByteIndex++;
			if (m_dataByteIndex == kSectorSizeBytes)
			{
				m_step++;
				m_dataByteIndex = 0;
			}
			return true;
		}
		case 6:
		{
			m_checksumOK = (val == m_checksum);
			response = m_previousCommandByte;

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X (checksum %s) response %02X\n",
					m_portIndex, m_step, val, m_checksumOK ? "OK" : "BAD", response);

			m_step++;
			return true;
		}
		case 7:
		{
			response = 0x5c; // Command Acknowledge 1

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X (Command Acknowledge 1)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 8:
		{
			response = 0x5d; // Command Acknowledge 2

			if (g_logMemoryCard)
				LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X (Command Acknowledge 2)\n", m_portIndex, m_step, val, response);

			m_step++;
			return true;
		}
		case 9:
		{
			// 4xh   Receive Memory End Byte (47h=Good, 4Eh=BadChecksum, FFh=BadSector)
			if (m_sectorIndex >= kNumSectors)
			{
				response = 0xFF; // Memory End Byte (should be always 47h="G"=Good for Write)
				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X. Write FAILED due to bad sector index %04X.\n",
						m_portIndex, m_step, val, response, m_sectorIndex);
			}
			else if (!m_checksumOK)
			{
				response = 0x4e; // Memory End Byte (4Eh=BadChecksum)
				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X. Write FAILED due to bad checksum (expected %02X)\n",
						m_portIndex, m_step, val, response, m_checksum);
			}
			else
			{
				response = 0x47;
				if (g_logMemoryCard)
					LOG_INFO("[MemCard] Card %u Write step %u received %02X response %02X. Write complete.\n", m_portIndex, m_step, val, response);
			}

			m_state = State::Idle; // Return to idle state after completing the write command
			m_step = 0;
			return true;
		}
		default:
		{
			HP_FATAL_ERROR("Unreachable code");
			response = 0xff;
			return false;
		}
	}
}
