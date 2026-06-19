#pragma once

/*
MIPS R3000A-compatible 32-bit RISC CPU MIPS R305

Hardware features:
- 33.8688 MHz
- MIPS I ISA
- Pipelined instruction execution 
- 32-bit address bus
- 32-bit data bus
- 4 KB instruction cache
- 1 KB data cache, but not used as a cache; configured as a scratchpad
- Little-endian for PSX (CPU can actually do big endian too, but not used on PSX)
- Branch delay slots
- Load delay slots
- Coprocessors
- No ALU flags!
- No hardware support for stack!
- Single addressing mode: base + offset

Not all of these features are implemented. Most games run without:
- Pipeline emulation; as long as branch delay and load delay slots are emulated
- Instruction cache
- Accurate timing: 2 cycles per instuction (CPI) works nicely
- Data cache - it's repurposed as addressable scratch pad in the PlayStation CPU anyway.

Note that the PlayStation CPU does not have floating point coprocessor.
*/

#include "GTE.h"

#include "core/Types.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

class R3000
{
public:

	// 5 bit code stored in bits 6:2 of the COP 0 Cause register
	enum class ExcCode
	{
		Int = 0, // Interrupt
		Mod = 1, // TLB modification" Unused on R3000 (no MMU)
		TLBL = 2, // TLB load. Unused on R3000 (no MMU)
		TLBS = 3, // TLB store. Unused on R3000 (no MMU)

		// Address error on load/I-fetch or store respectively
		// Either:
		// - an attempt to access outside kuseg when in user mode
		// - an attempt to read a word or half-word at a misaligned address.
		AdEL = 4, // load address error
		AdES = 5, // store address error

		// Bus error (instruction fetch or data load respectively)
		// n.b. R3000 does not generate bus errors on store because the write buffer would make the exception imprecise.
		IBE = 6,
		DBE = 7,

		Syscall = 8, // Generated unconditionally by a syscall instruction.

		Bp = 9, // Breakpoint - a break instruction.
		RI = 10, // Reserved instruction
		CpU = 11, // Coprocessor Unusable / unsupported coprocessor operation
		Ov = 12, // Arithmetic Overflow. n.b. "Unsigned" instructions do not cause this.
	};

	typedef u8 ReadByte(uint32_t address, void* userdata);
	typedef u16 ReadHalfWord(uint32_t address, void* userdata);
	typedef u32 ReadWord(uint32_t address, void* userdata);
	typedef void WriteByte(uint32_t address, u8 val, void* userdata);
	typedef void WriteHalfWord(uint32_t address, u16 val, void* userdata);
	typedef void WriteWord(uint32_t address, u32 val, void* userdata);

	// This is *not* the true CPU reset. It is designed to reset the state to a known state for determinism.
	void Reset();

	// Simplified model of the pipeline simply executes a whole instruction at once.
	void ExecuteInstruction();

	void SetCallbacks(
		ReadByte* pReadByte,
		ReadHalfWord* pReadHalfWord,
		ReadWord* pReadWord,
		WriteByte* pWriteByte,
		WriteHalfWord* pWriteHalfWord,
		WriteWord* pWriteWord,
		void* userdata);

	// This will be called between instructions, so PC will point to next instruction to be fetched.
	u32 GetPC() const { return m_fetchPC; }
	void SetPC(u32 pc);

	// Returns the PC of the instruction currently being executed (if accessed during ExecuteInstruction() in a callback)
	u32 GetCurrentPC() const { return m_PC; }

	u32 GetGPR(unsigned int index) const { return m_r[index]; }
	void SetGPR(unsigned int index, u32 val);

	u32 GetHI() const { return m_hi; }
	u32 GetLO() const { return m_lo; }

	u32 GetSR() const { return m_sr; }

	// There are 6 external interrupt pins for hardware interrupts
	void SetInterruptPins(unsigned int val);

