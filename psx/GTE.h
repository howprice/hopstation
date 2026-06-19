// PSX Geometry Transformation Engine
// CPU coprocessor 2 (COP2)
//
// References:
// - https://psx-spx.consoledev.net/geometrytransformationenginegte/
// - https://hitmen.c02.at/files/docs/psx/gte.txt
// - https://en.wikipedia.org/wiki/Fixed-point_arithmetic
// - https://en.wikipedia.org/wiki/Q_(number_format)
// - https://psx.amidog.se/lib/exe/detail.php?id=psx%3Adownload%3Agte&media=psx:download:psxtest_gte.png
// 
// The GTE doesn't have any memory or I/O ports mapped to the CPU memory bus, instead, it's solely accessed via coprocessor opcodes:
//
//     mov  cop0r12,rt          ;-enable/disable COP2 (GTE) via COP0 status register  (pseudo instruction for MTC0 cop0r12, rd)
//     mov  cop2r0-63,rt        ;\write parameters to GTE registers
//     mov  cop2r0-31,[rs+imm]  ;/
//     mov  cop2cmd,imm25       ;-issue GTE command
//     mov  rt,cop2r0-63        ;\read results from GTE registers
//     mov  [rs+imm],cop2r0-31  ;/
//     jt   cop2flg,dest        ;-jump never  ;\implemented (no exception), but,
//     jf   cop2flg,dest        ;-jump always ;/flag seems to be always "false"
//
// Lighting
// ========
//
// There are 3 directional lights.
// Each light has a direction and a colour.
// The light directions are stored in the 3x3 "light matrix"
// The light colours are stored in the 3x3 "light color matrix"

#pragma once

#include "core/Types.h"

// GTE is low level code so logging code should be completely removed unless required.
#define GTE_TRACE_ENABLED 0

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

class GTE
{
public:

	enum class DataReg
	{
		VY0_VX0, // cop2r0   2xS16
		VZ0,     // cop2r1   S16
		VY1_VX1, // cop2r2   2xS16
		VZ1,     // cop2r3   S16
		VY2_VX2, // cop2r4   2xS16
		VZ2,     // cop2r5   S16
		RGBC,    // cop2r6   4xU8 (CODE B G R)   Color/code
		OTZ,     // cop2r7   U16
		IR0,     // cop2r8   S16
		IR1,     // cop2r9   S16
		IR2,     // cop2r10  S16
		IR3,     // cop2r11  S16
		SXY0,    // cop2r12  2xS16
		SXY1,    // cop2r13  2xS16
		SXY2,    // cop2r14  2xS16
		SXYP,    // cop2r15  2xS16
		SZ0,     // cop2r16  U16
		SZ1,     // cop2r17  U16
		SZ2,     // cop2r18  U16
		SZ3,     // cop2r19  U16
		RGB0,    // cop2r20  4xU8 (CD0 B0 G0 R0)  Color FIFO
		RGB1,    // cop2r21  4xU8 (CD1 B1 G1 R1)  Color FIFO
		RGB2,    // cop2r22  4xU8 (CD2 B2 G2 R2)  Color FIFO
		RES1,    // cop2r23  Unused
		MAC0,    // cop2r24  S32  Used to store products e.g. IR1*IR1
		MAC1,    // cop2r25  S32  "
		MAC2,    // cop2r26	 S32  "
		MAC3,    // cop2r27	 S32  "
		IRGB,    // cop2r28  R/W
		ORGB,    // cop2r29  read-only
		LZCS,    // cop2r30  S32 R/W
		LZCR,    // cop2r31  S32 Read-only

		Max = LZCR
	};

