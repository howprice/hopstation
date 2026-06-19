#include "R3000Dasm.h"

#include "core/ArrayHelpers.h"
#include "core/StringHelpers.h"
#include "core/hp_assert.h"
#include "core/Helpers.h"

struct Opcode
{
	u32 mask;
	u32 match;
	u32 cycles;
	const char* instruction;
};

static constexpr Opcode kOpcodes[] =
{
	// Mask       Match      Cyc  Instruction
	{ 0xfc1f0000, 0x04000000, 2,  "bltz" },
	{ 0xfc1f0000, 0x04010000, 2,  "bgez" },
	{ 0xfc1f0000, 0x04100000, 2,  "bltzal" },
	{ 0xfc1f0000, 0x04110000, 2,  "bgezal" },
	{ 0xfc000000, 0x08000000, 2,  "j" },
	{ 0xfc000000, 0x0c000000, 2,  "jal" },
	{ 0xfc000000, 0x10000000, 2,  "beq" },
	{ 0xfc000000, 0x14000000, 2,  "bne" },
	{ 0xfc000000, 0x18000000, 2,  "blez" },
	{ 0xfc000000, 0x1c000000, 2,  "bgtz" },
	{ 0xfc000000, 0x20000000, 2,  "addi" },
	{ 0xfc000000, 0x24000000, 2,  "addiu" },
	{ 0xfc000000, 0x28000000, 2,  "slti" },
	{ 0xfc000000, 0x2c000000, 2,  "sltiu" },
	{ 0xfc000000, 0x30000000, 2,  "andi" },
	{ 0xfc000000, 0x34000000, 2,  "ori" },
	{ 0xfc000000, 0x38000000, 2,  "xori" },
	{ 0xfc000000, 0x3c000000, 2,  "lui" },
	{ 0xffe0003f, 0x40000000, 2,  "mfc0" },
	{ 0xffe0003f, 0x40400000, 2,  "cfc0" },
	{ 0xffe0003f, 0x40800000, 2,  "mtc0" },
	{ 0xffe0003f, 0x40c00000, 2,  "ctc0" },
	{ 0xffff0000, 0x41000000, 2,  "bc0f" },
	{ 0xffff0000, 0x41010000, 2,  "bc0t" },
	{ 0xffe0003f, 0x42000001, 2,  "tlbr" },
	{ 0xffe0003f, 0x42000002, 2,  "tlbwi" },
	{ 0xffe0003f, 0x42000006, 2,  "tlbwr" },
	{ 0xffe0003f, 0x42000008, 2,  "tlbp" },
	{ 0xffe0003f, 0x42000010, 2,  "rfe" },
	{ 0xffe0003f, 0x44000000, 2,  "mfc1" },
	{ 0xffe0003f, 0x44400000, 2,  "cfc1" },
	{ 0xffe0003f, 0x44800000, 2,  "mtc1" },
	{ 0xffe0003f, 0x44c00000, 2,  "ctc1" },
	{ 0xffff0000, 0x45000000, 2,  "bc1f" },
	{ 0xffff0000, 0x45010000, 2,  "bc1t" },
	{ 0xffe0003f, 0x48000000, 2,  "mfc2" },
	{ 0xffe0003f, 0x48400000, 2,  "cfc2" },
	{ 0xffe0003f, 0x48800000, 2,  "mtc2" },
	{ 0xffe0003f, 0x48c00000, 2,  "ctc2" },
	{ 0xffff0000, 0x49000000, 2,  "bc2f" },
	{ 0xffff0000, 0x49010000, 2,  "bc2t" },
	{ 0xffe0003f, 0x4c000000, 2,  "mfc3" },
	{ 0xffe0003f, 0x4c400000, 2,  "cfc3" },
	{ 0xffe0003f, 0x4c800000, 2,  "mtc3" },
	{ 0xffe0003f, 0x4cc00000, 2,  "ctc3" },
	{ 0xffff0000, 0x4d000000, 2,  "bc3f" },
	{ 0xffff0000, 0x4d010000, 2,  "bc3t" },
	{ 0xfc000000, 0x80000000, 2,  "lb" },
	{ 0xfc000000, 0x84000000, 2,  "lh" },
	{ 0xfc000000, 0x88000000, 2,  "lwl" },
	{ 0xfc000000, 0x8c000000, 2,  "lw" },
	{ 0xfc000000, 0x90000000, 2,  "lbu" },
	{ 0xfc000000, 0x94000000, 2,  "lhu" },
	{ 0xfc000000, 0x98000000, 2,  "lwr" },
	{ 0xfc000000, 0xa0000000, 2,  "sb" },
	{ 0xfc000000, 0xa4000000, 2,  "sh" },
	{ 0xfc000000, 0xa8000000, 2,  "swl" },
	{ 0xfc000000, 0xac000000, 2,  "sw" },
	{ 0xfc000000, 0xb8000000, 2,  "swr" },
	{ 0xfc000000, 0xc0000000, 2,  "lwc0" },
	{ 0xfc000000, 0xc4000000, 2,  "lwc1" },
	{ 0xfc000000, 0xc8000000, 2,  "lwc2" },
	{ 0xfc000000, 0xcc000000, 2,  "lwc3" },
	{ 0xfc000000, 0xe0000000, 2,  "swc0" },
	{ 0xfc000000, 0xe4000000, 2,  "swc1" },
	{ 0xfc000000, 0xe8000000, 2,  "swc2" },
	{ 0xfc000000, 0xec000000, 2,  "swc3" },
	{ 0xfc00003f, 0x00000000, 2,  "sll" },
	{ 0xfc00003f, 0x00000002, 2,  "srl" },
	{ 0xfc00003f, 0x00000003, 2,  "sra" },
	{ 0xfc00003f, 0x00000004, 2,  "sllv" },
	{ 0xfc00003f, 0x00000006, 2,  "srlv" },
	{ 0xfc00003f, 0x00000007, 2,  "srav" },
	{ 0xfc00003f, 0x00000008, 2,  "jr" },
	{ 0xfc00003f, 0x00000009, 2,  "jalr" },
	{ 0xfc00003f, 0x0000000c, 2,  "syscall" },
	{ 0xfc00003f, 0x0000000d, 2,  "break" },
	{ 0xfc00003f, 0x00000010, 2,  "mfhi" },
	{ 0xfc00003f, 0x00000011, 2,  "mthi" },
	{ 0xfc00003f, 0x00000012, 2,  "mflo" },
	{ 0xfc00003f, 0x00000013, 2,  "mtlo" },
	{ 0xfc00003f, 0x00000018, 2,  "mult" },
	{ 0xfc00003f, 0x00000019, 2,  "multu" },
	{ 0xfc00003f, 0x0000001a, 2,  "div" },
	{ 0xfc00003f, 0x0000001b, 2,  "divu" },
	{ 0xfc00003f, 0x00000020, 2,  "add" },
	{ 0xfc00003f, 0x00000021, 2,  "addu" },
	{ 0xfc00003f, 0x00000022, 2,  "sub" },
	{ 0xfc00003f, 0x00000023, 2,  "subu" },
	{ 0xfc00003f, 0x00000024, 2,  "and" },
	{ 0xfc00003f, 0x00000025, 2, "or" },
	{ 0xfc00003f, 0x00000026, 2, "xor" },
	{ 0xfc00003f, 0x00000027, 2,  "nor" },
	{ 0xfc00003f, 0x0000002a, 2,  "slt" },
	{ 0xfc00003f, 0x0000002b, 2,  "sltu" },
	{ 0xfe00003f, 0x4a000001, 15,  "gte_rtps" },
	{ 0xfe00003f, 0x4a000006, 8,  "gte_nclip" },
	{ 0xfe00003f, 0x4a00000c, 6,  "gte_op" },
	{ 0xfe00003f, 0x4a000010, 8,  "gte_dpcs" },
	{ 0xfe00003f, 0x4a000011, 8,  "gte_intpl" },
	{ 0xfe00003f, 0x4a000012, 8,  "gte_mvmva" },
	{ 0xfe00003f, 0x4a000013, 19,  "gte_ncds" },
	{ 0xfe00003f, 0x4a000014, 13,  "gte_cdp" },
	{ 0xfe00003f, 0x4a000016, 44,  "gte_ncdt" },
	{ 0xfe00003f, 0x4a00001b, 17,  "gte_nccs" },
	{ 0xfe00003f, 0x4a00001c, 11,  "gte_cc" },
	{ 0xfe00003f, 0x4a00001e, 14,  "gte_ncs" },
	{ 0xfe00003f, 0x4a000020, 30,  "gte_nct" },
	{ 0xfe00003f, 0x4a000028, 5,  "gte_sqr" },
	{ 0xfe00003f, 0x4a000029, 8,  "gte_dcpl" },
	{ 0xfe00003f, 0x4a00002a, 17,  "gte_dpct" },
	{ 0xfe00003f, 0x4a00002d, 5,  "gte_avsz3" },
	{ 0xfe00003f, 0x4a00002e, 6,  "gte_avsz4" },
	{ 0xfe00003f, 0x4a000030, 23,  "gte_rtpt" },
	{ 0xfe00003f, 0x4a00003d, 5,  "gte_gpf" },
	{ 0xfe00003f, 0x4a00003e, 5,  "gte_gpl" },
	{ 0xfe00003f, 0x4a00003f, 39,  "gte_ncct" },
};

