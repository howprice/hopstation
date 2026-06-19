// Comments taken from "IDT R30xx Family Software Reference Manual" Revision 1.0 aka R3000.pdf

#include "R3000.h"

#include "core/ArrayHelpers.h"
#include "core/Helpers.h" // HP_UNUSED
#include "core/hp_assert.h"

#ifdef DEBUG
#include "core/Log.h"
#endif

//------------------------------------------------------------------------------------------------------------------------
// COP 0 register 12 Status Register flags
// #TODO: Consider implementing struct SR with union of u32 and struct of bitfields and removing this (see struct CauseRegister)

// Current Interrupt Enable  (0=Disable, 1=Enable)
#define SRF_IEC (1<<0)

// Isolate Cache (0=No, 1=Isolate)
// When isolated, all load and store operations are targeted to the Data cache, and never the main memory.
// (Used by PSX Kernel, in combination with Port FFFE0130h)
#define SRF_ISC (1<<16)

// "Boot Exception Vectors"
// When BEV == 1, the CPU uses the ROM (kseg1) space exception entry point (described in a later chapter).
// BEV is usually set to zero in running systems; this relocates the exception vectors to RAM addresses, speeding accesses and
// allowing the use of "user supplied" exception service routines.
#define SRF_BEV (1<<22)

// CU2 COP2 Enable (0=Disable, 1=Enable) (GTE in PSX)
#define SRF_CU2 (1<<30)

//------------------------------------------------------------------------------------------------------------------------

void R3000::Reset()
{
	m_fetchPC = kInitialPC;
	m_nextFetchPC = m_fetchPC + 4; // all MIPS opcodes are 32-bits
	m_PC = m_fetchPC;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_r); i++)
	{
		m_r[i] = 0;
	}

	m_hi = 0;
	m_lo = 0;

	// COP 0 registers
	m_tar = 0;
	m_BadVaddr = 0;
	m_sr = 0;
	m_branch = false;
	m_branchDelaySlot = false;
	m_cause = {};
	m_EPC = 0;

	m_delayedLoad = {};
	m_delayedLoadNext = {};

	m_gte.Reset();
}

void R3000::ExecuteInstruction()
{
	// Store address of current opcode for use in instructions that use it e.g. J, JAL, BEQ, BNE
	m_PC = m_fetchPC;

	if (m_PC & 3)
	{
		m_BadVaddr = m_PC;
		triggerException(ExcCode::AdEL);
		return;
	}

	// The next instruction in memory will be executed even if this instruction branches.
	// This instruction is said to be in the branch delay slot.
	m_fetchPC = m_nextFetchPC;

	// If no branch is taken, this will be the address of the next opcode.
	// If this instruction branches, then it can set next PC, and it will be executed
	// after the instruction in the branch delay slot
	m_nextFetchPC += 4; // All opcodes are 32-bits

	// If the previous instruction was a branch, then this is a branch delay slot.
	// Implementation note: triggerException needs m_branchDelaySlot to have been updated for correct behaviour if interrupt occurs in a branch delay slot. 
	m_branchDelaySlot = m_branch;
	m_branch = false;

	processDelayedLoads();

	// Fetch opcode
	u32 opcode = m_pReadWord(m_PC, m_userdata);

	// Process interrupts

	// To avoid glitches in games such as Crash, IRQs should not be processed if the current instruction is a GTE command.
	// https://psx-spx.consoledev.net/cpuspecifications/#interrupts-vs-gte-commands
	bool isCOP2 = (opcode & 0xFE000000) == 0x4A000000;
	if (!isCOP2)
	{
		//
		// "An active level on any [interrupt] pin is sensed in each cycle, and will cause an exception if enabled." - R3000.pdf. 
		//
		// An interrupt exception is executed if both:
		// - SR bit 0 (IEc Current Interrupt Enable) is set, and
		// - CAUSE IP pending bit set and corresponding SR IM bit set
		//
		// We use the full 8-bit field to check for software interrupts too.
		u32 im = (m_sr >> 8) & 0xff; // 6-bit hardware interrupt mask field (correponding to CAUSE IP field) + 2-bit software interrupt mask field, 
		u32 ip = (m_cause.val >> 8) & 0xff; // bits 15:8 (IP and WS fields) combined
		if ((m_sr & SRF_IEC) && (im & ip))
		{
			//		LOG_INFO("[CPU] %08X Triggering interrupt %s\n", m_PC, m_branchDelaySlot ? "in branch delay slot" : "");

			// For interrupts, the EPC stored is the address of the next instruction that would have been executed had the interrupt not occurred.
			// m_EPC <- m_PC
			// m_fetchPC = handlerAddress;
			// m_nextFetchPC = m_fetchPC + 4;
			triggerException(ExcCode::Int);

			// Return so pipeline can naturally advance m_PC <- m_fetchPC <- m_nextFetchPC.
			// This also give the host a chance to display the handle the first handler instruction before executing it (disassemble, breakpoint etc.)
			return;
		}
	}

	// Decode and execue
	// Primary opcode field is bits 31:26
	const u32 primaryOpcode = opcode >> 26;
	switch (primaryOpcode)
	{
		case 0x00:
		{
			const u32 secondaryOpcode = opcode & 0x3f;
			switch (secondaryOpcode)
			{
				case 0x00:
					executeSLL(opcode);
					break;
				case 0x02:
					executeSRL(opcode);
					break;
				case 0x03:
					executeSRA(opcode);
					break;
				case 0x04:
					executeSLLV(opcode);
					break;
				case 0x06:
					executeSRLV(opcode);
					break;
				case 0x07:
					executeSRAV(opcode);
					break;
				case 0x08:
					executeJR(opcode);
					break;
				case 0x09:
					executeJALR(opcode);
					break;
				case 0x0C:
					executeSYSCALL(opcode);
					break;
				case 0x0D:
					executeBREAK(opcode);
					break;
				case 0x10:
					executeMFHI(opcode);
					break;
				case 0x11:
					executeMTHI(opcode);
					break;
				case 0x12:
					executeMFLO(opcode);
					break;
				case 0x13:
					executeMTLO(opcode);
					break;
				case 0x18:
					executeMULT(opcode);
					break;
				case 0x19:
					executeMULTU(opcode);
					break;
				case 0x1A:
					executeDIV(opcode);
					break;
				case 0x1B:
					executeDIVU(opcode);
					break;
				case 0x20:
					executeADD(opcode);
					break;
				case 0x21:
					executeADDU(opcode);
					break;
				case 0x22:
					executeSUB(opcode);
					break;
				case 0x23:
					executeSUBU(opcode);
					break;
				case 0x24:
					executeAND(opcode);
					break;
				case 0x25:
					executeOR(opcode);
					break;
				case 0x26:
					executeXOR(opcode);
					break;
				case 0x27:
					executeNOR(opcode);
					break;
				case 0x2A:
					executeSLT(opcode);
					break;
				case 0x2B:
					executeSLTU(opcode);
					break;
				default:
					triggerException(ExcCode::RI); // Reserved Instruction
			}

			break;
		}

		case 0x01:
			executeBcondZ(opcode);
			break;
		case 0x02:
			executeJ(opcode);
			break;
		case 0x03:
			executeJAL(opcode);
			break;
		case 0x4:
			executeBEQ(opcode);
			break;
		case 0x05:
			executeBNE(opcode);
			break;
		case 0x06:
			executeBLEZ(opcode);
			break;
		case 0x07:
			executeBGTZ(opcode);
			break;
		case 0x08:
			executeADDI(opcode);
			break;
		case 0x09:
			executeADDIU(opcode);
			break;
		case 0x0A:
			executeSLTI(opcode);
			break;
		case 0x0B:
			executeSLTIU(opcode);
			break;
		case 0x0C:
			executeANDI(opcode);
			break;
		case 0x0D:
			executeORI(opcode);
			break;
		case 0x0E:
			executeXORI(opcode);
			break;
		case 0x0F:
			executeLUI(opcode);
			break;
		case 0x10: // COP0
		{
			// n.b. CFCz and CTCz are not valid for COP 0

			if ((opcode & 0xffe0003f) == 0x40000000)
				executeMFC0(opcode);
			else if ((opcode & 0xffe0003f) == 0x40800000)
				executeMTC0(opcode);
			else if ((opcode & 0xffe0003f) == 0x42000010)
				executeRFE(opcode);
			else
			{
				HP_FATAL_ERROR("Invalid instruction?");
			}
			break;
		}
		case 0x11:
		{
			// No COP1 (FPU) on PSX
			triggerException(ExcCode::CpU); // Coprocessor Unusable
			break;
		}
		case 0x12: // COP2 = PSX GTE
		{
			if ((opcode & 0xffe0003f) == 0x48000000)
				executeMFC2(opcode);
			else if ((opcode & 0xffe0003f) == 0x48400000)
				executeCFC2(opcode);
			else if ((opcode & 0xffe0003f) == 0x48800000)
				executeMTC2(opcode);
			else if ((opcode & 0xffe0003f) == 0x48c00000)
				executeCTC2(opcode);
			else if ((opcode & 0xfe000000) == 0x4a000000)
				executeCOP2(opcode);
			else
				HP_FATAL_ERROR("Invalid instruction?");
			break;
		}
		case 0x13:
		{
			// No COP3 (FPU) on PSX
			triggerException(ExcCode::CpU); // Coprocessor Unusable
			break;
		}
		case 0x20:
			executeLB(opcode);
			break;
		case 0x21:
			executeLH(opcode);
			break;
		case 0x22:
			executeLWL(opcode);
			break;
		case 0x23:
			executeLW(opcode);
			break;
		case 0x24:
			executeLBU(opcode);
			break;
		case 0x25:
			executeLHU(opcode);
			break;
		case 0x26:
			executeLWR(opcode);
			break;
		case 0x28:
			executeSB(opcode);
			break;
		case 0x29:
			executeSH(opcode);
			break;
		case 0x2A:
			executeSWL(opcode);
			break;
		case 0x2B:
			executeSW(opcode);
			break;
		case 0x2E:
			executeSWR(opcode);
			break;
		case 0x30:
			executeLWC0(opcode);
			break;
		case 0x31:
			executeLWC1(opcode);
			break;
		case 0x32:
			executeLWC2(opcode);
			break;
		case 0x33:
			executeLWC3(opcode);
			break;
		case 0x38:
			executeSWC0(opcode);
			break;
		case 0x39:
			executeSWC1(opcode);
			break;
		case 0x3A:
			executeSWC2(opcode);
			break;
		case 0x3B:
			executeSWC3(opcode);
			break;
		default:
			triggerException(ExcCode::RI); // Reserved Instruction
			break;
	}
}