	enum class ControlReg
	{
		RT12_RT11, // cop2r32 cnt0   Rotation matrix
		RT21_RT13, // cop2r33 cnt1   Rotation matrix
		RT23_RT22, // cop2r34 cnt2   Rotation matrix
		RT32_RT31, // cop2r35 cnt3   Rotation matrix
		RT33,      // cop2r36 cnt4   Rotation matrix
		TRX,       // cop2r37 cnt5   S32
		TRY,       // cop2r38 cnt6   S32
		TRZ,       // cop2r39 cnt7   S32
		L12_L11,   // cop2r40 cnt8   Light source matrix 2xS16
		L21_L13,   // cop2r41 cnt9   Light source matrix 2xS16
		L23_L22,   // cop2r42 cnt10  Light source matrix 2xS16
		L32_L31,   // cop2r43 cnt11  Light source matrix 2xS16
		L33,       // cop2r44 cnt12  Light source matrix S16
		RBK,       // cop2r45 cnt13  Background color Red    S32 [1,19,12]
		GBK,       // cop2r46 cnt14  Background color Green  S32 [1,19,12]
		BBK,       // cop2r47 cnt15  Background color Blue   S32 [1,19,12]
		LR2_LR1,   // cop2r48 cnt16 Light color matrix 2x S16
		LG1_LR3,   // cop2r49 cnt17 Light color matrix 2x S16
		LG3_LG2,   // cop2r50 cnt18 Light color matrix 2x S16
		LB2_LB1,   // cop2r51 cnt19 Light color matrix 2x S16
		LB3,       // cop2r52 cnt20 Light color matrix S16
		RFC,       // cop2r53 cnt21 Red Far Color [1,27,4]
		GFC,       // cop2r54 cnt22 Green Far Color [1,27,4]
		BFC,       // cop2r55 cnt23 Blue Far Color [1,27,4]
		OFX,       // cop2r56 cnt24 S32 Screen Offset X
		OFY,       // cop2r57 cnt25 S32 Screen Offset Y
		H,         // cop2r58 cnt26 U16 (buggy - see docs)
		DQA,       // cop2r59 cnt27 S16
		DQB,       // cop2r60 cnt28 S32
		ZSF3,      // cop2r61 cnt29
		ZSF4,      // cop2r62 cnt30
		FLAG,      // cop2r63 cnt31

		Max = FLAG
	};

	void Reset();

	void WriteDataReg(unsigned int index, u32 val);
	u32 ReadDataReg(unsigned int index) const;

	void WriteControlRegister(unsigned int index, u32 val);
	u32 ReadControlReg(unsigned int index) const;

	void ExecuteCommand(u32 opcode);

private:

	struct Vector16
	{
		union {

			s16 e[3];

			struct {
				s16 x;
				s16 y;
				s16 z;
			};
		};

		s16 operator [](int index) const { return e[index]; }
	};

	struct Vector64
	{
		union {

			s64 e[3];

			struct {
				s64 x;
				s64 y;
				s64 z;
			};
		};

		Vector64(s64 x_, s64 y_, s64 z_) : x(x_), y(y_), z(z_) {}
		s64 operator [](int index) const { return e[index]; }
	};

	struct Matrix33
	{
		Vector16 row[3];
		const Vector16& operator [](int index) const { return row[index]; }
	};

	void writeIRGB(u32 val);
	u32 readORGB() const;
	void writeSXYP(u32 val);
	u32 readLZCR() const;

	void executeRTPS(u32 opcode);
	void executeRTPT(u32 opcode);
	s64 rtp(s16 VX, s16 VY, s16 VZ, bool sf, bool lm);
	void rtpDepthCueing(s64 n);

	void executeNCLIP();
	void executeMVMVA(u32 opcode);

	void nc(const Vector64& v, bool sf, bool lm);
	void executeNCS(u32 opcode);
	void executeNCT(u32 opcode);

	void ncd(const Vector64& v, bool sf, bool lm);
	void executeNCDS(u32 opcode);
	void executeNCDT(u32 opcode);

	void ncc(const Vector64& v, bool sf, bool lm);
	void executeNCCS(u32 opcode);
	void executeNCCT(u32 opcode);

	void executeSQR(u32 opcode);
	void executeAVSZ3();
	void executeAVSZ4();
	void executeOP(u32 opcode); // outer product
	void executeGPF(u32 opcode);
	void executeGPL(u32 opcode);

	void executeCDP(u32 opcode);
	void executeCC(u32 opcode);

	// Depth cueing functions
	// Meaning interpolating between a colour and the far colour to give the impression of depth.
	void executeDPCS(u32 opcode);
	void executeDPCT(u32 opcode);
	void executeINTPL(u32 opcode);
	void interpolateColorAndFarColor(s64 r, s64 g, s64 b, bool sf, bool lm);

	void executeDPCL(u32 opcode);
	void dpcl(bool sf, bool lm);