	// The COP0 CAUSE register 13 has six hardware interrupt bits in the IP field (bits 15:10) which directly correspond to the external interrupt pins.
	unsigned int GetInterruptPins() const { return m_cause.IP; };

private:

	// R3000 reset vector
	// The PSX BIOS is mapped to this address in KSEG1
	static constexpr u32 kInitialPC = 0xbfc0'0000;

	// This is the program counter value used at the start of the pipeline in the instruction fetch (IF) stage.
	u32 m_fetchPC{kInitialPC};

	// Program counter for the next instruction fetch.
	// Used to emulate branch delay in lieu of full pipeline emulation.
	// Set this rather than PC for instructions which require a branch delay slot.
	u32 m_nextFetchPC{kInitialPC + 4};

	// The address of the instruction currently being executed.
	u32 m_PC{kInitialPC};

	// 32 general purpose 32-bit registers
	// 
	// R0 is not a real register and is used for zero. Reading it returns zero. Writing it is a NOP.
	//
	// R29 is used as the stackpointer
	//
	union {
		u32 m_r[32]{};

		// Add resource aliases to aid debugging. People say things like "what is the value of $a0"
		// 
		// Name       Alias    Common Usage
		// 
		// (R0)       zero     Constant (always 0) (this one isn't a real register)
		// R1         at       Assembler temporary (destroyed by some pseudo opcodes!)
		// R2-R3      v0-v1    Subroutine return values, may be changed by subroutines
		// R4-R7      a0-a3    Subroutine arguments, may be changed by subroutines
		// R8-R15     t0-t7    Temporaries, may be changed by subroutines
		// R16-R23    s0-s7    Static variables, must be saved by subs
		// R24-R25    t8-t9    Temporaries, may be changed by subroutines
		// R26-R27    k0-k1    Reserved for kernel (destroyed by some IRQ handlers!)
		// R28        gp       Global pointer (rarely used)
		// R29        sp       Stack pointer
		// R30        fp(s8)   Frame Pointer, or 9th Static variable, must be saved
		// R31        ra       Return address (used so by JAL,BLTZAL,BGEZAL opcodes)
		//
		// Source: https://problemkaputt.de/psx-spx.htm
		struct {
			u32 zero; // R0, not a real register
			u32 at;
			u32 v0, v1;
			u32 a0, a1, a2, a3;
			u32 t0, t1, t2, t3, t4, t5, t6, t7;
			u32 s0, s1, s2, s3, s4, s5, s6, s7;
			u32 t8, t9;
			u32 k0, k1;
			u32 gp;
			u32 sp;
			u32 fp; // aka s8
			u32 ra;
		};
	};

	// Integer multiply results
	u32 m_hi{};
	u32 m_lo{};

	// COP0 register 6: TAR - Target Address Register (read only)
	// When an exception occurs in the delay slot of a jump or branch (cop0r13.31=1), and the branch
	// is to be taken (or it's an unconditional jump) (cop0r13.30=1), this register is updated to
	// contain the destination address of the jump or branch.
	// https://psx-spx.consoledev.net/cpuspecifications/#cop0r6-tar-target-address-r
	u32 m_tar{};

	// COP0 register 8: BadVaddr aka BadA - Bad Virtual Address (read only)
	// Contains the address whose reference caused an exception. n.b. This is not the address in PC containing the opcode that made the reference, but the actual bad address itself e.g. an odd address.
	// BadVaddr is updated ONLY by Address errors (Excode 04h and 05h), all other exceptions (including
	// bus errors) leave BadVaddr unchanged.
	// https://psx-spx.consoledev.net/cpuspecifications/#cop0r8-badvaddr-bad-virtual-address-r
	u32 m_BadVaddr{};