void R3000::SetCallbacks(ReadByte* pReadByte, ReadHalfWord* pReadHalfWord, ReadWord* pReadWord, WriteByte* pWriteByte, WriteHalfWord* pWriteHalfWord, WriteWord* pWriteWord, void* userdata)
{
	m_pReadByte = pReadByte;
	m_pReadHalfWord = pReadHalfWord;
	m_pReadWord = pReadWord;
	m_pWriteByte = pWriteByte;
	m_pWriteHalfWord = pWriteHalfWord;
	m_pWriteWord = pWriteWord;
	m_userdata = userdata;
}

void R3000::SetPC(u32 pc)
{
	m_fetchPC = pc;
	m_nextFetchPC = pc + 4; // all instructions are word-sized
}

void R3000::SetGPR(unsigned int index, u32 val)
{
	// Enforce that GPR 0 is always zero by disallowing writes to it.
	if (index == 0)
		return;

	m_r[index] = val;

	// If an instruction in the load delay slot writes to the same register as the load,
	// cancel the delayed load.
	// i.e. The instruction in the delay slot takes priority.
	// https://jsgroth.dev/blog/posts/ps1-cpu/#load
	if (m_delayedLoad.registerIndex == index)
		m_delayedLoad = {};
}

void R3000::SetInterruptPins(unsigned int val)
{
	// The interrupt pin values can be read by the COP0 CAUSE register 6-bit IP field, so just store there now.
	m_cause.IP = val;
}

// Implements load delay
void R3000::setGprDelayed(unsigned int index, u32 val)
{
	// Enforce that GPR 0 is always zero by disallowing writes to it.
	if (index == 0)
		return;

	m_delayedLoadNext.registerIndex = index;
	m_delayedLoadNext.val = val;

	// #TODO: If two consecutive load instructions write to the same register, cancel the first load.
	// See https://jsgroth.dev/blog/posts/ps1-cpu/#load
	if (m_delayedLoad.registerIndex == index)
		m_delayedLoad = {};
}

void R3000::processDelayedLoads()
{
	if (m_delayedLoad.registerIndex != 0)
	{
		SetGPR(m_delayedLoad.registerIndex, m_delayedLoad.val);
		m_delayedLoad = {};
	}

	if (m_delayedLoadNext.registerIndex != 0)
	{
		m_delayedLoad = m_delayedLoadNext;
		m_delayedLoadNext = {};
	}
}

void R3000::triggerException(ExcCode excCode)
{
	// Update the 3-deep 2 bit wide KU/IE stack.
	// Bits shifted out are lost, and should be managed by the kernel software if required.
	// Zero bits are shifted into the bottom KUc/IEc bits which puts the CPU in kernel mode and disables interrupts.
	u32 bits = m_sr & 0x3f; // bottom six bits
	m_sr &= ~0x3f; // clear bottom six bits
	m_sr |= (bits << 2) & 0x3f;

	// Store the ExcCode in the Cause register.
	m_cause.excCode = excCode;

	// Store return address for this exception.
	if (m_branchDelaySlot)
	{
		// When an exception occurs within a branch delay slot, EPC should store the address of the
		// preceding branch instruction.
		m_EPC = m_PC - 4; // all instructions are 4 bytes

		// The BD bit (31) of the Cause register should also be set.
		m_cause.BD = true;

		// Update cop0r6 - TAR - Target Address register
		// When an exception occurs in the delay slot of a jump or branch (cop0r13.31=1), and
		// the branch is to be taken (or it's an unconditional jump) (cop0r13.30=1), this register is
		// updated to contain the destination address of the jump or branch.
		m_tar = m_nextFetchPC;
	}
	else
	{
		m_cause.BD = false;

		// Save the current PC in the EPC register
		m_EPC = m_PC;
	}

	// Note: BadVaddr should already be set by the caller for address error exceptions (AdEL/AdES)

	// Select handler address using SR BEV bit.
	// This is a program address (virtual address) rather than a physical address.
	// See R3000.pdf 4-3
	u32 handlerAddress = (m_sr & SRF_BEV) ? 0xbfc00180 : 0x80000080;

	// Exceptions don't have a branch delay.
	m_fetchPC = handlerAddress;
	m_nextFetchPC = m_fetchPC + 4;
}

