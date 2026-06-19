#include "Sideload.h"

#include "R3000Utils.h"
#include "TTYLogger.h"

#include "psx/Bus.h"
#include "psx/R3000Dasm.h"

#include "core/MathsHelpers.h" // IsPowerOfTwo
#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/Types.h"

#include <stdio.h>
#include <string.h> // memcmp

// PSX General-Purpose Executable header
//
// See:
// - https://patpend.net/technical/psx/exeheader.txt
// - https://psx-spx.consoledev.net/cdromfileformats/#filenameexe-general-purpose-executable
//
struct ExeHeader
{
	char id[8];               // [0x0] "PS-X EXE" #TODO: Can this be "SCE EXE" too?
	u32 text_offset;          // [0x8] Offset of the text segment. Usually 0.
	u32 data_offset;          // [0xC] Offset of the data segment. Usually 0.
	u32 initialPC;            // [0x10] Program Counter. Usually 80010000h, or higher.
	u32 initialGP_R28;        // [0x14] Address of the Global Pointer. Usually 0
	u32 text_addr;            // [0x18] The address where the text segment is loaded, CODE/DATA is copied to this address in RAM. Usually 80010000h, or higher.
	u32 text_size;            // [0x1C] The size of the text segment in bytes.
	u32 data_addr;            // [0x20] The address where the data segment is loaded.
	u32 data_size;            // [0x24] The size of the data segment in bytes.
	u32 bss_addr;             // [0x28] The address of the BSS segment
	u32 bss_size;             // [0x2C] The size of the BSS segment in bytes.
	u32 s_addr;               // [0x30] The address of the stack. Usually 801FFFF0h or 0=None
	u32 s_size;               // [0x34] The size of the stack. Usually 0. Added to s_addr to get initial SP/R29 value.
	u32 SavedSP;              // [0x38] Reserved for A(43h) Function (should be zero in exefile).
	u32 SavedFP;              // [0x3C] Reserved for A(43h) Function (should be zero in exefile).
	u32 SavedGP;              // [0x40] Reserved for A(43h) Function (should be zero in exefile).
	u32 SavedRA;              // [0x44] Reserved for A(43h) Function (should be zero in exefile).
	u32 SavedS0;              // [0x48] Reserved for A(43h) Function (should be zero in exefile).
	char marker[0x7b4];       // [0x4C] Followed by zeros. e.g. "Sony Computer Entertainment Inc. for Europe area"

	// CODE/DATA sections follow header in file.
};
static_assert(sizeof(ExeHeader) == 0x800, "Expect exe header to be 2048 bytes");

// Debug
static bool s_logCpuState = false;
static bool s_logDisassembly = false;