	// COP0 register 12: Status Register
	// 
	// Bit(s)
	//  0     IEc Current Interrupt Enable  (0=Disable, 1=Enable) ;rfe pops IUp here
	//  1     KUc Current Kernel/User Mode  (0=Kernel, 1=User)    ;rfe pops KUp here
	//  2     IEp Previous Interrupt Enable                       ;rfe pops IUo here
	//  3     KUp Previous Kernel/User Mode                       ;rfe pops KUo here
	//  4     IEo Old Interrupt Enable                        ;left unchanged by rfe
	//  5     KUo Old Kernel/User Mode                        ;left unchanged by rfe
	//  6-7   -   Not used (zero)
	//  8-15  Im  8 bit interrupt mask fields. When set the corresponding interrupts are allowed to cause an exception.
	//            Corresponding to CAUSE register, the lower two bits 9:8 are software interrupt bits, and the upper 6 bits 15:10 are hardware interrupt bits.
	//  16    Isc Isolate Cache (0=No, 1=Isolate)
	//              When isolated, all load and store operations are targetted
	//              to the Data cache, and never the main memory.
	//              (Used by PSX Kernel, in combination with Port FFFE0130h)
	//  17    Swc Swapped cache mode (0=Normal, 1=Swapped)
	//              Instruction cache will act as Data cache and vice versa.
	//              Use only with Isc to access & invalidate Instr. cache entries.
	//              (Not used by PSX Kernel)
	//  18    PZ  When set cache parity bits are written as 0.
	//  19    CM  Shows the result of the last load operation with the D-cache
	//            isolated. It gets set if the cache really contained data
	//            for the addressed memory location.
	//  20    PE  Cache parity error (Does not cause exception)
	//  21    TS  TLB shutdown. Gets set if a programm address simultaneously
	//            matches 2 TLB entries.
	//            (initial value on reset allows to detect extended CPU version?)
	//  22    BEV Boot exception vectors in RAM/ROM (0=RAM/KSEG0, 1=ROM/KSEG1)
	//  23-24 -   Not used (zero)
	//  25    RE  Reverse endianness   (0=Normal endianness, 1=Reverse endianness)
	//              Reverses the byte order in which data is stored in
	//              memory. (lo-hi -> hi-lo)
	//              (Affects only user mode, not kernel mode) (?)
	//              (The bit doesn't exist in PSX ?)
	//  26-27 -   Not used (zero)
	//  28    CU0 COP0 Enable (0=Enable only in Kernel Mode, 1=Kernel and User Mode)
	//  29    CU1 COP1 Enable (0=Disable, 1=Enable) (none in PSX)
	//  30    CU2 COP2 Enable (0=Disable, 1=Enable) (GTE in PSX)
	//  31    CU3 COP3 Enable (0=Disable, 1=Enable) (none in PSX)
	// 
	// https://psx-spx.consoledev.net/cpuspecifications/#cop0r12-sr-system-status-register-rw
	// 
	// #TODO: Implement as union of struct of bitfields + value. See R3000.pdf 3-4
	u32 m_sr{};

	// State used to set correct value for EPC on exception.
	bool m_branch{}; // True if current instruction is a branch
	bool m_branchDelaySlot{}; // True if the current instruction is in a branch delay slot (which follows a branch instruction)

	// COP0 register 13: CAUSE
	// The cause of an exception.
	struct CauseRegister
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 unusedBits1_0 : 2; // Bits [0:2] Always zero
				ExcCode excCode : 5; // Bits [6:2]
				u32 unusedBit7 : 1; // Bit 7. Always zero
				u32 SW : 2; // Bits 9:8 Software Interrupts. Write to these bits to manually cause an exception. Clear them before returning from the exception handler.
				u32 IP : 6; // Bits 15:10 Interrupt Pending bits
				u32 unusedBits27_16 : 12;
				u32 CE : 2; // 29:28 Coprocessor Error

				u32 BT : 1; // Bit 30. When BD is set, BT determines whether the branch is taken. The Target Address Register holds the return address.