// BLTZ, BLTZAL, BGEZ and BGEZAL
//
void R3000::executeBcondZ(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 func = (opcode >> 16) & 0x1f; // bits 20:16
	u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended

	// n.b. Displacement is relative to address of instruction following the one being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
	u32 address = m_fetchPC + (wordOffset << 2);

	s32 val = (s32)m_r[rs]; // signed

	// Amidog CPU tests depend on undocumented opcode decoding.

	// The official mappings are:
	//    $00: BLTZ
	//    $01: BGEZ
	//    $10: BLTZAL
	//    $11: BGEZAL

	// The actual mappings are:
	//   $10: BLTZAL
	//   $11: BGEZAL
	//   All other values: BLTZ if opcode bit 16 is clear (0bxxxx0), BGEZ if opcode bit 16 is set (0bxxxx1)
	//
	// https://jsgroth.dev/blog/posts/ps1-cpu/#unofficial-branch-opcodes

	bool branch = false;
	bool link = false;

	switch (func)
	{
		case 0x10: // BLTZAL
		{
			// Unconditionally, the address of the instruction after the delay slot is placed in the link register, r31.
			link = true;

			branch = (val < 0);
			break;
		}

		case 0x11: // BGEZAL
		{
			// Unconditionally, the address of the instruction after the delay slot is placed in the link register, r31.
			link = true;

			branch = (val >= 0);
			break;
		}
		default:
		{
			if ((func & 1) == 0) // BLTZ
				branch = (val < 0);
			else // BGEZ
				branch = (val >= 0);
			break;
		}
	}

	if (branch)
	{
		m_branch = true;
		m_nextFetchPC = address;
	}

	if (link)
		SetGPR(/*ra*/31, m_PC + 8);
}

// J target
// 
// The 26-bit target address is shifted left two bits and combined with the high-order bits of the
// address of the delay slot. The program unconditionally jumps to this calculated address with a
// delay of one instruction.
//
void R3000::executeJ(u32 opcode)
{
	u32 target = opcode & 0x03ffffff; // bits 25:0

	// 4 most significant bits from PC remain in place
	// Instructions are always 4-byte aligned, so shift up target by 2.
	// n.b. We set next fetch PC; The opcode at fetch PC will be executed first, then followed by
	// this one to emulate the branch delay slot.
	// n.b. PC used in calculation is address of instruction being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
	m_nextFetchPC = (m_PC & 0xf0000000) | (target << 2);

	m_branch = true;
}

// JAL target
//
// The 26-bit target address is shifted left two bits and combined with the high-order bits of the
// address of the delay slot. The program unconditionally jumps to this calculated address with a
// delay of one instruction. The address of the instruction after the delay slot is placed in the link
// register, r31.
//
// This instruction is used for subroutine calls.
//
void R3000::executeJAL(u32 opcode)
{
	u32 target = opcode & 0x03ffffff; // bits 25:0

	// 4 most significant bits from PC remain in place
	// Instructions are always 4-byte aligned, so shift up target by 2.
	// n.b. We set next PC rather than PC. The opcode at PC will be executed first, then followed by
	// this one to emulate the branch delay slot.
	// n.b. PC used in calculation is address of instruction being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
	m_nextFetchPC = (m_PC & 0xf0000000) | (target << 2);
	m_branch = true;

	SetGPR(31, m_PC + 8); // address of instruction *after* delay slot i.e. 2 instructions forward
}

// BEQ rs, rt, offset
//
// A branch target address is computed from the sum of the address of the instruction in the delay slot
// and the 16-bit offset, shifted left two bits and sign-extended. The contents of general register rs and
// the contents of general register rt are compared. If the two registers are equal, then the program
// branches to the target address, with a delay of one instruction.
void R3000::executeBEQ(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	if (m_r[rs] == m_r[rt])
	{
		m_branch = true;

		u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended

		// n.b. Displacement is relative to address of instruction following the one being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
		m_nextFetchPC = m_fetchPC + (wordOffset << 2);
	}
}

// BNE rs, rt, offset
//
// A branch target address is computed from the sum of the address of the instruction in the delay slot
// and the 16-bit offset, shifted left two bits and sign-extended. The contents of general register rs and
// the contents of general register rt are compared. If the two registers are not equal, then the program
// branches to the target address, with a delay of one instruction.
void R3000::executeBNE(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	if (m_r[rs] != m_r[rt])
	{
		m_branch = true;

		u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended

		// n.b. Displacement is relative to address of instruction following the one being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
		m_nextFetchPC = m_fetchPC + (wordOffset << 2);
	}
}

void R3000::executeBLEZ(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	if ((s32)m_r[rs] <= 0)
	{
		m_branch = true;

		u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended

		// Displacement is relative to address of instruction following the one being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
		m_nextFetchPC = m_fetchPC + (wordOffset << 2);
	}
}

// BGTZ rs, offset
//
// A branch target address is computed from the sum of the address of the instruction in the delay slot
// and the 16-bit offset, shifted left two bits and sign-extended. The contents of general register rs are
// compared to zero. If the contents of general register rs have the sign bit cleared and are not equal
// to zero, then the program branches to the target address, with a delay of one instruction.
//
void R3000::executeBGTZ(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	if ((s32)m_r[rs] > 0)
	{
		m_branch = true;

		u32 wordOffset = (s16)opcode; // bits 15:0, sign-extended

		// Displacement is relative to address of instruction following the one being executed. See https://problemkaputt.de/psx-spx.htm#cpuspecifications
		m_nextFetchPC = m_fetchPC + (wordOffset << 2);
	}
}

// ADDI rt, rs, immediate
//
// The 16-bit immediate is sign-extended and added to the contents of general register rs to form the
// result. The result is placed into general register rt.
// An overflow exception occurs if carries out of bits 30 and 31 differ (2’s complement overflow). The
// destination register rt is not modified when an integer overflow exception occurs.
//
void R3000::executeADDI(u32 opcode)
{
	// #TODO: "The destination register rt is not modified when an integer overflow exception occurs."
	//        Is this true? The "Operation" section in the docs for ADDI and ADDIU seem to be mixed up.

	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 imm = (s16)opcode; // bits 15:0, sign-extended
	u32 sum = m_r[rs] + imm;

	// Signed overflow in addition occurs when:
	// 1. the two operands have the same sign, and
	// 2. the result has a different sign than the operands.
	//
	//  ~(a ^ b) bit 31 is set iff the operands have the same sign.
	// (a ^ sum) bit 31 is set iff the result has a different sign than the operands.
	// https://stackoverflow.com/questions/3944505/detecting-signed-overflow-in-c-c
	if ((~(m_r[rs] ^ imm) & ((imm ^ sum))) & 0x80000000)
		triggerException(ExcCode::Ov);
	else
		SetGPR(rt, sum);
}