	void multiplyMatrixVector(const Vector64& v, const Matrix33& mat, bool sf);
	void multiplyMatrixVectorAndBias(const Vector64& v, const Matrix33& mat, const Vector64& bias, bool sf);

	void copyMAC123toIR123(bool lm);
	void pushMAC123toColorFIFO();

	void checkMac0Overflow(s64 m0);
	s64 mac123_s64_to_s44(unsigned int index, s64 val);
	void updateErrorFlag();

	union {
		u32 m_dataReg[32]{};

		struct {
			u32 vy0_vx0; // cop2r0   2xS16
			u32 vz0;     // cop2r1   S16
			u32 vy1_vx1; // cop2r2   2xS16
			u32 vz1;     // cop2r3   S16
			u32 vy2_vx2; // cop2r4   2xS16
			u32 vz2;     // cop2r5   S16
			u32 rgbc;    // cop2r6   4xU8 (CD B G R)  Color/code
			u32 otz;     // cop2r7   U16
			u32 ir0;     // cop2r9   S16
			u32 ir1;     // cop2r10  S16
			u32 ir2;     // cop2r11  S16
			u32 ir3;     // cop2r11  S16
			u32 sxy0;    // cop2r12  2xS16
			u32 sxy1;    // cop2r13  2xS16
			u32 sxy2;    // cop2r14  2xS16
			u32 sxyp;    // cop2r15  2xS16
			u32 sz0;     // cop2r16  U16
			u32 sz1;     // cop2r17  U16
			u32 sz2;     // cop2r18  U16
			u32 sz3;     // cop2r19  U16
			u32 rgb0;    // cop2r20  4xU8 (CD0 B0 G0 R0)  Color FIFO
			u32 rgb1;    // cop2r21  4xU8 (CD1 B1 G1 R1)  Color FIFO
			u32 rgb2;    // cop2r22  4xU8 (CD2 B2 G2 R2)  Color FIFO
			u32 res1;    // cop2r23  Unused
			s32 mac0;    // cop2r24  S32  Used to store products e.g. IR1*IR1
			s32 mac1;    // cop2r25	 S32  "
			s32 mac2;    // cop2r26	 S32  "
			s32 mac3;    // cop2r27	 S32  "
			u32 irgb;    // cop2r28
			u32 orgb;    // cop2r29
			s32 lzcs;    // cop2r30
			u32 lzcr;    // cop2r31
		};
	};

	s64 m1{};
	s64 m2{};
	s64 m3{};