bool Sideload::SideloadExecutable(const char* path, Bus& bus, TTYLogger* pTTYLogger)
{
	HP_ASSERT(path && path[0]);

	// Load the executable into memory.
	FILE* fp = fopen(path, "rb");
	if (!fp)
	{
		LOG_ERROR("Failed to open file: %s\n", path);
		return false;
	}

	fseek(fp, 0, SEEK_END);
	unsigned int fileSizeBytes = (unsigned int)ftell(fp);
	fseek(fp, 0, SEEK_SET);
	LOG_INFO("Opened sideload file \"%s\" size %u (0x%X)\n", path, fileSizeBytes, fileSizeBytes);

	uint8_t* pExecutable = new uint8_t[fileSizeBytes];
	if (fread(pExecutable, 1, fileSizeBytes, fp) != fileSizeBytes)
	{
		LOG_ERROR("Failed to read sideload file: %s\n", path);
		delete[] pExecutable;
		return false;
	}
	fclose(fp);
	fp = nullptr;

	// Parse EXE header
	// See:
	// - https://psx-spx.consoledev.net/cdromfileformats/#filenameexe-general-purpose-executable
	// - https://jsgroth.dev/blog/posts/ps1-sideloading/#exe-sideloading
	const ExeHeader* pHeader = (ExeHeader*)pExecutable;
	if (memcmp(pHeader->id, "PS-X EXE", 8) != 0)
	{
		// #TODO: "SCE EXE" may be valid too
		LOG_ERROR("Not a valid PSX executable.\n");
		return false;
	}

	R3000& r3000 = bus.GetCPU();

	// For the SCPH1001 BIOS, the sideload address is hit in 0x2921c2 instructions.
	// To detect a problem, fail if this is exceeded by much.
	// #TODO: Test with other BIOS, which may execute more instructions before they load the shell.
	u64 instructionCount = 0;
	while (instructionCount < 0x300000)
	{
		if (s_logCpuState)
			R3000Utils::LogState(r3000);

		if (s_logDisassembly)
		{
			// Disassemble instruction
			u32 pc = r3000.GetPC();
			u32 opcode = bus.ReadWord(pc);
			char disasmBuffer[64]{};
			if (R3000Dasm::Disassemble(opcode, pc, disasmBuffer, sizeof(disasmBuffer)) == 0)
			{
				LOG_ERROR("Failed to disassemble opcode: 0x%08X at address 0x%08X\n", opcode, pc);
				return false;
			}

			LOG_INFO("%08X: %08X %s\n", pc, opcode, disasmBuffer);
		}

		// The PSX BIOS copies the shell to $30000 in RAM and jumps to $80030000 to execute it.
		// We will detect this and sideload our own exe if required.
		static constexpr u32 kSideloadAddress = 0x80030000;
		if (r3000.GetPC() == kSideloadAddress)
		{
			LOG_INFO("Sideloading %s at address %08X\n", path, kSideloadAddress);

			// Executables are not relocatable (and the header contains no relocation information).
			// Code and data sections are simply copied unaltered to RAM.

			RAM& ram = bus.GetRAM();
			HP_ASSERT(IsPowerOfTwo(ram.GetSizeBytes()));
			u32 kRamMask = ram.GetSizeBytes() - 1;

			// Copy CODE/DATA section data into RAM
			if (pHeader->text_size > 0)
			{
				// Section data follows header in file.
				if (sizeof(ExeHeader) + pHeader->text_size > fileSizeBytes)
				{
					LOG_ERROR("Executable section data exceeds file size\n");
					delete[] pExecutable;
					return false;
				}

				u32 loadAddress = pHeader->text_addr & kRamMask;
				ram.Write(loadAddress, pExecutable + sizeof(ExeHeader), pHeader->text_size);
			}

			HP_ASSERT(pHeader->data_size == 0, "Not tested");

			// Clear BSS section
			if (pHeader->bss_size > 0)
			{
				HP_FATAL_ERROR("Not implemented: BSS section handling");
				return false;
			}

			// Set initial register values
			r3000.SetGPR(28, pHeader->initialGP_R28);

			u32 initialSP = pHeader->s_addr + pHeader->s_size;
			if (initialSP != 0)
				r3000.SetGPR(29, initialSP);

			// Jump to exe entry point
			r3000.SetPC(pHeader->initialPC);

			// #TODO: Might not want to delete executable to avoid potential performance spike.
			delete[] pExecutable;
			pExecutable = nullptr;

			return true;
		}

		bus.StepInstruction();

		instructionCount++;

		if (pTTYLogger)
			pTTYLogger->Update(r3000, /*callback*/nullptr);
	}

	delete[] pExecutable;
	pExecutable = nullptr;

	return false;
}

void Sideload::EnableAmidogTestDebugOutput(Bus& bus)
{
	static constexpr char args[3][15] = { "auto", "console", "release" };
	int argLen = 2;
	unsigned int len = 0;

	for (int i = 0; i < argLen; i++) {
		bus.WriteWord(0x1f800004 + i * 4, (u32)(0x1f800044 + len));
		unsigned int x;
		unsigned int n = (unsigned int)strlen(args[i]) + 1;
		for (x = len; x < len + n; x++) {
			bus.WriteByte(0x1f800044 + x, args[i][x - len]);
		}

		len = x;
	}

	bus.WriteWord(0x1f800000, argLen);
}