// ADDIU rt, rs, immediate
//
// The 16-bit immediate is sign-extended and added to the contents of general register rs to form the
// result. The result is placed into general register rt. No integer overflow exception occurs under any
// circumstances.
// 
// The only difference between this instruction and the ADDI instruction is that ADDIU never causes
// an overflow exception.
//
// This is a very badly named instruction, because the immediate value is interpreted as *signed* and
// sign-extended, not zero-extended. The R30xx reference manual has a confusing description of the
// operation of this instruction.
//
// There is no "load immediate" opcode, so use ADDIU with the $zero register instead.
//
void R3000::executeADDIU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 imm = (s16)opcode; // bits 15:0, sign-extended
	u32 result = m_r[rs] + imm;
	SetGPR(rt, result);
}

// SLTI rt, rs, immediate
//
void R3000::executeSLTI(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	s32 imm = (s16)opcode; // bits 15:0, sign-extended
	u32 val = (s32)m_r[rs] < imm ? 1 : 0;
	SetGPR(rt, val);
}

// SLTIU rt, rs, immediate
//
// The 16-bit immediate is sign-extended and subtracted from the contents of general register rs.
// Considering both quantities as unsigned integers, if rs is less than the sign-extended immediate, the
// result is set to one; otherwise the result is set to zero.
// 
// The result is placed into general register rt.
//
// No integer overflow exception occurs under any circumstances. The comparison is valid even if
// the subtraction used during the comparison overflows.
//
void R3000::executeSLTIU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 imm = (s16)opcode; // bits 15:0, sign-extended
	u32 val = m_r[rs] < imm ? 1 : 0;
	SetGPR(rt, val);
}

// ANDI rs, rt
//
// The 16-bit immediate is zero-extended and combined with the contents of general register rs in a bitwise
// logical AND operation. The result is placed into general register rt.
//
void R3000::executeANDI(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u16 imm16 = (u16)opcode; // bits 15:0
	u32 result = m_r[rs] & imm16;
	SetGPR(rt, result);
}

// MFC0 rt, rd
//
// The contents of coprocessor register rd of coprocessor z are loaded into general register rt.
//
// Subject to load delay:
// T:   data <- CPR[z,rd]
// T+1: GPR[rt] <- data
//
void R3000::executeMFC0(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	// https://psx-spx.consoledev.net/cpuspecifications/#cop0-register-summary
	switch (rd)
	{
		case 6: // TAR - Target Address (R)
			setGprDelayed(rt, m_tar);
			break;

		case 7: // DCIC - Debug and Cache Invalidate Control (R/W)
			// https://psx-spx.consoledev.net/cpuspecifications/#cop0r7-dcic-debug-and-cache-invalidate-control-rw
			setGprDelayed(rt, 0);
			break;

		case 8: // BadA aka BadVaddr - Bad Virtual Address (R)
			setGprDelayed(rt, m_BadVaddr);
			break;

		case 12: // SR - Status Register
			setGprDelayed(rt, m_sr);
			break;

		case 13: // CAUSE - Cause of the last exception
			setGprDelayed(rt, m_cause.val);
			break;

		case 14: // EPC - Exception Program Counter
			setGprDelayed(rt, m_EPC);
			break;

		case 15: // PRID - Processor ID (read-only)
			// 0-7   Revision
			// 8-15  Implementation
			// 16-31 Not used
			// PRID=00000001h on Playstation with CPU CXD8530BQ/CXD8530CQ
			// PRID=00000002h on Playstation with CPU CXD8606CQ
			setGprDelayed(rt, 0x00000002); // #TODO: Is this correct?

			break;

		default:
			HP_FATAL_ERROR("Unhandled read from COP0 register %u", rd);
	}
}

// MTC0 rt, rd
//
// The contents of general register rt are loaded into coprocessor register rd of coprocessor 0;
//
void R3000::executeMTC0(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	u32 val = m_r[rt];
	switch (rd)
	{
		// Breakpoint registers
		case 3:
		case 5:
		case 6:
		case 7:
		case 9:
		case 11:
			HP_ASSERT(val == 0, "Unhandled non-zero write to breakpoint register");
			break;

		case 12:
		{
#if GTE_TRACE_ENABLED
			const u32 sr_prev = m_sr;
			u32 changed = val ^ sr_prev;
			if (changed & SRF_CU2)
				LOG_INFO("[CPU] COP2 GTE %s\n", (val & SRF_CU2) ? "enabled" : "disabled");
#endif
			m_sr = val;
			break;
		}

		case 13:
		{
			// Only allow writes to bits 8 and 9 (IP0 and IP1) of Cause register
			// #TODO: Verify this is correct behaviour and find documentation.
			u32 mask = 0x00000300;
			m_cause.val = (m_cause.val & ~mask) | (val & mask);
			break;
		}

		default:
			HP_FATAL_ERROR("Unhandled write to COP0 register %u value %08X", rd, val);
	}
}

//
// COP 0 instruction
//
// RFE restores the "previous" interrupt enable mask bit and kernel/user mode bit (IEp and KUp) of
// the Status Register into the corresponding "current" status bits (IEc and KUc), and restores the
// "old" Status bits (IEo and KUo) into the corresponding "previous" status bits (IEp and KUp). The
// "old" status bits remain unchanged.
// 
// The MIPS architecture does not specify the operation of memory references associated with load/
// store instructions immediately prior to an RFE instruction. Normally, the RFE instruction follows
// in the delay slot of a JR instruction to restore the PC.
// 
// RFE does not affect the PC - the BIOS must do that.
// RFE is typically in the final branch delay slot of an exception handler i.e. after JR instruction.
//
void R3000::executeRFE(u32 /*opcode*/)
{
	// Restore the interrupt enable and kernel/user mode bits
	// Shift the 3-deep stack back down
	
	u32 mode = m_sr & 0x3f; // get bottom 6 bits
	m_sr &= ~0xf; // Clear bottom *4* bits. n.b. Bits 5:4 (KUo/IEo) are unchanged. https://psx-spx.consoledev.net/cpuspecifications/#cop0r12-sr-system-status-register-rw
	m_sr |= (mode >> 2); // restore previous mode
}

// MFC2 rt, rd
//
// rt = cop2datRd ;data regs
//
// The contents of coprocessor register rd of coprocessor 2 (GTE) are loaded into general register rt.
//
// // The contents of coprocessor register rd of coprocessor z are loaded into general register rt.
//
// Subject to load delay:
// T:   data <- CPR[z,rd]
// T+1: GPR[rt] <- data
//
void R3000::executeMFC2(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 val = m_gte.ReadDataReg(rd);
	setGprDelayed(rt, val);
}

// MTC2 rt, rd
//
// Move To Coprocessor
// 
// The contents of general register rt are loaded into coprocessor register rd of coprocessor 2 (GTE).
//
// cop2datRd = rt ;data regs
//
// Store delay:
// T:   data <- GPR[rt]
// T+1: CPR[z,rd] <- data
//
void R3000::executeMTC2(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	u32 val = m_r[rt];
	// #TODO: Implement store delay
	m_gte.WriteDataReg(rd, val);
}

// CFC2 rt,rd       ;rt = cop2cntRd ;control regs
//
// The contents of coprocessor control register rd of coprocessor 2 (GTE) are loaded into general register rt.
//
// This does not exist for COP0 i.e. there is no CFC0
//
// Subject to load delay:
// T:   data <- CCR[z,rd]
// T+1: GPR[rt] <- data
//
void R3000::executeCFC2(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	u32 val = m_gte.ReadControlReg(rd);
	setGprDelayed(rt, val);
}

