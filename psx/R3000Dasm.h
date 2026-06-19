#pragma once

#include "core/ClassHelpers.h"
#include "core/Types.h"

//
// R3000 (MIPS I) CPU instruction disassembler, specifically tailored for PlayStation 1 emulation.
//
class R3000Dasm
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(R3000Dasm);

	// Returns number of bytes disassembled, which is usually 4 for R3000 instructions.
	static unsigned int Disassemble(u32 opcode, u32 pc, char* buffer, unsigned int bufferSizeBytes);
};