				// Branch Delay.
				// If set, this bit indicates that the EPC does not point to the actual "exception" instruction, but
				// rather to the branch instruction which immediately precedes it.
				u32 BD : 1; // Bit 31
			};
		};
	};
	CauseRegister m_cause{};

	// COP0 register 14: EPC
	// Exception PC
	// Address of the return point for this exception. The instruction causing (or suffering) the exception is at
	// EPC, unless BD is set in Cause, in which case EPC points to the previous (branch) instruction.
	u32 m_EPC{};

	struct DelayedLoad
	{
		unsigned int registerIndex = 0;
		u32 val = 0;
	};

	DelayedLoad m_delayedLoad{};
	DelayedLoad m_delayedLoadNext{};

	ReadByte* m_pReadByte{};
	ReadHalfWord* m_pReadHalfWord{};
	ReadWord* m_pReadWord{};
	WriteByte* m_pWriteByte{};
	WriteHalfWord* m_pWriteHalfWord{};
	WriteWord* m_pWriteWord{};
	void* m_userdata{};

	// PSX SOC Coprocessor 2 (COP2) Geometry Transformation Engine (GTE)
	GTE m_gte;

	// Implements load delay
	void setGprDelayed(unsigned int index, u32 val);
	void processDelayedLoads();

	void triggerException(ExcCode excCode);

	// Primary opcode field (Bit 26..31)
	//   00h=SPECIAL 08h=ADDI  10h=COP0 18h=N/A   20h=LB   28h=SB   30h=LWC0 38h=SWC0
	//   01h=BcondZ  09h=ADDIU 11h=COP1 19h=N/A   21h=LH   29h=SH   31h=LWC1 39h=SWC1
	//   02h=J       0Ah=SLTI  12h=COP2 1Ah=N/A   22h=LWL  2Ah=SWL  32h=LWC2 3Ah=SWC2
	//   03h=JAL     0Bh=SLTIU 13h=COP3 1Bh=N/A   23h=LW   2Bh=SW   33h=LWC3 3Bh=SWC3
	//   04h=BEQ     0Ch=ANDI  14h=N/A  1Ch=N/A   24h=LBU  2Ch=N/A  34h=N/A  3Ch=N/A
	//   05h=BNE     0Dh=ORI   15h=N/A  1Dh=N/A   25h=LHU  2Dh=N/A  35h=N/A  3Dh=N/A
	//   06h=BLEZ    0Eh=XORI  16h=N/A  1Eh=N/A   26h=LWR  2Eh=SWR  36h=N/A  3Eh=N/A
	//   07h=BGTZ    0Fh=LUI   17h=N/A  1Fh=N/A   27h=N/A  2Fh=N/A  37h=N/A  3Fh=N/A

	void executeBcondZ(u32 opcode);// 01
	void executeJ(u32 opcode);     // 02
	void executeJAL(u32 opcode);   // 03
	void executeBEQ(u32 opcode);   // 04
	void executeBNE(u32 opcode);   // 05
	void executeBLEZ(u32 opcode);  // 06
	void executeBGTZ(u32 opcode);  // 07
	void executeADDI(u32 opcode);  // 08
	void executeADDIU(u32 opcode); // 09
	void executeSLTI(u32 opcode);  // 0A
	void executeSLTIU(u32 opcode); // 0B
	void executeANDI(u32 opcode);  // 0C
	void executeORI(u32 opcode);   // 0D
	void executeXORI(u32 opcode);  // 0E
	void executeLUI(u32 opcode);   // 0F
	void executeMFC0(u32 opcode);  // 10 (COP0)
	void executeMTC0(u32 opcode);  // 10 (COP0)
	void executeRFE(u32 opcode);   // 10 (COP0)
	void executeMFC2(u32 opcode);  // 12 (COP2)
	void executeMTC2(u32 opcode);  // 12 (COP2)
	void executeCFC2(u32 opcode);  // 12 (COP2)
	void executeCTC2(u32 opcode);  // 12 (COP2)
	void executeCOP2(u32 opcode);  // 12 (COP2)
	void executeLB(u32 opcode);    // 20
	void executeLH(u32 opcode);    // 21
	void executeLWL(u32 opcode);   // 22
	void executeLW(u32 opcode);    // 23
	void executeLBU(u32 opcode);   // 24
	void executeLHU(u32 opcode);   // 25
	void executeLWR(u32 opcode);   // 26
	void executeSB(u32 opcode);    // 28
	void executeSH(u32 opcode);    // 29
	void executeSWL(u32 opcode);   // 2A
	void executeSW(u32 opcode);    // 2B
	void executeSWR(u32 opcode);   // 2E
	void executeLWC0(u32 opcode);  // 30
	void executeLWC1(u32 opcode);  // 31
	void executeLWC2(u32 opcode);  // 32
	void executeLWC3(u32 opcode);  // 33
	void executeSWC0(u32 opcode);  // 38
	void executeSWC1(u32 opcode);  // 39
	void executeSWC2(u32 opcode);  // 3A
	void executeSWC3(u32 opcode);  // 3B

	// Secondary opcode field (Bit 0..5) (when Primary opcode = 00h) aka "SPECIAL"
	//  00h=SLL   08h=JR      10h=MFHI 18h=MULT  20h=ADD  28h=N/A  30h=N/A  38h=N/A
	//  01h=N/A   09h=JALR    11h=MTHI 19h=MULTU 21h=ADDU 29h=N/A  31h=N/A  39h=N/A
	//  02h=SRL   0Ah=N/A     12h=MFLO 1Ah=DIV   22h=SUB  2Ah=SLT  32h=N/A  3Ah=N/A
	//  03h=SRA   0Bh=N/A     13h=MTLO 1Bh=DIVU  23h=SUBU 2Bh=SLTU 33h=N/A  3Bh=N/A
	//  04h=SLLV  0Ch=SYSCALL 14h=N/A  1Ch=N/A   24h=AND  2Ch=N/A  34h=N/A  3Ch=N/A
	//  05h=N/A   0Dh=BREAK   15h=N/A  1Dh=N/A   25h=OR   2Dh=N/A  35h=N/A  3Dh=N/A
	//  06h=SRLV  0Eh=N/A     16h=N/A  1Eh=N/A   26h=XOR  2Eh=N/A  36h=N/A  3Eh=N/A
	//  07h=SRAV  0Fh=N/A     17h=N/A  1Fh=N/A   27h=NOR  2Fh=N/A  37h=N/A  3Fh=N/A

	void executeSLL(u32 opcode);     // 00
	void executeSRL(u32 opcode);     // 02
	void executeSRA(u32 opcode);     // 03
	void executeSLLV(u32 opcode);    // 04
	void executeSRLV(u32 opcode);    // 06
	void executeSRAV(u32 opcode);    // 07
	void executeJR(u32 opcode);      // 08
	void executeJALR(u32 opcode);    // 09
	void executeSYSCALL(u32 opcode); // 0C
	void executeBREAK(u32 opcode);   // 0D
	void executeMFHI(u32 opcode);    // 10
	void executeMTHI(u32 opcode);    // 11
	void executeMFLO(u32 opcode);    // 12
	void executeMTLO(u32 opcode);    // 13
	void executeMULT(u32 opcode);    // 18
	void executeMULTU(u32 opcode);   // 19
	void executeDIV(u32 opcode);     // 1A
	void executeDIVU(u32 opcode);    // 1B
	void executeADD(u32 opcode);     // 20
	void executeADDU(u32 opcode);    // 21
	void executeSUB(u32 opcode);     // 22
	void executeSUBU(u32 opcode);    // 23
	void executeAND(u32 opcode);     // 24
	void executeOR(u32 opcode);      // 25
	void executeXOR(u32 opcode);     // 26
	void executeNOR(u32 opcode);     // 27
	void executeSLT(u32 opcode);     // 2A
	void executeSLTU(u32 opcode);    // 2B
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