// CTC2 rt, rd
//
// Move Control to Coprocessor
//
// cop2cntRd = rt ;control regs
//
// The contents of general register rt are loaded into *control* register rd of coprocessor 2 (GTE)
//
// This instruction is not valid for CP0.
//
// Store delay:
// T:   data <- GPR[rt]
// T+1: CCR[z, rd] <- data
//
void R3000::executeCTC2(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	// #TODO: Store delay. See R3000.pdf CTCz
	m_gte.WriteControlRegister(rd, m_r[rt]);
}

// COPz cofun
// COP2 cofun
//
// cop2 imm25       ;exec cop# command 0..1FFFFFFh (25-bit)
// 
// A coprocessor operation is performed. The operation may specify and reference internal
// coprocessor registers, and may change the state of the coprocessor condition line, but does not
// modify state within the processor or the cache/memory system. Details of coprocessor operations
// are contained in other appendices.
//
void R3000::executeCOP2(u32 opcode)
{
	HP_ASSERT(m_sr & SRF_CU2, "#TODO: Coprocessor unusable exception if GTE is not enabled");

	u32 cofun = opcode & 0x01ffffff; // bits 24:0
	m_gte.ExecuteCommand(cofun);
}

// LB rt, offset(base)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of the byte at the memory location specified by the effective address are signextended
// and loaded into general register rt.
//
void R3000::executeLB(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	u32 mem = (s8)m_pReadByte(address, m_userdata); // sign-extended
	setGprDelayed(rt, mem);
}

// LH rt, offset(base)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of the halfword at the memory location specified by the effective address are
// sign-extended and loaded into general register rt.
// 
// If the least-significant bit of the effective address is non-zero, an address error exception occurs.
//
void R3000::executeLH(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 va = m_r[base] + offset;
	if (va & 1)
	{
		// Mis-aligned load
		m_BadVaddr = va;
		triggerException(ExcCode::AdEL);
		return;
	}

	u32 mem = (s16)m_pReadHalfWord(va, m_userdata); // sign-extended
	setGprDelayed(rt, mem);
}

// LW rt, offset(base)
// 
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of the word at the memory location specified by the effective address are
// loaded into general register rt. In 64-bit mode, the loaded word is sign-extended.
//
void R3000::executeLW(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	// If either of the two least-significant bits of the effective address is non-zero, an address error
	// exception occurs.
	if (address & 3)
	{
		// Mis-aligned load
		m_BadVaddr = address;
		triggerException(ExcCode::AdEL);
		return;
	}

	u32 mem = m_pReadWord(address, m_userdata);
	setGprDelayed(rt, mem);
}

// LBU rt, offset(base)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of the byte at the memory location specified by the effective address are zeroextended
// and loaded into general register rt.
//
void R3000::executeLBU(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	u32 mem = (u32)m_pReadByte(address, m_userdata); // zero-extended
	setGprDelayed(rt, mem);
}

// LHU rt, offset(base)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of the halfword at the memory location specified by the effective address are
// zero-extended and loaded into general register rt.
//
void R3000::executeLHU(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read from data cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	// If the least-significant bit of the effective address is non-zero, an address error exception occurs.
	if (address & 1)
	{
		// Mis-aligned load
		m_BadVaddr = address;
		triggerException(ExcCode::AdEL);
		return;
	}

	u32 val = m_pReadHalfWord(address, m_userdata); // zero-extended
	setGprDelayed(rt, val);
}

// LWL rt, offset(base)
//
// Load Word Left
//
// This instruction can be used in combination with the LWR instruction to load a register with four
// consecutive bytes from memory, when the bytes cross a word boundary. LWL loads the left
// portion of the register with the appropriate part of the high-order word; LWR loads the right
// portion of the register with the appropriate part of the low-order word.
//
// The LWL instruction adds its sign-extended 16-bit offset to the contents of general register base to
// form a virtual address which can specify an arbitrary byte. It reads bytes only from the word in
// memory which contains the specified starting byte. From one to four bytes will be loaded,
// depending on the starting byte specified.
//
// Conceptually, it starts at the specified byte in memory and loads that byte into the high-order (leftmost)
// byte of the register; then it loads bytes from memory into the register until it reaches the loworder
// byte of the word in memory. The least-significant (right-most) byte(s) of the register will not
// be changed.
//
// The contents of general register rt are internally bypassed within the processor so that no NOP is
// needed between an immediately preceding load instruction which specifies register rt and a
// following LWL (or LWR) instruction which also specifies register rt.
// 
// No address exceptions due to alignment are possible.
// 
// n.b. This is little-endian only implementation, as required by PSX.
//
void R3000::executeLWL(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 va = m_r[base] + offset;

	// LWL and LWR can read in-flight delayed loads.
	u32 val = (m_delayedLoad.registerIndex == rt) ? m_delayedLoad.val : m_r[rt];

	// Load aligned word
	u32 alignedVA = va & ~3;
	u32 mem = m_pReadWord(alignedVA, m_userdata);

	// This is the implementation from the PlayStation Emulation Guide
	// It doesn't look right to me, but it passes the Amidog CPU test!
	// #TODO: Understand this.
	// 
	// Set the leftmost 1, 2, 3 or 4 bytes depending on offset into word.
	u32 byteOffset = va & 3;
	if (byteOffset == 0)
		val = (val & 0x00ffffff) | (mem << 24);
	else if (byteOffset == 1)
		val = (val & 0x0000ffff) | (mem << 16);
	else if (byteOffset == 2)
		val = (val & 0x000000ff) | (mem << 8);
	else // byteOffset == 3
		val = mem;

	// Load is delayed, but subseqent LWR may read-this in-flight value;
	setGprDelayed(rt, val);
}

// LWR rt, offset(base)
//
// This instruction can be used in combination with the LWL instruction to load a register with four
// consecutive bytes from memory, when the bytes cross a word boundary. LWR loads the right
// portion of the destination register rt with the appropriate part of the low-order word; LWL loads
// the left portion of the register with the appropriate part of the high-order word.
// 
// The LWR instruction adds its sign-extended 16-bit offset to the contents of general register base to
// form a virtual address which can specify an arbitrary byte. It loads bytes only from the word in
// memory which contains the specified starting byte. From one to four bytes will be merged into the
// destination register rt, depending on the starting byte specified.
// 
// Conceptually, it starts at the specified byte in memory and loads that byte into the low-order (rightmost)
// byte of the register; then it loads bytes from memory into the register until it reaches the highorder
// byte of the word in memory. The most significant (left-most) byte(s) of the register will not
// be changed.
// 
// The contents of general register rt are internally bypassed within the processor so that no NOP is
// needed between an immediately preceding load instruction which specifies register rt and a
// following LWR (or LWL) instruction which also specifies register rt.
//
void R3000::executeLWR(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 va = m_r[base] + offset;

	// LWL and LWR can read in-flight delayed loads.
	u32 val = (m_delayedLoad.registerIndex == rt) ? m_delayedLoad.val : m_r[rt];

	// Load aligned word
	u32 alignedVA = va & ~3;
	u32 mem = m_pReadWord(alignedVA, m_userdata);

	// This is the implementation from the PlayStation Emulation Guide
	// It doesn't look right to me, but it passes the Amidog CPU test!
	// #TODO: Understand this.
	//
	// Set the rightmost 1, 2, 3 or 4 bytes depending on offset into word.
	u32 byteOffset = va & 3;
	if (byteOffset == 0)
		val = mem;
	else if (byteOffset == 1)
		val = (val & 0xff000000) | (mem >> 8);
	else if (byteOffset == 2)
		val = (val & 0xffff0000) | (mem >> 16);
	else if (byteOffset == 3)
		val = (val & 0xffffff00) | (mem >> 24);

	// Load is delayed, but subseqent LWL may read-this in-flight value;
	setGprDelayed(rt, val);
}