	// Flag Register
	// 
	//   31   Error Flag (Bit30..23, and 18..13 ORed together) (Read only)
	//   30   MAC1 Result larger than 43 bits and positive
	//   29   MAC2 Result larger than 43 bits and positive
	//   28   MAC3 Result larger than 43 bits and positive
	//   27   MAC1 Result larger than 43 bits and negative
	//   26   MAC2 Result larger than 43 bits and negative
	//   25   MAC3 Result larger than 43 bits and negative
	//   24   IR1 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
	//   23   IR2 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
	//   22   IR3 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
	//   21   Color-FIFO-R saturated to +00h..+FFh
	//   20   Color-FIFO-G saturated to +00h..+FFh
	//   19   Color-FIFO-B saturated to +00h..+FFh
	//   18   SZ3 or OTZ saturated to +0000h..+FFFFh
	//   17   Divide overflow. RTPS/RTPT division result saturated to max=1FFFFh
	//   16   MAC0 Result larger than 31 bits and positive
	//   15   MAC0 Result larger than 31 bits and negative
	//   14   SX2 saturated to -0400h..+03FFh
	//   13   SY2 saturated to -0400h..+03FFh
	//   12   IR0 saturated to +0000h..+1000h
	//   0-11 Not used (always zero) (Read only)
	// 
	// https://psx-spx.consoledev.net/geometrytransformationenginegte/#cop2r63-cnt31-flag-returns-any-calculation-errors_1
	struct Flag
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			// Bits flaged as Error result in ErrorFlag being set. n.b. IR3Saturated does not for some reason. Hardware bug?
			struct {
				u32 unusedBits11_0 : 12;      // Bits 0-11 Always zero
				u32 IR0Saturated : 1;         // Bit 12
				u32 SY2Saturated : 1;         // Bit 13  Error 
				u32 SX2Saturated : 1;         // Bit 14  Error 
				u32 MAC0NegativeOverflow : 1; // Bit 15  Error 
				u32 MAC0PositiveOverflow : 1; // Bit 16  Error 
				u32 DivideOverflow : 1;       // Bit 17  Error 
				u32 SZ3_OTZSaturated : 1;     // Bit 18  Error 
				u32 ColorFIFOBSaturated : 1;  // Bit 19
				u32 ColorFIFOGSaturated : 1;  // Bit 20
				u32 ColorFIFORSaturated : 1;  // Bit 21
				u32 IR3Saturated : 1;         // Bit 22
				u32 IR2Saturated : 1;         // Bit 23  Error 
				u32 IR1Saturated : 1;         // Bit 24  Error 
				u32 MAC3NegativeOverflow : 1; // Bit 25  Error 
				u32 MAC2NegativeOverflow : 1; // Bit 26  Error 
				u32 MAC1NegativeOverflow : 1; // Bit 27  Error 
				u32 MAC3PositiveOverflow : 1; // Bit 28  Error 
				u32 MAC2PositiveOverflow : 1; // Bit 29  Error 
				u32 MAC1PositiveOverflow : 1; // Bit 30  Error 
				u32 ErrorFlag : 1;            // Bit 31
			};
		};
	};

	union {
		u32 m_controlReg[32]{};

		struct {
			union {
				Matrix33 rt;
				struct {
					u32 rt12_rt11; // cop2r32 cnt0  Rotation matrix
					u32 rt21_rt13; // cop2r33 cnt1  Rotation matrix
					u32 rt23_rt22; // cop2r34 cnt2  Rotation matrix
					u32 rt32_rt31; // cop2r35 cnt3  Rotation matrix
					u32 rt33;      // cop2r36 cnt4  Rotation matrix
				};
			};
			s32 trx;       // cop2r37 cnt5  S32
			s32 try_;      // cop2r38 cnt6  S32
			s32 trz;       // cop2r39 cnt7  S32

			// Light source matrix.
			// Each row is the direction to one of the three directional lights.
			union {
				Matrix33 lightSourceMatrix;
				struct {
					u32 l12_l11;   // cop2r40 cnt8  Light source matrix 2xS16
					u32 l21_l13;   // cop2r41 cnt9	Light source matrix 2xS16
					u32 l23_l22;   // cop2r42 cnt10	Light source matrix 2xS16
					u32 l32_l31;   // cop2r43 cnt11	Light source matrix 2xS16
					u32 l33;       // cop2r44 cnt12	Light source matrix S16
				};
			};

			s32 rbk;       // cop2r45 cnt13 Background color Red    S32 [1,19,12]
			s32 gbk;       // cop2r46 cnt14 Background color Green  S32 [1,19,12]
			s32 bbk;       // cop2r47 cnt15 Background color Blue   S32 [1,19,12]

			// Light color matrix.
			// Each row is the RGB color of one of the three directional lights.
			union {
				Matrix33 lightColorMatrix;
				struct {
					u32 lr2_lr1;   // cop2r48 cnt16 Light color matrix 2x S16
					u32 lg1_lr3;   // cop2r49 cnt17 Light color matrix 2x S16
					u32 lg3_lg2;   // cop2r50 cnt18 Light color matrix 2x S16
					u32 lb2_lb1;   // cop2r51 cnt19 Light color matrix 2x S16
					u32 lb3;       // cop2r52 cnt20 Light color matrix S16
				};
			};

			// Far Color is used for "depth cueing" which is a fog like effect for simulating depth.
			s32 m_rfc;     // cop2r53 cnt21 Red Far Color [1,27,4]
			s32 m_gfc;     // cop2r54 cnt22 Green Far Color [1,27,4]
			s32 m_bfc;     // cop2r55 cnt23 Blue Far Color [1,27,4]

			s32 ofx;       // cop2r56 cnt24 S32 Screen Offset X
			s32 ofy;       // cop2r57 cnt25 S32 Screen Offset Y
			u32 h;         // cop2r58 cnt26 U16 (buggy - see docs)
			u32 dqa;       // cop2r59 cnt27 S16
			s32 dqb;       // cop2r60 cnt28 S32
			u32 zsf3;      // cop2r61 cnt29 S16
			u32 zsf4;      // cop2r62 cnt30 S16
			Flag flag;     // cop2r63 cnt31
		};
	};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