unsigned int R3000Dasm::Disassemble(u32 opcode, u32 pc, char* buffer, unsigned int bufferSizeBytes)
{
	// Decode opcode.
	// See https://problemkaputt.de/psx-spx.htm#cpuopcodeencoding
	
	// Primary opcode field is bits 31:26
	const u32 primaryOpcode = opcode >> 26;
	switch (primaryOpcode)
	{
		case 0x00:
		{
			// Decode SPECIAL opcodes from secondary opcode field bits 5:0");
			const u32 secondaryOpcode = opcode & 0x3f;
			switch (secondaryOpcode)
			{
				case 0x00: // SLL rd, rt, sa
				{
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
					SafeSnprintf(buffer, bufferSizeBytes, "sll $%u, $%u, %u", rd, rt, sa);
					break;
				}

				// 0x01 Reserved Instruction Exception

				case 0x02: // SRL rd, rt, sa
				{
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
					SafeSnprintf(buffer, bufferSizeBytes, "srl $%u, $%u, %u", rd, rt, sa);
					break;
				}

				case 0x03: // SRA rd, rt, sa
				{
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
					SafeSnprintf(buffer, bufferSizeBytes, "sra $%u, $%u, %u", rd, rt, sa);
					break;
				}

				case 0x04: // SLLV rd, rt, rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "sllv $%u, $%u, $%u", rd, rt, rs);
					break;
				}

				// 0x05 Reserved Instruction Exception

				case 0x06: // SRLV rd, rt, rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "srlv $%u, $%u, $%u", rd, rt, rs);
					break;
				}

				case 0x07: // SRAV rd, rt, rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "srav $%u, $%u, $%u", rd, rt, rs);
					break;
				}

				case 0x08: // JR rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					SafeSnprintf(buffer, bufferSizeBytes, "jr $%u", rs);
					break;
				}

				case 0x09: // JALR rd, rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "jalr $%u, $%u", rd, rs);
					break;
				}

				// 0x0A Reserved Instruction Exception
				// 0x0B Reserved Instruction Exception

				case 0x0C: // SYSCALL
				{
					SafeStrcpy(buffer, bufferSizeBytes, "SYSCALL");
					break;
				}

				case 0x0D: // BREAK
				{
					SafeStrcpy(buffer, bufferSizeBytes, "BREAK");
					break;
				}

				// 0x0E Reserved Instruction Exception
				// 0x0F Reserved Instruction Exception

				case 0x10: // MFHI rd
				{
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "mfhi $%u", rd);
					break;
				}

				case 0x11: // MTHI rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					SafeSnprintf(buffer, bufferSizeBytes, "mthi $%u", rs);
					break;
				}

				case 0x12: // MFLO rd
				{
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "mflo $%u", rd);
					break;
				}

				case 0x13: // MTLO rs
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					SafeSnprintf(buffer, bufferSizeBytes, "mtlo $%u", rs);
					break;
				}

				// 0x14 Reserved Instruction Exception
				// 0x15 Reserved Instruction Exception
				// 0x16 Reserved Instruction Exception
				// 0x17 Reserved Instruction Exception

				case 0x18: // MULT rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					SafeSnprintf(buffer, bufferSizeBytes, "mult $%u, $%u", rs, rt);
					break;
				}

				case 0x19: // MULTU rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					SafeSnprintf(buffer, bufferSizeBytes, "multu $%u, $%u", rs, rt);
					break;
				}

				case 0x1A: // DIV rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					SafeSnprintf(buffer, bufferSizeBytes, "div $%u, $%u", rs, rt);
					break;
				}

				case 0x1B: // DIVU rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					SafeSnprintf(buffer, bufferSizeBytes, "divu $%u, $%u", rs, rt);
					break;
				}

				// 0x1C Reserved Instruction Exception
				// 0x1D Reserved Instruction Exception
				// 0x1E Reserved Instruction Exception
				// 0x1F Reserved Instruction Exception

				case 0x20: // ADD rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "add $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x21: // ADDU rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "addu $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x22: // SUB rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "sub $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x23: // SUBU rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "subu $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x24: // AND rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "and $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x25: // OR rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "or $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x26: // XOR rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "xor $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x27: // NOR rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "nor $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				// 0x28 Reserved Instruction Exception
				// 0x29 Reserved Instruction Exception

				case 0x2A: // SLT rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "slt $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				case 0x2B: // SLTU rd, rs, rt
				{
					u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
					u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
					u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
					SafeSnprintf(buffer, bufferSizeBytes, "sltu $%u, $%u, $%u", rd, rs, rt);
					break;
				}

				// 0x2C to 0x3F Reserved Instruction Exception

				default:
				{
					// "Should never get here now, in good code anyway.
					HP_FATAL_ERROR("Reserved instruction exception, opcode: 0x%08x", opcode);
					break;
				}
			}

			break;
		}

		case 0x01:
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 func = (opcode >> 16) & 0x1f; // bits 20:16
			u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended
			u32 address = pc + 4 + (wordOffset << 2);

			switch (func)
			{
				case 0x00:
					SafeSnprintf(buffer, bufferSizeBytes, "bltz $%u, 0x%04X (= %08X)", rs, wordOffset, address);
					break;
				case 0x01:
					SafeSnprintf(buffer, bufferSizeBytes, "bgez $%u, 0x%04X (= %08X)", rs, wordOffset, address);
					break;
				case 0x10:
					SafeSnprintf(buffer, bufferSizeBytes, "bltzal $%u, 0x%04X (= %08X)", rs, wordOffset, address);
					break;
				case 0x11:
					SafeSnprintf(buffer, bufferSizeBytes, "bgezal $%u, 0x%04X (= %08X)", rs, wordOffset, address);
					break;
				default:
					HP_FATAL_ERROR("#TODO: Reserved Instruction Exception");
					break;
			}
			break;
		}

		case 0x02: // 0b000010 J target
		{
			u32 wordAddress = opcode & 0x3ffffff; // bits 25:0

			// Calculate target byte address.
			// 26 bits from opcode shifted left by 2 (opcodes are always word-aligned)
			// 4 most significant bits from PC remain in place
			u32 target = (pc & 0xf0000000) | (wordAddress << 2);

			SafeSnprintf(buffer, bufferSizeBytes, "j 0x%08x (= %08X)", wordAddress, target);
			break;
		}

		case 0x03: // 0b000011 JAL target
		{
			u32 wordAddress = opcode & 0x3ffffff; // bits 25:0

			// Calculate target address.
			// 26 bits from opcode shifted left by 2 (opcodes are always word-aligned)
			// 4 most significant bits from PC remain in place
			u32 target = (pc & 0xf0000000) | (wordAddress << 2);

			SafeSnprintf(buffer, bufferSizeBytes, "jal 0x%08x (= %08X)", wordAddress, target);
			break;
		}

		case 0x04: // 0b000101 BEQ rs, rt, offset
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended
			u32 address = pc + 4 + (wordOffset << 2);
			SafeSnprintf(buffer, bufferSizeBytes, "beq $%u, $%u, 0x%04x (= %08X)", rs, rt, wordOffset, address);
			break;
		}

		case 0x05: // 0b000101 BNE rs, rt, offset
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended
			u32 address = pc + 4 + (wordOffset << 2);
			SafeSnprintf(buffer, bufferSizeBytes, "bne $%u, $%u, 0x%04x (= %08X)", rs, rt, wordOffset, address);
			break;
		}

		case 0x06: // BLEZ rs, offset
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended
			u32 address = pc + 4 + (wordOffset << 2);
			SafeSnprintf(buffer, bufferSizeBytes, "blez $%u, 0x%04x (= %08X)", rs, wordOffset, address);
			break;
		}

		case 0x07: // BGTZ rs, offset
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended
			u32 address = pc + 4 + (wordOffset << 2);
			SafeSnprintf(buffer, bufferSizeBytes, "bgtz $%u, 0x%04x (= %08X)", rs, wordOffset, address);
			break;
		}

		case 0x08: // 0b001000 ADDI rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "addi $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x09: // 0b001001 ADDIU rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "addiu $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0A: // SLTI rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "slti $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0B: // SLTIU rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "sltiu $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0C: // ANDI rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "andi $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0D: // ORI rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "ori $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0E: // XORI rt, rs, immediate
		{
			u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "xori $%u, $%u, 0x%04x", rt, rs, imm16);
			break;
		}

		case 0x0F: // LUI rt, immediate
		{
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 imm16 = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lui $%u, 0x%04x", rt, imm16);
			break;
		}

		case 0x10: // COP0
		{
			// n.b. CFCz and CTCz are not valid for COP 0

			if ((opcode & 0xffe0003f) == 0x40000000)
			{
				// MFC0 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "mfc0 $%u, $cop0_%u", rt, rd);
			}
			else if ((opcode & 0xffe0003f) == 0x40800000)
			{
				// MTC0 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "mtc0 $%u, $cop0_%u", rt, rd);
			}
			else if ((opcode & 0xffe0003f) == 0x42000010)
			{
				SafeStrcpy(buffer, bufferSizeBytes, "rfe");
			}
			else
			{
				HP_FATAL_ERROR("Invalid instruction?");
			}

			break;
		}

		// 0x11 No COP1 on PSX. Reserved Instruction Exception?

		case 0x12: // COP2
		{
			if ((opcode & 0xffe0003f) == 0x48000000)
			{
				// MFC2 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "mfc2 $%u, $cop0_%u", rt, rd);
			}
			else if ((opcode & 0xffe0003f) == 0x48400000)
			{
				// CFC2 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "cfc2 $%u, $cop0_%u", rt, rd);
			}
			else if ((opcode & 0xffe0003f) == 0x48800000)
			{
				// MTC2 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "mtc2 $%u, $cop0_%u", rt, rd);
			}
			else if ((opcode & 0xffe0003f) == 0x48c00000)
			{
				// CTC2 rt, rd
				u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
				u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
				SafeSnprintf(buffer, bufferSizeBytes, "ctc2 $%u, $cop0_%u", rt, rd);
			}
			else
			{
				HP_FATAL_ERROR("Invalid instruction?");
			}

			break;
		}

		// 0x13 No COP3 on PSX. Reserved Instruction Exception?
		
		// 0x14 Reserved Instruction Exception
		// 0x15 Reserved Instruction Exception
		// 0x16 Reserved Instruction Exception
		// 0x17 Reserved Instruction Exception
		// 0x18 Reserved Instruction Exception
		// 0x19 Reserved Instruction Exception
		// 0x1A Reserved Instruction Exception
		// 0x1B Reserved Instruction Exception
		// 0x1C Reserved Instruction Exception
		// 0x1D Reserved Instruction Exception
		// 0x1E Reserved Instruction Exception
		// 0x1F Reserved Instruction Exception

		case 0x20: // LB rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lb $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x21: // LH rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lh $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x22: // LWL rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lwl $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x23: // LW rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lw $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x24: // LBU rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lbu $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x25: // LHU rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lhu $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x26: // LWR rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lwr $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		// 0x27 Reserved Instruction Exception

		case 0x28: // SB rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "sb $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x29: // SH rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "sh $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x2A: // SWL rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "swl $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x2B: // SW rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "sw $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		// 0x2C Reserved Instruction Exception
		// 0x2D Reserved Instruction Exception

		case 0x2E: // SWR rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "swr $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		// 0x2F Reserved Instruction Exception

		case 0x30: // LWC0 rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lwc0 $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x31:
		{
			HP_FATAL_ERROR("#TODO: LWC1"); // #TODO: No COP1 on PSX? Reserved Instruction Exception?
			break;
		}

		case 0x32: // LWC2 rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "lwc2 $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x33:
		{
			HP_FATAL_ERROR("#TODO: LWC3"); // #TODO: No COP3 on PSX? Reserved Instruction Exception?
			break;
		}

		// 0x34 Reserved Instruction Exception
		// 0x35 Reserved Instruction Exception
		// 0x36 Reserved Instruction Exception
		// 0x37 Reserved Instruction Exception

		case 0x38: // SWC0 rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "swc0 $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x39:
		{
			HP_FATAL_ERROR("#TODO: SWC1"); // #TODO: No COP1 on PSX? Reserved Instruction Exception?
			break;
		}

		case 0x3A: // SWC2 rt, offset(base)
		{
			u32 base = (opcode >> 21) & 0x1f; // bits 25:21
			u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
			u16 offset = (u16)opcode; // bits 15:0
			SafeSnprintf(buffer, bufferSizeBytes, "swc2 $%u, 0x%04x($%u)", rt, offset, base);
			break;
		}

		case 0x3B:
		{
			HP_FATAL_ERROR("#TODO: SWC3"); // #TODO: No COP3 on PSX? Reserved Instruction Exception?
			break;
		}

		// 0x3C Reserved Instruction Exception
		// 0x3D Reserved Instruction Exception
		// 0x3E Reserved Instruction Exception
		// 0x3F Reserved Instruction Exception

		default:
		{
			// "Should never get here now, in good code anyway.
			HP_FATAL_ERROR("Reserved instruction exception, opcode: 0x%08x", opcode);
			break;
		}
	}

	return 4; // All R3000 instructions are 4 bytes.
}