// SB rt, offset(base)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The least-significant byte of register rt is stored at the effective address.
void R3000::executeSB(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore writes while cache is isolated
		// #TODO: Write data to cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;
	u8 val = (u8)m_r[rt];
	m_pWriteByte(address, val, m_userdata);
}

// SH rt, offset(base)
// 
// Store halfword (16-bit value)
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form an
// unsigned effective address. The least-significant halfword of register rt is stored at the effective
// address. If the least-significant bit of the effective address is non-zero, an address error exception
// occurs.
//
void R3000::executeSH(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore writes while cache is isolated
		// #TODO: Write to data cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	// If the least-significant bit of the effective address is non-zero, an address error exception occurs.
	if (address & 1)
	{
		// Mis-aligned store
		m_BadVaddr = address;
		triggerException(ExcCode::AdES);
		return;
	}

	u16 val = (u16)m_r[rt];
	m_pWriteHalfWord(address, val, m_userdata);
}

// LUI rt, immediate
// The 16-bit immediate is shifted left 16 bits and concatenated with 16 bits of low-order zeros. The
// 32-bit result is then placed into general register rt.
void R3000::executeLUI(u32 opcode)
{
	// 16-bit immediate data in bits 15:0
	u16 imm16 = (u16)opcode;

	// register index in bits 20:16
	u32 rt = (opcode >> 16) & 0x1f;

	u32 result = imm16 << 16;
	SetGPR(rt, result);
}

// ORI rt, rs, immediate
// 
// The 16-bit immediate is zero-extended and combined with the contents of general register rs in a bitwise
// logical OR operation. The result is placed into general register rt.
//
void R3000::executeORI(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u16 imm16 = (u16)opcode; // bits 15:0
	u32 result = m_r[rs] | imm16;
	SetGPR(rt, result);
}

// XORI rt, rs, immediate
// 
// The 16-bit immediate is zero-extended and combined with the contents of general register rs in a bitwise
// logical exclusive OR operation.
// The result is placed into general register rt.
//
void R3000::executeXORI(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u16 imm16 = (u16)opcode; // bits 15:0
	u32 result = m_r[rs] ^ imm16;
	SetGPR(rt, result);
}

// SW rt, offset(base)
//
// Store word (32-bit value)
// 
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The contents of general register rt are stored at the memory location specified by the
// effective address.
//
void R3000::executeSW(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore writes while cache is isolated
		// #TODO: Write to data cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	// If either of the two least-significant bits of the effective address are non-zero, an address error
	// exception occurs.
	if (address & 3)
	{
		// Mis-aligned store
		m_BadVaddr = address;
		triggerException(ExcCode::AdES);
		return;
	}

	m_pWriteWord(address, m_r[rt], m_userdata);
}

// SWL rt, offset(base)
//
// This instruction can be used with the SWR instruction to store the contents of a register into four
// consecutive bytes of memory, when the bytes cross a word boundary. SWL stores the left portion
// of the register into the appropriate part of the high-order word of memory; SWR stores the right
// portion of the register into the appropriate part of the low-order word.
// 
// The SWL instruction adds its sign-extended 16-bit offset to the contents of general register base to
// form a virtual address which may specify an arbitrary byte. It alters only the word in memory
// which contains that byte. From one to four bytes will be stored, depending on the starting byte
// specified.
// 
// Conceptually, it starts at the most-significant byte of the register and copies it to the specified byte
// in memory; then it copies bytes from register to memory until it reaches the low-order byte of the
// word in memory.
//
// No address exceptions due to alignment are possible.
//
// #TODO: Test this
//
void R3000::executeSWL(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore writes while cache is isolated
		// #TODO: Write to data cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 va = m_r[base] + offset;

	// Load aligned word
	u32 alignedVA = va & ~3;
	u32 mem = m_pReadWord(alignedVA, m_userdata);

	u32 reg = m_r[rt];

	// This is the implementation from the PlayStation Emulation Guide, but it doesn't look right to me!
	u32 result;
	u32 byteOffset = va & 3;
	if (byteOffset == 0)
		result = (mem & 0xffffff00) | (reg >> 24);
	else if (byteOffset == 1)
		result = (mem & 0xffff0000) | (reg >> 16);
	else if (byteOffset == 2)
		result = (mem & 0xff000000) | (reg >> 8);
	else // (byteOffset == 3)
		result = reg;

	m_pWriteWord(alignedVA, result, m_userdata);
}

// SWR rt, offset(base)
//
// This instruction can be used with the SWL instruction to store the contents of a register into four
// consecutive bytes of memory, when the bytes cross a boundary between two words. SWR stores
// the right portion of the register into the appropriate part of the low-order word; SWL stores the left
// portion of the register into the appropriate part of the low-order word of memory.
// 
// The SWR instruction adds its sign-extended 16-bit offset to the contents of general register base to
// form a virtual address which may specify an arbitrary byte. It alters only the word in memory
// which contains that byte. From one to four bytes will be stored, depending on the starting byte
// specified.
// 
// Conceptually, it starts at the least-significant (rightmost) byte of the register and copies it to the
// specified byte in memory; then copies bytes from register to memory until it reaches the high-order
// byte of the word in memory.
// 
// No address exceptions due to alignment are possible.
//
// #TODO: Test this
//
void R3000::executeSWR(u32 opcode)
{
	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore writes while cache is isolated
		// #TODO: Write to data cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 va = m_r[base] + offset;

	// Load aligned word
	u32 alignedVA = va & ~3;
	u32 mem = m_pReadWord(alignedVA, m_userdata);

	u32 reg = m_r[rt];

	// This is the implementation from the PlayStation Emulation Guide, but it doesn't look right to me!
	u32 result;
	u32 byteOffset = va & 3;
	if (byteOffset == 0)
		result = reg;
	else if (byteOffset == 1)
		result = (mem & 0x000000ff) | (reg << 8);
	else if (byteOffset == 2)
		result = (mem & 0x0000ffff) | (reg << 16);
	else // (byteOffset == 3)
		result = (mem & 0x00ffffff) | (reg << 24);

	m_pWriteWord(alignedVA, result, m_userdata);
}

void R3000::executeLWC0(u32 /*opcode*/)
{
	// Not supported by CP0
	triggerException(ExcCode::CpU); 
}

void R3000::executeLWC1(u32 /*opcode*/)
{
	// No COP1 (FPU) on PSX
	triggerException(ExcCode::CpU); // Coprocessor Unusable
}

// LWCz Load Word To Coprocessor
//
// LWCz rt, offset(base)
// 
// LWC2 rt,imm(rs)  ;cop2dat_rt = [rs+imm]  ;word
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. The processor reads a word from the addressed memory location, and makes the data
// available to coprocessor 2 (GTE).
// 
// The manner in which each coprocessor uses the data is defined by the individual coprocessor
// specifications.
// 
// If either of the two least-significant bits of the effective address is non-zero, an address error
// exception occurs.
//
void R3000::executeLWC2(u32 opcode)
{
	HP_ASSERT(m_sr & SRF_CU2, "#TODO: Coprocessor unusable exception if GTE is not enabled");

	if (m_sr & SRF_ISC)
	{
		// #TEMP: Ignore reads while cache is isolated
		// #TODO: Read data from cache rather than main memory.
		return;
	}

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	if (address & 3)
	{
		// Mis-aligned load
		m_BadVaddr = address;
		triggerException(ExcCode::AdEL);
		return;
	}

	// #TODO: Implement LWC2 delay
	u32 mem = m_pReadWord(address, m_userdata);
	m_gte.WriteDataReg(rt, mem);
}

void R3000::executeLWC3(u32 /*opcode*/)
{
	// No COP3 (FPU) on PSX
	triggerException(ExcCode::CpU); // Coprocessor Unusable
}

void R3000::executeSWC0(u32 /*opcode*/)
{
	// Not supported by CP0
	triggerException(ExcCode::CpU);
}

void R3000::executeSWC1(u32 /*opcode*/)
{
	// No COP1 (FPU) on PSX
	triggerException(ExcCode::CpU); // Coprocessor Unusable
}

// SWCz Store Word From Coprocessor
// 
// SWCz rt, offset(base)
// 
// SWC2 rt,imm(rs)  ;[rs+imm] = cop#dat_rt  ;word
//
// The 16-bit offset is sign-extended and added to the contents of general register base to form a virtual
// address. Coprocessor unit z sources a word, which the processor writes to the addressed memory
// location.
// 
// The data to be stored is defined by individual coprocessor specifications.
// 
// If either of the two least-significant bits of the effective address is non-zero, an address error
// exception occurs.
// 
void R3000::executeSWC2(u32 opcode)
{
	HP_ASSERT(m_sr & SRF_CU2, "#TODO: Coprocessor unusable exception if GTE is not enabled");

	u32 base = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 offset = (s16)opcode; // bits 15:0, sign-extended
	u32 address = m_r[base] + offset;

	if (address & 3)
	{
		// Mis-aligned load
		m_BadVaddr = address;
		triggerException(ExcCode::AdEL);
		return;
	}

	u32 val = m_gte.ReadDataReg(rt);
	// #TODO: Implement SWC2 store delay
	m_pWriteWord(address, val, m_userdata);
}

void R3000::executeSWC3(u32 /*opcode*/)
{
	// No COP3 (FPU) on PSX

	// Execution of the instruction referencing coprocessor 3 causes a reserved instruction exception, not
	// a coprocessor unusable exception.
	HP_FATAL_ERROR("Not tested");
	triggerException(ExcCode::RI); // Reserved Instruction
}

// SPECIAL

// SLL rd, rt, sa
// The contents of the low-order word of general register rt are shifted left by sa bits, inserting zeros
// into the low-order bits. The word result is placed in register rd.
void R3000::executeSLL(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
	u32 result = m_r[rt] << sa;
	SetGPR(rd, result);
}

// SRL rd, rt, sa
//
// The low-order word of general register rt is shifted right by sa bits, inserting zeros into the highorder
// bits. The result is placed in register rd.
//
void R3000::executeSRL(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
	u32 result = m_r[rt] >> sa;
	SetGPR(rd, result);
}

// SRA rd, rt, sa
//
// The contents of the low-order word of general register rt are shifted right by sa bits, sign-extending
// the high-order bits. The result is placed in register rd.
//
void R3000::executeSRA(u32 opcode)
{
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 sa = (opcode >> 6) & 0x1f; // bits 10:6
	u32 result = (s32)m_r[rt] >> sa;
	SetGPR(rd, result);
}

// SLLV rd, rt, rs
//
// The contents of the low-order word of general register rt are shifted left the number of bits specified
// by the low-order five bits contained in general register rs, inserting zeros into the low-order bits.
// The word-value result is placed in register rd.
//
void R3000::executeSLLV(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rt] << (m_r[rs] & 0x1f);
	SetGPR(rd, result);
}

// SRLV rd, rt, rs
//
// The low-order word of general register rt are shifted right by the number of bits specified by the
// low-order five bits of general register rs, inserting zeros into the high-order bits.
// The result is placed in register rd.
//
void R3000::executeSRLV(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rt] >> (m_r[rs] & 0x1f);
	SetGPR(rd, result);
}

// SRAV rd, rt, rs
//
// The contents of general register rt are shifted right by the number of bits specified by the low-order
// five bits of general register rs, sign-extending the high-order bits.
// The result is placed in register rd.
//
void R3000::executeSRAV(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 shift = m_r[rs] & 0x1f;
	u32 result = (s32)m_r[rt] >> shift;
	SetGPR(rd, result);
}

// JR rs
//
// The program unconditionally jumps to the address contained in general register rs, with a delay of
// one instruction.
// 
// Since instructions must be word-aligned, a Jump Register instruction must specify a target register
// (rs) whose two low-order bits are zero. If these low-order bits are not zero, an address exception
// will occur when the jump target instruction is subsequently fetched.
// 
// Subroutine return is normally done with jr $31.
//
void R3000::executeJR(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21

	// Note: No address error at this point. If new fetch PC is no word-aligned then will occur on next fetch.

	// n.b. We set next fetch PC; The opcode at fetch PC will be executed first, then followed by
	// this one to emulate the branch delay slot.
	m_nextFetchPC = m_r[rs];
	m_branch = true;
}

// JALR rd, rs
//
// The program unconditionally jumps to the address contained in general register rs, with a delay of
// one instruction. The address of the instruction after the delay slot is placed in general register rd.
// The default value of rd, if omitted in the assembly language instruction, is 31.
// 
// Register specifiers rs and rd may not be equal, because such an instruction does not have the same
// effect when re-executed. However, an attempt to execute this instruction is not trapped, and the
// result of executing such an instruction is undefined.
//
void R3000::executeJALR(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11

	// Note: No address error at this point. If new fetch PC is no word-aligned then will occur on next fetch.

	// n.b. We set next fetch PC; The opcode at fetch PC will be executed first, then followed by
	// this one to emulate the branch delay slot.
	m_nextFetchPC = m_r[rs];
	m_branch = true;

	SetGPR(rd, m_PC + 8);
}

void R3000::executeSYSCALL(u32 /*opcode*/)
{
	triggerException(ExcCode::Syscall);
}

void R3000::executeBREAK(u32 /*opcode*/)
{
	triggerException(ExcCode::Bp);
}

// MFHI rd
void R3000::executeMFHI(u32 opcode)
{
	// #TODO: Should stall if MUL/DIV instruction is not complete.

	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	SetGPR(rd, m_hi);
}

// MTHI rs
void R3000::executeMTHI(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	m_hi = m_r[rs];
}

// MFLO rd
void R3000::executeMFLO(u32 opcode)
{
	// #TODO: Should stall if MUL/DIV instruction is not complete.

	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	SetGPR(rd, m_lo);
}

// MTLO rs
void R3000::executeMTLO(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	m_lo = m_r[rs];
}

// MULT rs, rt
//
// The contents of general registers rs and rt are multiplied, treating both operands as 32-bit 2’s
// complement values. No integer overflow exception occurs under any circumstances.
//	
// When the operation completes, the low-order word of the double result is loaded into special
// register LO, and the high-order word of the double result is loaded into special register HI.
// 
// #TODO: If either of the two preceding instructions is MFHI or MFLO, the results of these instructions are
// undefined. Correct operation requires separating reads of HI or LO from writes by a minimum of
// two other instructions.
//
// #TODO: Improve the timing of this function. See R3000.pdf
// 
void R3000::executeMULT(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	s64 a = (s32)m_r[rs]; // sign extend
	s64 b = (s32)m_r[rt]; // sign extend
	u64 result = (u64)(a * b);
	m_lo = (u32)result;
	m_hi = (result >> 32);
}

// MULTU rs, rt
//
// The contents of general register rs and the contents of general register rt are multiplied, treating
// both operands as unsigned values. No overflow exception occurs under any circumstances.
// 
// When the operation completes, the low-order word of the double result is loaded into special
// register LO, and the high-order word of the double result is loaded into special register HI.
//
// #TODO: If either of the two preceding instructions is MFHI or MFLO, the results of these instructions are
// undefined. Correct operation requires separating reads of HI or LO from writes by a minimum of
// two instructions.
//
// #TODO: Improve the timing of this function. See R3000.pdf
// 
void R3000::executeMULTU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u64 result = (u64)m_r[rs] * (u64)m_r[rt];
	m_lo = (u32)result;
	m_hi = (result >> 32);
}

// DIV rs, rt
//
// Division by zero does not cause an exception.
//
// #TODO: This should take > 1 cycle to complete.
//
void R3000::executeDIV(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16

	s32 numerator = (s32)m_r[rs];
	s32 denominator = (s32)m_r[rt];

	if (denominator == 0)
	{
		m_hi = (u32)numerator;

		if (numerator >= 0)
			m_lo = 0xffffffff;
		else
			m_lo = 1;
	}
	else if ((u32)numerator == 0x80000000 && denominator == -1)
	{
		// -INT_MIN too large to store in 32-bits
		// This is encoded as:
		m_hi = 0;
		m_lo = 0x80000000;
	}
	else
	{
		m_hi = (u32)(numerator % denominator);
		m_lo = (u32)(numerator / denominator);
	}
}

// DIVU rs, rt
//
// #TODO: This should take > 1 cycle to complete.
//
void R3000::executeDIVU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16

	u32 numerator = m_r[rs];
	u32 denominator = m_r[rt];

	if (denominator == 0) // division by zero
	{
		m_hi = numerator;
		m_lo = 0xffffffff;
	}
	else
	{
		m_hi = numerator % denominator;
		m_lo = numerator / denominator;
	}
}

// ADD rd, rs, rt
//
// Add two 32-bit values and produce a 32-bit result; arithmetic overflow causes an exception
//
// The word value in general register rt is added to the word value in general register rs and the result
// word value is placed into general register rd.If the addition results in 32-bit 2’s complement
// arithmetic overflow (carries out of bits 30 and 31 differ) then the destination register rd is not
// modified and an integer overflow exception occurs.
//
void R3000::executeADD(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 sum = m_r[rs] + m_r[rt];

	// Signed overflow in addition occurs when:
	// 1. the two operands have the same sign, and
	// 2. the result has a different sign than the operands.
	//
	//  ~(a ^ b) bit 31 is set iff the operands have the same sign.
	// (a ^ sum) bit 31 is set iff the result has a different sign than the operands.
	// https://stackoverflow.com/questions/3944505/detecting-signed-overflow-in-c-c
	if ((~(m_r[rs] ^ m_r[rt]) & ((m_r[rt] ^ sum))) & 0x80000000)
		triggerException(ExcCode::Ov);
	else
		SetGPR(rd, sum);
}

// ADDU rd, rs, rt
void R3000::executeADDU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rs] + m_r[rt];
	SetGPR(rd, result);
}

// SUB rd, rs, rt
//
// The contents of general register rt are subtracted from the contents of general register rs to form a
// result. The result is placed into general register rd.
// 
// The only difference between this instruction and the SUBU instruction is that SUBU never traps on
// overflow.
// 
// An integer overflow exception takes place if the carries out of bits 30 and 31 differ (2’s complement
// overflow). The destination register rd is not modified when an integer overflow exception occurs.
//
// #TODO: Test this
//
void R3000::executeSUB(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	s32 s = (s32)m_r[rs];
	s32 t = (s32)m_r[rt];
	u32 result = s - t;

	// For subtraction a - b, overflow occurs when:
	// 1. a and b have different signs, AND
	// 2. the result has a different sign than a
	//
	//  (a ^ b) bit 31 is set iff the operands have different signs.
	// (a ^ result) bit 31 is set iff the result has a different sign than the operands.
	//
	// #TODO: Test this
	if (((m_r[rs] ^ m_r[rt]) & ((m_r[rs] ^ result))) & 0x80000000)
		triggerException(ExcCode::Ov);
	else
		SetGPR(rd, result);
}

// SUBU rd, rs, rt
//
void R3000::executeSUBU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rs] - m_r[rt];
	SetGPR(rd, result);
}

// AND rd, rs, rt
// 
// The contents of general register rs are combined with the contents of general register rt in
// a bit-wise logical AND operation. The result is placed into general register rd.
//
void R3000::executeAND(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rs] & m_r[rt];
	SetGPR(rd, result);
}

// OR rd, rs, rt
// 
// The contents of general register rs are combined with the contents of general register rt in
// a bit-wise logical OR operation. The result is placed into general register rd.
//
// There is no clear instruction so OR rd, $0, $0 can be used instead.
//
void R3000::executeOR(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rs] | m_r[rt];
	SetGPR(rd, result);
}

// XOR rd, rs, rt
// 
// The contents of general register rs are combined with the contents of general register rt in
// a bit-wise logical exclusive OR operation. The result is placed into general register rd.
//
void R3000::executeXOR(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = m_r[rs] ^ m_r[rt];
	SetGPR(rd, result);
}

// NOR rd, rs, rt
// 
// The contents of general register rs are combined with the contents of general register rt in
// a bit-wise logical NOR operation. The result is placed into general register rd.
//
void R3000::executeNOR(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 result = ~(m_r[rs] | m_r[rt]);
	SetGPR(rd, result);
}

// SLT rd, rs, rt
//
void R3000::executeSLT(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 val = (s32)m_r[rs] < (s32)m_r[rt] ? 1 : 0;
	SetGPR(rd, val);
}

// SLTU rd, rs, rt
//
// The contents of general register rt are subtracted from the contents of general register rs.
// Considering both quantities as unsigned integers, if the contents of general register rs are less than
// the contents of general register rt, the result is set to one; otherwise the result is set to zero.
// The result is placed into general register rd.
void R3000::executeSLTU(u32 opcode)
{
	u32 rs = (opcode >> 21) & 0x1f; // bits 25:21
	u32 rt = (opcode >> 16) & 0x1f; // bits 20:16
	u32 rd = (opcode >> 11) & 0x1f; // bits 15:11
	u32 val = m_r[rs] < m_r[rt] ? 1 : 0;
	SetGPR(rd, val);
}
