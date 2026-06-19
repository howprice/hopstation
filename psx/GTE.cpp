#include "GTE.h"

#include "core/Log.h"
#include "core/ArrayHelpers.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED #TODO: Remove this
#include "core/MathsHelpers.h"

#if defined(_MSC_VER)
#include <intrin.h> // _tzcnt_u32, _lzcnt_u32
#endif

#if GTE_TRACE_ENABLED
#define GTE_TRACE(...) LogLevel(LOG_LEVEL_TRACE, "[GTE] " __VA_ARGS__)
#else
#define GTE_TRACE(...) do {} while (0)
#endif

// n.b. The error flag bit 31 is Bit30..23, and 18..13 ORed together. Read only.
// This set of bits seems a bit strange: it doesn't capture bit 22 IR3 saturated.
// Maybe a hardware bug or something I don't understand.
static const u32 kErrorFlagMask = 0x7f87e000;

/*
  GTE Data Register Summary (cop2r0-31)

  cop2r0-1   3xS16 VXY0,VZ0              Vector 0 (X,Y,Z)
  cop2r2-3   3xS16 VXY1,VZ1              Vector 1 (X,Y,Z)
  cop2r4-5   3xS16 VXY2,VZ2              Vector 2 (X,Y,Z)
  cop2r6     4xU8  RGBC                  Color/code value
  cop2r7     1xU16 OTZ                   Average Z value (for Ordering Table)
  cop2r8     1xS16 IR0                   16bit Accumulator (Interpolate)
  cop2r9-11  3xS16 IR1,IR2,IR3           16bit Accumulator (Vector)
  cop2r12-15 6xS16 SXY0,SXY1,SXY2,SXYP   Screen XY-coordinate FIFO  (3 stages)
  cop2r16-19 4xU16 SZ0,SZ1,SZ2,SZ3       Screen Z-coordinate FIFO   (4 stages)
  cop2r20-22 12xU8 RGB0,RGB1,RGB2        Color CRGB-code/color FIFO (3 stages)
  cop2r23    4xU8  (RES1)                Prohibited
  cop2r24    1xS32 MAC0                  32bit Maths Accumulators (Value)
  cop2r25-27 3xS32 MAC1,MAC2,MAC3        32bit Maths Accumulators (Vector)
  cop2r28-29 1xU15 IRGB,ORGB             Convert RGB Color (48bit vs 15bit)
  cop2r30-31 2xS32 LZCS,LZCR             Count Leading-Zeroes/Ones (sign bits)

  https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-data-register-summary-cop2r0-31
*/
[[maybe_unused]]
static const char* kGTEDataRegisterNames[] =
{
	"VY0_VX0", // cop2r0  #TODO: Is this correct order?
	"VZ0",     // cop2r1
	"VY1_VX1", // cop2r2
	"VZ1",     // cop2r3
	"VY2_VX2", // cop2r4
	"VZ2",     // cop2r5
	"RGBC",    // cop2r6
	"OTZ",     // cop2r7
	"IR0",     // cop2r8
	"IR1",     // cop2r9
	"IR2",     // cop2r10
	"IR3",     // cop2r11
	"SXY0",    // cop2r12
	"SXY1",    // cop2r13
	"SXY2",    // cop2r14
	"SXYP",    // cop2r15
	"SZ0",     // cop2r16
	"SZ1",     // cop2r17
	"SZ2",     // cop2r18
	"SZ3",     // cop2r19
	"RGB0",    // cop2r20
	"RGB1",    // cop2r21
	"RGB2",    // cop2r22
	"(RES1)",  // cop2r23
	"MAC0",    // cop2r24
	"MAC1",    // cop2r25
	"MAC2",    // cop2r26
	"MAC3",    // cop2r27
	"IRGB",    // cop2r28
	"ORGB",    // cop2r29
	"LZCS",    // cop2r30
	"LZCR"     // cop2r31
};
static_assert(COUNTOF_ARRAY(kGTEDataRegisterNames) == 32);

/*
  GTE Control Register Summary (cop2r32-63)

  cop2r32-36 9xS16 RT11RT12,..,RT33 Rotation matrix     (3x3)        ;cnt0-4
  cop2r37-39 3x 32 TRX,TRY,TRZ      Translation vector  (X,Y,Z)      ;cnt5-7
  cop2r40-44 9xS16 L11L12,..,L33    Light source matrix (3x3)        ;cnt8-12
  cop2r45-47 3x 32 RBK,GBK,BBK      Background color    (R,G,B)      ;cnt13-15
  cop2r48-52 9xS16 LR1LR2,..,LB3    Light color matrix source (3x3)  ;cnt16-20
  cop2r53-55 3x 32 RFC,GFC,BFC      Far color           (R,G,B)      ;cnt21-23
  cop2r56-57 2x 32 OFX,OFY          Screen offset       (X,Y)        ;cnt24-25
  cop2r58 BuggyU16 H                Projection plane distance.       ;cnt26
  cop2r59      S16 DQA              Depth queing parameter A (coeff) ;cnt27
  cop2r60       32 DQB              Depth queing parameter B (offset);cnt28
  cop2r61-62 2xS16 ZSF3,ZSF4        Average Z scale factors          ;cnt29-30
  cop2r63      U20 FLAG             Returns any calculation errors   ;cnt31

  https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-control-register-summary-cop2r32-63
*/
[[maybe_unused]]
static const char* kGTEControlRegisterNames[] =
{
	"RT12_RT11", // cop2r32 cnt0
	"RT21_RT13", // cop2r33 cnt1
	"RT23_RT22", // cop2r34 cnt2
	"RT32_RT31", // cop2r35 cnt3
	"RT33",      // cop2r36 cnt4
	"TRX",       // cop2r37 cnt5
	"TRY",       // cop2r38 cnt6
	"TRZ",       // cop2r39 cnt7
	"L12_L11",   // cop2r40 cnt8
	"L21_L13",   // cop2r41 cnt9
	"L23_L22",   // cop2r42 cnt10
	"L32_L31",   // cop2r43 cnt11
	"L33",       // cop2r44 cnt12
	"RBK",       // cop2r45 cnt13
	"GBK",       // cop2r46 cnt14
	"BBK",       // cop2r47 cnt15
	"LR2_LR1",   // cop2r48 cnt16
	"LG1_LR3",   // cop2r49 cnt17
	"LG3_LG2",   // cop2r50 cnt18
	"LB2_LB1",   // cop2r51 cnt19
	"LB3",      // cop2r52 cnt20
	"RFC",       // cop2r53 cnt21
	"GFC",       // cop2r54 cnt22
	"BFC",       // cop2r55 cnt23
	"OFX",       // cop2r56 cnt24
	"OFY",       // cop2r57 cnt25
	"H",         // cop2r58 cnt26
	"DQA",       // cop2r59 cnt27
	"DQB",       // cop2r60 cnt28
	"ZSF3",      // cop2r61 cnt29
	"ZSF4",      // cop2r62 cnt30
	"FLAG"       // cop2r63 cnt31
};
static_assert(COUNTOF_ARRAY(kGTEControlRegisterNames) == 32);

/*
  GTE Command Summary (sorted by Real Opcode bits) (bit0-5)

  Opc  Name   Clk Expl.
  00h  -          N/A (modifies similar registers than RTPS...)
  01h  RTPS   15  Perspective Transformation single
  0xh  -          N/A
  06h  NCLIP  8   Normal clipping
  0xh  -          N/A
  0Ch  OP(sf) 6   Cross product of 2 vectors
  0xh  -          N/A
  10h  DPCS   8   Depth Cueing single
  11h  INTPL  8   Interpolation of a vector and far color vector
  12h  MVMVA  8   Multiply vector by matrix and add vector (see below)
  13h  NCDS   19  Normal color depth cue single vector
  14h  CDP    13  Color Depth Que
  15h  -          N/A
  16h  NCDT   44  Normal color depth cue triple vectors
  1xh  -          N/A
  1Bh  NCCS   17  Normal Color Color single vector
  1Ch  CC     11  Color Color
  1Dh  -          N/A
  1Eh  NCS    14  Normal color single
  1Fh  -          N/A
  20h  NCT    30  Normal color triple
  2xh  -          N/A
  28h  SQR(sf)5   Square of vector IR
  29h  DPCL   8   Depth Cue Color light
  2Ah  DPCT   17  Depth Cueing triple (should be fake=08h, but isn't)
  2xh  -          N/A
  2Dh  AVSZ3  5   Average of three Z values
  2Eh  AVSZ4  6   Average of four Z values
  2Fh  -          N/A
  30h  RTPT   23  Perspective Transformation triple
  3xh  -          N/A
  3Dh  GPF(sf)5   General purpose interpolation
  3Eh  GPL(sf)5   General purpose interpolation with base
  3Fh  NCCT   39  Normal Color Color triple vector

  https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-command-summary-sorted-by-real-opcode-bits-bit0-5
*/
[[maybe_unused]]
static const char* kGTERealOpcodeNames[] =
{
	"-",      // 00h
	"RTPS",   // 01h
	"-",      // 02h
	"-",      // 03h
	"-",      // 04h
	"-",      // 05h
	"NCLIP",  // 06h
	"-",      // 07h
	"-",      // 08h
	"-",      // 09h
	"-",      // 0Ah
	"-",      // 0Bh
	"OP",     // 0Ch
	"-",      // 0Dh
	"-",      // 0Eh
	"-",      // 0Fh
	"DPCS",   // 10h
	"INTPL",  // 11h
	"MVMVA",  // 12h
	"NCDS",   // 13h
	"CDP",    // 14h
	"-",      // 15h
	"NCDT",   // 16h
	"-",      // 17h
	"-",      // 18h
	"-",      // 19h
	"-",      // 1Ah
	"NCCS",   // 1Bh
	"CC",     // 1Ch
	"-",      // 1Dh
	"NCS",    // 1Eh
	"-",      // 1Fh
	"NCT",    // 20h
	"-",      // 21h
	"-",      // 22h
	"-",      // 23h
	"-",      // 24h
	"-",      // 25h
	"-",      // 26h
	"-",      // 27h
	"SQR",    // 28h
	"DPCL",   // 29h
	"DPCT",   // 2Ah
	"-",      // 2Bh
	"-",      // 2Ch
	"AVSZ3",  // 2Dh
	"AVSZ4",  // 2Eh
	"-",      // 2Fh
	"RTPT",   // 30h
	"-",      // 31h
	"-",      // 32h
	"-",      // 33h
	"-",      // 34h
	"-",      // 35h
	"-",      // 36h
	"-",      // 37h
	"-",      // 38h
	"-",      // 39h
	"-",      // 3Ah
	"-",      // 3Bh
	"-",      // 3Ch
	"GPF",    // 3Dh
	"GPL",    // 3Eh
	"NCCT"    // 3Fh
};
static_assert(COUNTOF_ARRAY(kGTERealOpcodeNames) == 64);

/*
  GTE Command Summary (sorted by Fake Opcode bits) (bit20-24)

  n.b. Not used by the hardware

  Fake Name   Clk Expl.
  00h  -          N/A
  01h  RTPS   15  Perspective Transformation single
  02h  RTPT   23  Perspective Transformation triple
  03h  -          N/A
  04h  MVMVA  8   Multiply vector by matrix and add vector (see below)
  05h  -          N/A
  06h  DPCL   8   Depth Cue Color light
  07h  DPCS   8   Depth Cueing single
  08h  DPCT   17  Depth Cueing triple (should be fake=08h, but isn't)
  09h  INTPL  8   Interpolation of a vector and far color vector
  0Ah  SQR(sf)5   Square of vector IR
  0Bh  -          N/A
  0Ch  NCS    14  Normal color single
  0Dh  NCT    30  Normal color triple
  0Eh  NCDS   19  Normal color depth cue single vector
  0Fh  NCDT   44  Normal color depth cue triple vectors
  10h  NCCS   17  Normal Color Color single vector
  11h  NCCT   39  Normal Color Color triple vector
  12h  CDP    13  Color Depth Que
  13h  CC     11  Color Color
  14h  NCLIP  8   Normal clipping
  15h  AVSZ3  5   Average of three Z values
  16h  AVSZ4  6   Average of four Z values
  17h  OP(sf) 6   Cross product of 2 vectors aka Outer Product
  18h  -          N/A
  19h  GPF(sf)5   General purpose interpolation
  1Ah  GPL(sf)5   General purpose interpolation with base
  1Bh  -          N/A
  1Ch  -          N/A
  1Dh  -          N/A
  1Eh  -          N/A
  1Fh  -          N/A

  https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-command-summary-sorted-by-fake-opcode-bits-bit20-24
*/
[[maybe_unused]]
static const char* kGTEFakeOpcodeNames[] =
{
	"-",
	"RTPS",
	"RTPT",
	"-",
	"MVMVA",
	"-",
	"DPCL",
	"DPCS",
	"DPCT",
	"INTPL",
	"SQR",
	"-",
	"NCS",
	"NCT",
	"NCDS",
	"NCDT",
	"NCCS",
	"NCCT",
	"CDP",
	"CC",
	"NCLIP",
	"AVSZ3",
	"AVSZ4",
	"OP",
	"-",
	"GPF",
	"GPL",
	"-",
	"-",
	"-",
	"-",
	"-"
};
static_assert(COUNTOF_ARRAY(kGTEFakeOpcodeNames) == 32);

//-------------------------------------------------------------------------------------------
//
// Unsigned Newton-Raphson (UNR) algorithm table used for RTPS/RTPT division.
//
// Contains reciprocals of values in the range [0.5, 1].
// 
// This table just stores the 8-bit fractional portion of the 1.8 format fixed point number because
// the integer part is implicitly 1. [1/0.5, 1/1] = [2,1]. To produce the final result, add 0x100 (=1).
//
static const u8 kUNRTable[] =
{
	0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3, // [0x00]
	0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, // [0x10]
	0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0, // [0x20]
	0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A, // [0x30]
	0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86, // [0x40]
	0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74, // [0x50]
	0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, // [0x60]
	0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, // [0x70]
	0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48, // [0x80]
	0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B, // [0x90]
	0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, // [0xa0]
	0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, // [0xb0]
	0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A, // [0xc0]
	0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, // [0xd0]
	0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08, // [0xe0]
	0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, // [0xf0]
	0x00 // one extra table entry for "(d-7FC0h)/80h" = 100h                                           [0x100]
};
static_assert(COUNTOF_ARRAY(kUNRTable) == 256 + 1);

// Fixed point division of two unsigned 16-bit fractional values in range [0,1) = [0x0000,0xffff]
// using the Unsigned Newton-Raphson (UNR) algorithm.
//
// Quotient = dividend / divisor
// 
// Returns 17-bit quotient in the range [0,2) in 1.16 fixed point format.
//
// https://en.wikipedia.org/wiki/Division_algorithm#Newton–Raphson_division
//
// GTE algorithm from "Division Inaccuracy for RTPS/RTPT commands" https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-division-inaccuracy-for-rtpsrtpt-commands
// This exact code is explained at https://stackoverflow.com/questions/41785903/how-does-this-division-approximation-algorithm-work
/*
	z = count_leading_zeroes(SZ3)                ;z=0..0Fh (for 16bit SZ3)
	n = (H SHL z)                                ;n=0..7FFF8000h
	d = (SZ3 SHL z)                              ;d=8000h..FFFFh
	u = unr_table[(d-7FC0h) SHR 7] + 101h        ;u=200h..101h
	d = ((2000080h - (d * u)) SHR 8)             ;d=10000h..0FF01h  #TODO: Is the final figure a typo?
	d = ((0000080h + (d * u)) SHR 8)             ;d=20000h..10000h
	n = min(1FFFFh, (((n*d) + 8000h) SHR 16))    ;n=0..1FFFFh
*/
static u32 UnsignedNewtonRaphsonDivide(u16 dividend, u16 divisor)
{
	// Normalise the divisor into the range [0.5,1) for table lookup by applying a scale factor or 2^z.
	// This shifts the highest set bit into the MSb.
	u32 d; // scaled divisor
#if 0 // This function should never be called with zero divisor.
	// Support zero divisor
	if (divisor > 0)
	{
		unsigned int z = COUNT_LEADING_ZEROS((u32)divisor) - 16; // [0,F]
		HP_DEBUG_ASSERT(z <= 0xf);
		d = divisor << z; // [8000,FFFF]
		HP_DEBUG_ASSERT(d >= 0x8000 && d <= 0xFFFF);
	}
	else
	{
		d = 0x8000; // 0.5
	}
#else
	HP_DEBUG_ASSERT(divisor != 0, "This function should never be called with zero divisor");
	// Normalise the divisor into the range [0.5,1) for table lookup by applying a scale factor or 2^z.
	// This shifts the highest set bit into the MSb.
	// n.b. Result of COUNT_LEADING_ZEROS is not defined for value zero.
	unsigned int z = COUNT_LEADING_ZEROS((u32)divisor) - 16; // [0,F]
	HP_DEBUG_ASSERT(z <= 0xf);
	d = divisor << z; // [8000,FFFF]
	HP_DEBUG_ASSERT(d >= 0x8000 && d <= 0xFFFF);
#endif

	// The dividend needs to be scaled by the same factor.
	u32 scaledDividend = (u32)dividend << z;

	// Initial 9-bit approximation to the reciprocal of the divisor via a table lookup
	unsigned int tableIndex = (d - 0x7fc0) >> 7;
#if HP_DEBUG_ASSERTS_ENABLED
	unsigned int tableIndex2 = ((d & 0x7fff) + 0x40) >> 7; // #TODO: Why is this form used in jsmooch and Duckstation
	HP_DEBUG_ASSERT(tableIndex == tableIndex2);
#endif
	HP_DEBUG_ASSERT(tableIndex < COUNTOF_ARRAY(kUNRTable));
	u32 u = kUNRTable[tableIndex] + 0x101; // Not clear why the GTE adds 0x101 (1+1/256) instead of 0x100 (1.0) here. Bias to ensure overestimate of result?

	// Refine result with a single Newton-Raphson iteration for the reciprocal.
	// x[i+1] = x[i] + (2 - d * x[i])
	// 0x20000'00 is 2.0 in Q.8 format shifted up by another 8. The '80 is a rounding factor.
	d = (0x2000080 - (d * u)) >> 8; // [10000h,0FF01h]
//	HP_DEBUG_ASSERT(d >= 0x0FF01 && d <= 0x10000); // This assert seems invalid
	d = (0x0000080 + (d * u)) >> 8; // [20000h,10000h]
	HP_DEBUG_ASSERT(d >= 0x10000 && d <= 0x20000);

	// Compute the quotient by multiplying the dividend by the reciprocal of the divisor.
	// n.b. Intermediate value is 33 bits so u64 required.
	// 0x8000 is rounding
	u32 quotient = (u32)((((u64)scaledDividend * (u64)d) + 0x8000) >> 16);
	if (quotient > 0x1ffff)
		quotient = 0x1ffff;

	return quotient;
}

//-------------------------------------------------------------------------------------------

void GTE::Reset()
{
	for (unsigned int i = 0; i < 32; i++)
	{
		m_dataReg[i] = 0;
	}

	for (unsigned int i = 0; i < 32; i++)
	{
		m_controlReg[i] = 0;
	}

	m1 = 0;
	m2 = 0;
	m3 = 0;
}

void GTE::WriteDataReg(unsigned int index, u32 val)
{
	HP_DEBUG_ASSERT(index < 32);

	GTE_TRACE("Write data reg %u cop2r%u %s value 0x%08x\n", index, index, kGTEDataRegisterNames[index], val);

	DataReg reg = (DataReg)index;
	switch (reg)
	{
		case DataReg::IRGB:
			writeIRGB(val);
			break;
		case DataReg::SXY2:
		{
			sxy2 = val;

			// Also write to mirror register SXYP
			sxyp = val;
			break;
		}
		case DataReg::SXYP:
			writeSXYP(val);
			break;
		case DataReg::SZ0:
		case DataReg::SZ1:
		case DataReg::SZ2:
		case DataReg::SZ3:
			m_dataReg[index] = (u16)val; // no point storing unused upper 16 bits of unsigned value
			break;
		case DataReg::ORGB: // read-only
			break;
		case DataReg::LZCR: // read-only
			break;
		default:
			m_dataReg[index] = val;
	}
}

u32 GTE::ReadDataReg(unsigned int index) const
{
	HP_DEBUG_ASSERT(index < 32);

	DataReg reg = (DataReg)index;
	u32 val;
	switch (reg)
	{
		// Reading these 16-bit signed registers returns the 16 bit value sign-extended to 32 bits.
		// https://psx-spx.consoledev.net/geometrytransformationenginegte/#16bit-vectors-rw
		case DataReg::VZ0:
		case DataReg::VZ1:
		case DataReg::VZ2:
		case DataReg::IR0:
		case DataReg::IR1:
		case DataReg::IR2:
		case DataReg::IR3:
			val = (s32)(s16)m_dataReg[index]; // sign-extend lower 16-bits
			break;
		case DataReg::OTZ:
			val = (u16)otz;
			break;
		case DataReg::IRGB:
		case DataReg::ORGB: // ORGB is just a read only mirror of IRGB
			val = readORGB();
			break;
		case DataReg::LZCR:
			val = readLZCR();
			break;
		default:
			val = m_dataReg[index];
			break;
	}

	GTE_TRACE("Read data reg %u cop2r%u %s value 0x%08x\n", index, index, kGTEDataRegisterNames[index], val);

	return val;
}

void GTE::WriteControlRegister(unsigned int index, u32 val)
{
	HP_DEBUG_ASSERT(index < 32);

	// psx-psx specs list the control registers as starting at 32, but the opcodes seem to index them from zero.
	// https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-control-register-summary-cop2r32-63
	GTE_TRACE("Write control reg %u cop2r%u %s value 0x%08x\n", index, 32+index, kGTEControlRegisterNames[index], val);

	ControlReg reg = (ControlReg)index;
	switch (reg)
	{
		case ControlReg::FLAG:
		{
			flag.val = val;
			flag.unusedBits11_0 = 0; // always zero when read, so zero on write
			updateErrorFlag();
			break;
		}
		default:
			m_controlReg[index] = val;
	}
}

u32 GTE::ReadControlReg(unsigned int index) const
{
	HP_DEBUG_ASSERT(index < 32);
	ControlReg reg = (ControlReg)index;
	u32 val;
	switch (reg)
	{
		// Reading these 16-bit signed registers returns the 16 bit value sign-extended to 32 bits.
		// https://psx-spx.consoledev.net/geometrytransformationenginegte/#matrix-registers
		case ControlReg::RT33:
		case ControlReg::L33:
		case ControlReg::LB3:
			val = (s32)(s16)m_controlReg[index]; // sign-extend lower 16-bits
			break;
		case ControlReg::H:
			// BUG: When reading the H register, the hardware does accidently sign-expand the U16 value
			// i.e. values +8000h..+FFFFh are returned as FFFF8000h..FFFFFFFFh
			// https://psx-spx.consoledev.net/geometrytransformationenginegte/#screen-offset-and-distance-input-rw
			val = (s32)(s16)h; // sign-extend lower 16-bits
			break;
		case ControlReg::DQA:
			val = (s32)(s16)dqa; // sign-extend lower 16-bits
			break;
		case ControlReg::ZSF3:
			val = (s32)(s16)zsf3; // sign-extend lower 16-bits
			break;
		case ControlReg::ZSF4:
			val = (s32)(s16)zsf4; // sign-extend lower 16-bits
			break;
		default:
			val = m_controlReg[index];
	}

	GTE_TRACE("Read control reg %u cop2r%u %s value 0x%08x\n", index, 32 + index, kGTEControlRegisterNames[index], val);

	return val;
}

/*

  GTE Command Encoding (COP2 imm25 opcodes)

  31-25  Must be 0100101b for "COP2 imm25" instructions  [Implementation note: Stripped by CPU.]
  24-20  Fake GTE Command Number (00h..1Fh) (ignored by hardware)
  19     sf - Shift Fraction in IR registers (0=No fraction, 1=12bit fraction)
  17-18  MVMVA Multiply Matrix    (0=Rotation. 1=Light, 2=Color, 3=Reserved)
  15-16  MVMVA Multiply Vector    (0=V0, 1=V1, 2=V2, 3=IR/long)
  13-14  MVMVA Translation Vector (0=TR, 1=BK, 2=FC/Bugged, 3=None)
  11-12  Always zero                        (ignored by hardware)
  10     lm - Saturate IR1,IR2,IR3 result (0=To -8000h..+7FFFh, 1=To 0..+7FFFh)
  6-9    Always zero                        (ignored by hardware)
  0-5    Real GTE Command Number (00h..3Fh) (used by hardware)

  https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-command-encoding-cop2-imm25-opcodes
*/
void GTE::ExecuteCommand(u32 opcode)
{
	u32 realOpcode = opcode & 0x3f;
#if GTE_TRACE_ENABLED
	u32 fakeOpcode = (opcode >> 20) & 0x1f;
	GTE_TRACE("Command %02X Real Opcode: %02x \"%s\" (Fake Opcode: %02x \"%s\") NOT IMPLEMENTED\n", opcode, realOpcode, kGTERealOpcodeNames[realOpcode], fakeOpcode, kGTEFakeOpcodeNames[fakeOpcode]);
#endif

	flag.val = 0; // all bits are automatically reset at the begin of a new GTE command.

	// Current implementation execute GTE command immediately and returns.
	// #TODO: GTE timing and work in parallel with rest of system.
	switch (realOpcode)
	{
		case 0x01:
			executeRTPS(opcode);
			break;
		case 0x06:
			executeNCLIP();
			break;
		case 0x0C:
			executeOP(opcode);
			break;
		case 0x10:
			executeDPCS(opcode);
			break;
		case 0x11:
			executeINTPL(opcode);
			break;
		case 0x12:
			executeMVMVA(opcode);
			break;
		case 0x13:
			executeNCDS(opcode);
			break;
		case 0x14:
			executeCDP(opcode);
			break;
		case 0x16:
			executeNCDT(opcode);
			break;
		case 0x1B:
			executeNCCS(opcode);
			break;
		case 0x1C:
			executeCC(opcode);
			break;
		case 0x1E:
			executeNCS(opcode);
			break;
		case 0x20:
			executeNCT(opcode);
			break;
		case 0x28:
			executeSQR(opcode);
			break;
		case 0x29:
			executeDPCL(opcode);
			break;
		case 0x2A:
			executeDPCT(opcode);
			break;
		case 0x2D:
			executeAVSZ3();
			break;
		case 0x2E:
			executeAVSZ4();
			break;
		case 0x30:
			executeRTPT(opcode);
			break;
		case 0x3D:
			executeGPF(opcode);
			break;
		case 0x3E:
			executeGPL(opcode);
			break;
		case 0x3F:
			executeNCCT(opcode);
			break;
		default:
			HP_FATAL_ERROR("Unexpected GTE opcode %02Xh", realOpcode); // Could be an undocumented "unofficial" opcode, as tested by Amidog's GTE test
			break;
	}
}

// cop2r28 - IRGB - Color conversion Input (R/W)
// 
// Expands 5:5:5 bit RGB (range 0..1Fh) to 16:16:16 bit RGB (range 0000h..0F80h).
//
//  0-4    Red   (0..1Fh) (R/W)  ;multiplied by 80h, and written to IR1
//  5-9    Green (0..1Fh) (R/W)  ;multiplied by 80h, and written to IR2
//  10-14  Blue  (0..1Fh) (R/W)  ;multiplied by 80h, and written to IR3
//  15-31  Not used (always zero) (Read only)
//
// After writing to IRGB, the result can be read from IR3 after TWO nop's, and from IR1,IR2 after THREE nop's
// For uncached code, ONE nop would work. When using IR1,IR2,IR3 as parameters for GTE commands, similar timing
// restrictions might apply... depending on when the specific commands use the parameters?
// 
// https://psx-spx.consoledev.net/geometrytransformationenginegte/#cop2r28-irgb-color-conversion-input-rw
//
void GTE::writeIRGB(u32 val)
{
	ir1 = (val & 0x1fu) << 7;
	ir2 = ((val >> 5) & 0x1fu) << 7;
	ir3 = ((val >> 10) & 0x1fu) << 7;

	irgb = val & 0x7fff; // store lower 15 bits; clear unused upper 17 bits
}

// cop2r29 - ORGB - Color conversion Output (R)
//
// - Collapses 16:16:16 bit RGB (range 0000h..0F80h) to 5:5:5 bit RGB (range 0..1Fh).
// - Negative values (8000h..FFFFh/80h) are saturated to 00h.
// - Large positive values (1000h..7FFFh/80h) are saturated to 1Fh
// - There are no overflow or saturation flags set in cop2r63 though.
//
//   0-4    Red   (0..1Fh) (R)  ;IR1 divided by 80h, saturated to +00h..+1Fh
//   5-9    Green (0..1Fh) (R)  ;IR2 divided by 80h, saturated to +00h..+1Fh
//   10-14  Blue  (0..1Fh) (R)  ;IR3 divided by 80h, saturated to +00h..+1Fh
//   15-31  Not used (always zero) (Read only)
//
// Any changes to IR1,IR2,IR3 are reflected to this register, and, actually also to IRGB
// i.e. ORGB is simply a read-only mirror of IRGB.
// 
// https://psx-spx.consoledev.net/geometrytransformationenginegte/#cop2r29-orgb-color-conversion-output-r
//
u32 GTE::readORGB() const
{
	u32 val = 0;
	val |= Clamp((s16)ir1 / 0x80, 0, 0x1f);
	val |= Clamp((s16)ir2 / 0x80, 0, 0x1f) << 5;
	val |= Clamp((s16)ir3 / 0x80, 0, 0x1f) << 10;
	return val;
}

// The SXYn FIFO has only 3 stages, and a special mirrored register SXYP.
// SXYP is a mirror of SXY2, the difference is that writing to SXYP moves SXY2/SXY1 to SXY1/SXY0,
// whilst writing to SXY2 (or any other SXYn or SZn registers) changes only the written register,
// but doesn't move any other FIFO entries.
// 
// https://psx-spx.consoledev.net/geometrytransformationenginegte/#screen-xyz-coordinate-fifos
//
void GTE::writeSXYP(u32 val)
{
	sxy0 = sxy1;
	sxy1 = sxy2;
	sxy2 = val;
	sxyp = val;
}

// cop2r31 - LZCR - Count Leading Bits Result (R)
//
// Reading LZCR returns the leading 0 count of LZCS if LZCS is positive and the leading 1 count of LZCS if LZCS is negative.
// The results are in range 1..32
// 
// https://psx-spx.consoledev.net/geometrytransformationenginegte/#cop2r31-lzcr-count-leading-bits-result-r
u32 GTE::readLZCR() const
{
	// n.b. The count leading/trailing zero intrinsics are only defined for non-zero values.
	if (lzcs == 0 || (u32)lzcs == 0xffffffff)
	{
		return 32;
	}
	else if (lzcs > 0)
	{
		// Only call with value != 0!
		return COUNT_LEADING_ZEROS(lzcs);
	}
	else
		return COUNT_LEADING_ZEROS(~lzcs); // count leading ones
}

// RTPS - Perspective Transformation (single)
// COP2 0180001h
// #TODO: 15 Cycles
//
// Rotate, translate and perspective transformation on vertex V0.
//
// The points are first multiplied with a rotation matrix (R),
// and after that translated (TR). Finally a perspective transformation is
// applied, which results in 2D screen coordinates. It also returns an
// interpolation value to be used with the various depth cueing instructions.
// 
// Before writing to the FIFOs, the older entries are moved one stage down.
//
//   IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
//   IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
//   IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
//   SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
//   MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
//   MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
//   MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
//
// If the result of the "(((H*20000h/SZ3)+1)/2)" division is greater than 1FFFFh, then the division
// result is saturated to +1FFFFh, and the divide overflow bit in the FLAG register gets set;
// that happens if the vertex is exceeding the "near clip plane", ie. if it is very close to the
// camera (SZ3\<=H/2), exactly at the camara position (SZ3=0), or behind the camera (negative Z
// coordinates are saturated to SZ3=0). 
//
// In:      V0       Vector to transform.                         [1,15,0]
//          R        Rotation matrix                              [1,3,12]
//          TR       Translation vector                           [1,31,0]
//          H        View plane distance                          [0,16,0]
//          DQA      Depth que interpolation values.              [1,7,8]
//          DQB                                                   [1,7,8]
//          OFX      Screen offset values.                        [1,15,16]
//          OFY                                                   [1,15,16]
// 
// Out:     SXY fifo Screen XY coordinates.(short)                [1,15,0]
//          SZ fifo  Screen Z coordinate.(short)                  [0,16,0]
//          IR0      Interpolation value for depth queing.        [1,3,12]
//          IR1      Screen X (short)                             [1,15,0]
//          IR2      Screen Y (short)                             [1,15,0]
//          IR3      Screen Z (short)                             [1,15,0]
//          MAC1     Screen X (long)                              [1,31,0]
//          MAC2     Screen Y (long)                              [1,31,0]
//          MAC3     Screen Z (long)                              [1,31,0]
// 
// Calculation:
// [1,31,0] MAC1=A1[TRX + R11*VX0 + R12*VY0 + R13*VZ0]            [1,31,12]
// [1,31,0] MAC2=A2[TRY + R21*VX0 + R22*VY0 + R23*VZ0]            [1,31,12]
// [1,31,0] MAC3=A3[TRZ + R31*VX0 + R32*VY0 + R33*VZ0]            [1,31,12]
// [1,15,0] IR1= Lm_B1[MAC1]                                      [1,31,0]
// [1,15,0] IR2= Lm_B2[MAC2]                                      [1,31,0]
// [1,15,0] IR3= Lm_B3[MAC3]                                      [1,31,0]
//          SZ0<-SZ1<-SZ2<-SZ3
// [0,16,0] SZ3= Lm_D(MAC3)                                       [1,31,0]
//          SX0<-SX1<-SX2, SY0<-SY1<-SY2
// [1,15,0] SX2= Lm_G1[F[OFX + IR1*(H/SZ)]]                       [1,27,16]
// [1,15,0] SY2= Lm_G2[F[OFY + IR2*(H/SZ)]]                       [1,27,16]
// [1,31,0] MAC0= F[DQB + DQA * (H/SZ)]                           [1,19,24]
// [1,15,0] IR0= Lm_H[MAC0]                                       [1,31,0]
//
// Lm_D means set flag bit 18 SZ3_OTZSaturated if value negative or larger than 16 bits.
// Lm_G means set flag bit 14 SX2Saturated or 13 SY2Saturated if value larger than 10 bits.
// F means that the intermediate result stored in MAC0 sets flag bit 16 if larger than 31 bits and positive, or flag bit 15 if larger than 31 bits and negative.
// Lm_H means set flag bit 12 IR0Saturated if value negative or larger than 12 bits.
//
void GTE::executeRTPS(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 VX0 = (s16)vy0_vx0;
	s16 VY0 = (s16)(vy0_vx0 >> 16);
	s16 VZ0 = (s16)vz0;
	s64 n = rtp(VX0, VY0, VZ0, sf, lm);
	rtpDepthCueing(n);
}

s64 GTE::rtp(s16 VX, s16 VY, s16 VZ, bool sf, bool lm)
{
	// Perform the calculation in 64 bits to emulate 48 bit internal precision and catch overflows.

	// Perform the coordinate system transform:
	//   IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
	//   IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
	//   IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
	Vector64 v{ (s64)VX, (s64)VY, (s64)VZ };
	Vector64 translation{ (s64)trx, (s64)try_, (s64)trz };
	multiplyMatrixVectorAndBias(v, rt, translation, sf);

	// The spec says:
	//   The command saturates IR1,IR2,IR3 to -8000h..+7FFFh *regardless of lm bit*.
	// However this is not true for IR3, which does respect lm.

	//
	// When using RTP with sf=0, then the IR3 saturation flag (FLAG.22) gets set *only* if "MAC3 SAR 12"
	// exceeds -8000h..+7FFFh (although IR3 is saturated when "MAC3" exceeds -8000h..+7FFFh).
	// 
	// MAC1 -> IR1
	if (mac1 > INT16_MAX)
	{
		ir1 = (u32)INT16_MAX;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else if (lm == 0 && mac1 < INT16_MIN)
	{
		ir1 = (u32)INT16_MIN;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else if (lm == 1 && mac1 < 0)
	{
		ir1 = 0;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else
		ir1 = (s16)mac1;

	// MAC2 -> IR2
	if (mac2 > INT16_MAX)
	{
		ir2 = (u32)INT16_MAX;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else if (lm == 0 && mac2 < INT16_MIN)
	{
		ir2 = (u32)INT16_MIN;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else if (lm == 1 && mac2 < 0)
	{
		ir2 = 0;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else
		ir2 = (s16)mac2;

	// MAC3 -> IR3
	// IR3 requires special case handling for RTPS and RTPT.
	// The spec incorrectly says
	//     "The command saturates IR1,IR2,IR3 to -8000h..+7FFFh *regardless of lm bit*."
	// This is incorrect: The LM bit is respected as proved by Amidog GTE brute force test.
	if (mac3 > INT16_MAX)
	{
		ir3 = (u32)INT16_MAX;
	}
	else if (lm == 0 && mac3 < INT16_MIN)
	{
		ir3 = (u32)INT16_MIN;
	}
	else if (lm == 1 && mac3 < 0)
	{
		ir3 = (u32)0;
	}
	else
		ir3 = (s16)mac3;

	// The spec incorrectly says:
	//     "When using RTP with sf=0, then the IR3 saturation flag (FLAG.22) gets set *only* if "MAC3 SAR 12"
	//     exceeds -8000h..+7FFFh (although IR3 value itself is saturated when "MAC3" exceeds -8000h..+7FFFh)."
	// This is incorrect: SF does not factor into the IR3 saturated flag.
	s32 viewZ = (s32)(m3 >> 12); // n.b. Use internal 48-bit register, not 32-bit MAC3
	if ((viewZ < INT16_MIN || viewZ > INT16_MAX))
	{
		flag.IR3Saturated = 1; // bit 22
		// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
	}

	// Update screen Z FIFO
	sz0 = sz1;
	sz1 = sz2;
	sz2 = sz3;

	// Calculate new screen Z and store in SZ3
	if (viewZ < 0)
	{
		sz3 = 0;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else if (viewZ > UINT16_MAX)
	{
		sz3 = UINT16_MAX;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else
		sz3 = (u16)viewZ;

	// Update screen XY FIFO
	sxy0 = sxy1;
	sxy1 = sxy2;

	// Calculate factor used for screen coords and depth cueing.
	// Avoid divide by zero!
	s64 n;

	// If the result of the "(((H*20000h/SZ3)+1)/2)" division is greater than 1FFFFh, then the division
	// result is saturated to +1FFFFh, and the divide overflow bit in the FLAG register gets set.
	if ((u16)h >= (u16)sz3 * 2)
	{
		n = 0x1ffff;
		flag.DivideOverflow = 1;
		flag.ErrorFlag = 1;
	}
	else
	{
#if 0
		// This is logically the correct result...
		n = ((u64)(u16)h << 16) / (s64)(u16)sz3;
		n += 1;
		n >>= 1; // divide by 2
#else
		// ... but the hardware performs the division using the Unsigned Newton-Raphson (UNR) algorithm.
		n = UnsignedNewtonRaphsonDivide((u16)h, (u16)sz3);
#endif
	}

	s64 m0; // internal 48 bit register

	// Calculate new screen X
	// MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
	// or
	// [1,15,0] SX2= Lm_G1[F[OFX + IR1*(H/SZ)]]   calculation performed in [1,27,16]
	m0 = n * (s64)(s16)ir1 + (s64)(s32)ofx;
	checkMac0Overflow(m0);
	mac0 = (s32)m0;
	s64 sx2 = m0 >> 16; // n.b. use 48-bit m0 register, not 32-bit mac0 register

	// Saturate SX2
	if (sx2 < -0x400)
	{
		sx2 = -0x400;
		flag.SX2Saturated = 1;
		flag.ErrorFlag = 1;
	}
	else if (sx2 > 0x3ff)
	{
		sx2 = 0x3ff;
		flag.SX2Saturated = 1;
		flag.ErrorFlag = 1;
	}

	// Calculate new screen Y
	// MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
	// or
	// [1,15,0] SY2= Lm_G2[F[OFY + IR2*(H/SZ)]]   calculation performed in [1,27,16]
	m0 = n * (s64)(s16)ir2 + (s64)(s32)ofy;
	checkMac0Overflow(m0);
	mac0 = (s32)m0;
	s64 sy2 = m0 >> 16;  // n.b. use 48-bit m0 register, not 32-bit mac0 register

	// Saturate SY2
	if (sy2 < -0x400)
	{
		sy2 = -0x400;
		flag.SY2Saturated = 1;
		flag.ErrorFlag = 1;
	}
	else if (sy2 > 0x3ff)
	{
		sy2 = 0x3ff;
		flag.SY2Saturated = 1;
		flag.ErrorFlag = 1;
	}

	// Store sx2 and sy2 in sxy2 register
	sxy2 = ((u32)sy2 << 16) | (u16)sx2; // important to only take lower 16 bits of sx2

	// Don't forget that SXYP is a mirror or SXY2
	sxyp = sxy2;

	return n;
}

void GTE::rtpDepthCueing(s64 n)
{
	// Calculate depth cueing value, using DQA and DQB
	//   MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
	// #TODO: Refactor this into a reusable function: I think there are specific depth cueing operations
	s64 m0 = n * (s64)(s16)dqa + (s64)dqb;
	checkMac0Overflow(m0);
	mac0 = (s32)m0; // This is the final MAC0 value written by this operation.

	// Set flags bit 12 if IR0 saturated to +0000h..+1000h
	// n.b. This flags bit does not factor into the error flag.
	s32 result = (s32)(m0 >> 12); // n.b. use 48-bit internal register, not 32-bit MAC0 register
	if (result < 0)
	{
		ir0 = 0;
		flag.IR0Saturated = true;
	}
	else if (result > 0x1000)
	{
		ir0 = 0x1000;
		flag.IR0Saturated = true;
	}
	else
		ir0 = (s16)result;
}

//
// COP2 0280030h - 23 Cycles - RTPT - Perspective Transformation (triple)
//
void GTE::executeRTPT(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 VX0 = (s16)vy0_vx0;
	s16 VY0 = (s16)(vy0_vx0 >> 16);
	s16 VZ0 = (s16)vz0;
	rtp(VX0, VY0, VZ0, sf, lm);

	s16 VX1 = (s16)vy1_vx1;
	s16 VY1 = (s16)(vy1_vx1 >> 16);
	s16 VZ1 = (s16)vz1;
	rtp(VX1, VY1, VZ1, sf, lm);

	s16 VX2 = (s16)vy2_vx2;
	s16 VY2 = (s16)(vy2_vx2 >> 16);
	s16 VZ2 = (s16)vz2;
	s64 n = rtp(VX2, VY2, VZ2, sf, lm);

	rtpDepthCueing(n);
}

// 06h NCLIP
//
// Determines if a triangle defined by three 2D points is facing towards or away from the camera.
// 
// in:      SXY0,SXY1,SXY2    Screen coordinates                  [1,15,0]
// out:     MAC0              Outerproduct of SXY1 and SXY2 with  [1,31,0]
//                            SXY0 as origin.
// 
// Calculation:
// [1,31,0] MAC0 = F[SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1] [1,43,0]
// Where F means error flag bits 15 MAC0NegativeOverflow and 16 MAC0PositiveOverflow can be set as a result of the calculation.
//
// This is "ORIENT2D" from Real Time Collision Detection p32-33
//
// If the result is positive, then we are facing the front side of the plane, and the triangle should be rendered.
// If the result is negative, then we are facing the back side of the plane, and the triangle should be culled.
// If the result is zero, then the vertices are colinear. This can happen if a projected 3D triangle is edge-on to the camera.
//
// Three screen space points S0, S1 and S2 define a triangle with edges a = S1-S0 and b = S2-S0.
// The 2D determinant of the edges determines if the triangle is front or back facing.
//
//     det(a,b) = |ax ay| = |SX1-SX0 SY1-SY0|
//                |bx by|   |SX2-SX0 SY2-SY0|
//
//     = (SX1-SX0)*(SY2-SY0)-(SY1-SY0)*(SX2-SX0)
//     = SX1*SY2 - SX1*SY0 - SX0*SY2 + SX0*SY0 - SY1*SX2 + SY1*SX0 + SY0*SX2 - SY0*SX0
// 
// The 4th and 8th terms cancel. Re-ordering by positive then negative SX0 SX1 SX2 gives the form from the spec:
//
// ORIENT2D = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
// 
void GTE::executeNCLIP()
{
	// Perform the calculation in 64 bits so can detect 32-bit overflow
	s64 sx0 = (s64)(s16)(sxy0 & 0xffff); // lower 16 bits
	s64 sy0 = (s64)(s16)(sxy0 >> 16); // upper 16 bits
	s64 sx1 = (s64)(s16)(sxy1 & 0xffff); // lower 16 bits
	s64 sy1 = (s64)(s16)(sxy1 >> 16); // upper 16 bits
	s64 sx2 = (s64)(s16)(sxy2 & 0xffff); // lower 16 bits
	s64 sy2 = (s64)(s16)(sxy2 >> 16); // upper 16 bits

	s64 result = sx0*sy1 + sx1*sy2 + sx2*sy0 - sx0*sy2 - sx1*sy0 - sx2*sy1;

	// AFAICT MAC0 is not saturated
	mac0 = (s32)result;

	// Set flags if MAC0 overflowed signed 32 bit storage
	if (result > INT32_MAX)
	{
		flag.MAC0PositiveOverflow = 1;
		flag.ErrorFlag = 1;
	}
	else if (result < INT32_MIN)
	{
		flag.MAC0NegativeOverflow = 1;
		flag.ErrorFlag = 1;
	}
}

// MVMVA(sf,mx,v,cv,lm)
// COP2 0400012h
// #TODO: 8 Cycles
// 
// Multiply vector by matrix and vector addition.
//
// Multiplies a vector with either the rotation matrix, the light matrix or
// the color matrix and then adds the translation vector or background color
// vector.
//
//   Mx = matrix specified by mx  ;RT/LLM/LCM - Rotation, light or color matrix
//   Vx = vector specified by v   ;V0, V1, V2, or [IR1,IR2,IR3]
//   Tx = translation vector specified by cv  ;TR or BK or Bugged/FC, or None
// 
// Calculation:
//   MAC1 = (Tx1*1000h + Mx11*Vx1 + Mx12*Vx2 + Mx13*Vx3) SAR (sf*12)
//   MAC2 = (Tx2*1000h + Mx21*Vx1 + Mx22*Vx2 + Mx23*Vx3) SAR (sf*12)
//   MAC3 = (Tx3*1000h + Mx31*Vx1 + Mx32*Vx2 + Mx33*Vx3) SAR (sf*12)
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
// 
// The GTE also allows selection of the far color vector (FC), but this vector is not
// added correctly by the hardware: The return values are reduced to the last portion
// of the formula, ie. MAC1=(Mx13*Vx3) SAR (sf*12), and similar for MAC2 and MAC3,
// nethertheless, some bits in the FLAG register seem to be adjusted as if the full
// operation would have been executed. Setting Mx=3 selects a garbage matrix (with
// elements -60h, +60h, IR0, RT13, RT13, RT13, RT22, RT22, RT22).
//
void GTE::executeMVMVA(u32 opcode)
{
	// Perform the calculation in 64 bits to emulate 48 bit internal precision and catch overflows.

	// Bit 19 Shift
	bool sf = (opcode >> 19) & 1;

	// Bits 18:17  MVMVA Multiply Matrix    (0=Rotation. 1=Light, 2=Color, 3=Reserved)
	unsigned int mx = (opcode >> 17) & 3;
	// Rotation matrix components are fixed point Q3.12 format (1 bit sign, 3 bits integer, 12 bits fraction)
	s64 R11, R12, R13, R21, R22, R23, R31, R32, R33;
	switch (mx)
	{
		case 0: // Rotation matrix
			R11 = (s64)(s16)rt12_rt11;
			R12 = (s64)(s16)(rt12_rt11 >> 16);
			R13 = (s64)(s16)rt21_rt13;
			R21 = (s64)(s16)(rt21_rt13 >> 16);
			R22 = (s64)(s16)rt23_rt22;
			R23 = (s64)(s16)(rt23_rt22 >> 16);
			R31 = (s64)(s16)rt32_rt31;
			R32 = (s64)(s16)(rt32_rt31 >> 16);
			R33 = (s64)(s16)rt33;
			break;
		case 1: // Light matrix
			R11 = (s64)(s16)l12_l11;
			R12 = (s64)(s16)(l12_l11 >> 16);
			R13 = (s64)(s16)l21_l13;
			R21 = (s64)(s16)(l21_l13 >> 16);
			R22 = (s64)(s16)l23_l22;
			R23 = (s64)(s16)(l23_l22 >> 16);
			R31 = (s64)(s16)l32_l31;
			R32 = (s64)(s16)(l32_l31 >> 16);
			R33 = (s64)(s16)l33;
			break;
		case 2: // Color matrix
			R11 = (s64)(s16)lr2_lr1;
			R12 = (s64)(s16)(lr2_lr1 >> 16);
			R13 = (s64)(s16)lg1_lr3;
			R21 = (s64)(s16)(lg1_lr3 >> 16);
			R22 = (s64)(s16)lg3_lg2;
			R23 = (s64)(s16)(lg3_lg2 >> 16);
			R31 = (s64)(s16)lb2_lb1;
			R32 = (s64)(s16)(lb2_lb1 >> 16);
			R33 = (s64)(s16)lb3;
			break;
#if 0 // Warning unused variables R11 etc...
		case 3: // Reserved
#else // this is probabably better codegen anyway
		default:
#endif
		{
			// Docs say:
			//   Setting Mx=3 selects a garbage matrix with elements:
			//   -60h, +60h, IR0, RT13, RT13, RT13, RT22, RT22, RT22.
			// However, the first two elements are actually derived from RGBC red value! Thanks to kaezrr and starpsx
#if 0
			R11 = (s64)(s16)-0x0060; // #TODO: This may be rgbc red component negated and shifted 4! See starpsx
			R12 = (s64)(s16)0x0060; // #TODO: This may be rgbc red component shifted 4! See starpsx
#else
			s16 r = (s16)(rgbc & 0xff); // zero extended
			R11 = (s64)(-r << 4);
			R12 = (s64)(r << 4);
#endif
			R13 = (s64)(s16)ir0;
			R21 = (s64)(s16)rt21_rt13;
			R22 = (s64)(s16)rt21_rt13;
			R23 = (s64)(s16)rt21_rt13;
			R31 = (s64)(s16)rt23_rt22;
			R32 = (s64)(s16)rt23_rt22;
			R33 = (s64)(s16)rt23_rt22;
			break;
		}
	}
	
	// Bits 16:15  MVMVA Multiply Vector    (0=V0, 1=V1, 2=V2, 3=IR/long)
	unsigned int vx = (opcode >> 15) & 3;
	s64 VX, VY, VZ;
	switch (vx)
	{
		case 0: // V0
			VX = (s64)(s16)vy0_vx0;
			VY = (s64)(s16)(vy0_vx0 >> 16);
			VZ = (s64)(s16)vz0;
			break;
		case 1: // V1;
			VX = (s64)(s16)vy1_vx1;
			VY = (s64)(s16)(vy1_vx1 >> 16);
			VZ = (s64)(s16)vz1;
			break;
		case 2: // V2:
			VX = (s64)(s16)vy2_vx2;
			VY = (s64)(s16)(vy2_vx2 >> 16);
			VZ = (s64)(s16)vz2;
			break;
#if 0 // Warning unused variables R11 etc...
		case 3: // IR
#else // This is probably better codegen anyway!
		default:
#endif
			VX = (s64)(s16)ir1;
			VY = (s64)(s16)ir2;
			VZ = (s64)(s16)ir3;
			break;
	}

	// Bits 14:13  MVMVA Translation Vector (0=TR, 1=BK, 2=FC/Bugged, 3=None)
	unsigned int tx = (opcode >> 13) & 3;
	s64 TX, TY, TZ;
	switch (tx)
	{
		case 0: // TR
			TX = (s64)trx  << 12;
			TY = (s64)try_ << 12;
			TZ = (s64)trz  << 12;
			break;
		case 1: // BK
			TX = (s64)(s32)rbk << 12;
			TY = (s64)(s32)gbk << 12;
			TZ = (s64)(s32)bbk << 12;
			break;
		case 2: // FC/Bugged
			// Convert Far Color components from [1,27,4] to [1,26,16]
			TX = (s64)(s32)m_rfc << 12;
			TY = (s64)(s32)m_gfc << 12;
			TZ = (s64)(s32)m_bfc << 12;
			break;
#if 0 // Warning unused variables R11 etc...
		case 3: // None
#else // this is probabably better codegen anyway
		default:
#endif
			TX = 0;
			TY = 0;
			TZ = 0;
			break;
	}

	// Bit 10     lm - Saturate IR1,IR2,IR3 result (0=To -8000h..+7FFFh, 1=To 0..+7FFFh)
	bool lm = (opcode >> 10) & 1;

	m1 = mac123_s64_to_s44(1, TX + (R11 * VX));
	m2 = mac123_s64_to_s44(2, TY + (R21 * VX));
	m3 = mac123_s64_to_s44(3, TZ + (R31 * VX));

	if (tx == 2) // buggy FC
	{
		if (sf)
		{
			mac1 = (s32)(m1 >> 12);
			mac2 = (s32)(m2 >> 12);
			mac3 = (s32)(m3 >> 12);
		}
		else
		{
			mac1 = (s32)m1;
			mac2 = (s32)m2;
			mac3 = (s32)m3;
		}

		if (mac1 > INT16_MAX)
		{
			flag.IR1Saturated = 1; // bit 24
			flag.ErrorFlag = 1;
		}
		else if (mac1 < INT16_MIN)
		{
			flag.IR1Saturated = 1; // bit 24
			flag.ErrorFlag = 1;
		}

		// MAC2 -> IR2
		if (mac2 > INT16_MAX)
		{
			flag.IR2Saturated = 1; // bit 23
			flag.ErrorFlag = 1;
		}
		else if (mac2 < INT16_MIN)
		{
			flag.IR2Saturated = 1; // bit 23
			flag.ErrorFlag = 1;
		}

		// MAC3 -> IR3
		if (mac3 > INT16_MAX)
		{
			flag.IR3Saturated = 1; // bit 22
			// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
		}
		else if (mac3 < INT16_MIN)
		{
			flag.IR3Saturated = 1; // bit 22
			// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
		}

		m1 = 0;
		m2 = 0;
		m3 = 0;
	}

	m1 = mac123_s64_to_s44(1, m1 + (R12 * VY));
	m2 = mac123_s64_to_s44(2, m2 + (R22 * VY));
	m3 = mac123_s64_to_s44(3, m3 + (R32 * VY));

	m1 = mac123_s64_to_s44(1, m1 + (R13 * VZ));
	m2 = mac123_s64_to_s44(2, m2 + (R23 * VZ));
	m3 = mac123_s64_to_s44(3, m3 + (R33 * VZ));

	if (sf)
	{
		mac1 = (s32)(m1 >> 12);
		mac2 = (s32)(m2 >> 12);
		mac3 = (s32)(m3 >> 12);
	}
	else
	{
		mac1 = (s32)m1;
		mac2 = (s32)m2;
		mac3 = (s32)m3;
	}

	copyMAC123toIR123(lm);
}

// NCS
// 
// Normal color
// 
// Calculates a color from a surface normal and the light sources and colors.
// The basic color of the plane or point the normal refers to is assumed to be white.
//
// Cycles: 14
// Opcode:  cop2 $0C8041E
// 
// In:      V0                Normal vector                       [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          CODE              Code value from RGB.           CODE [0,8,0]
//          LCM               Color matrix                        [1,3,12]
//          LLM               Light matrix                        [1,3,12]
// 
// Out:     RGBn              RGB fifo.              Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// [1,19,12] MAC1=A1[L11*VX0 + L12*VY0 + L13*VZ0]                 [1,19,24]
// [1,19,12] MAC2=A2[L21*VX0 + L22*VY0 + L23*VZ0]                 [1,19,24]
// [1,19,12] MAC3=A3[L31*VX0 + L32*VY0 + L33*VZ0]                 [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3]           [1,19,24]
// [1,19,12] MAC2=A2[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3]           [1,19,24]
// [1,19,12] MAC3=A3[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3]           [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// From psx-spx:
//
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
// 
void GTE::executeNCS(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 VX = (s16)vy0_vx0;
	s16 VY = (s16)(vy0_vx0 >> 16);
	s16 VZ = (s16)vz0;
	Vector64 V0{ (s64)VX, (s64)VY, (s64)VZ };
	nc(V0, sf, lm);
}

void GTE::nc(const Vector64& v, bool sf, bool lm)
{
	// [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
	// The rows of the light matrix are the directions to each of the 3 lights.
	// After the matrix multiplication IR0, IR1, and IR2 will be the dot product of the normal
	// vector V and the light directions.

	multiplyMatrixVector(v, lightSourceMatrix, sf);

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	// Usually lm would be 1 here so saturate the result to [0,1]
	copyMAC123toIR123(lm);

	// Now scale by the colour of each light and add background (ambient) light colour.
	// [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
	Vector64 IR{ (s64)(s16)ir1, (s64)(s16)ir2, (s64)(s16)ir3 };
	Vector64 BK{ (s64)(s32)rbk, (s64)(s32)gbk, (s64)(s32)bbk };
	multiplyMatrixVectorAndBias(IR, lightColorMatrix, BK, sf);

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	copyMAC123toIR123(lm);

	// Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE]
	pushMAC123toColorFIFO();
}

// NCT
// Normal Color
// Cycles: 30
// 
// Opcode:  cop2 $0D80420
// 
// In:      V0,V1,V2          Normal vector                       [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          CODE              Code value from RGB.           CODE [0,8,0]
//          LCM               Color matrix                        [1,3,12]
//          LLM               Light matrix                        [1,3,12]
// Out:     RGBn              RGB fifo.              Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculation: Same as NCS, but repeated for V1 and V2.
// 
// Calculates a color from the normal of a point or plane and the light sources and colors.
// The basic color of the plane or point the normal refers to is assumed to be white.
//
void GTE::executeNCT(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 V0X = (s16)vy0_vx0;
	s16 V0Y = (s16)(vy0_vx0 >> 16);
	s16 V0Z = (s16)vz0;
	Vector64 V0{ (s64)V0X, (s64)V0Y, (s64)V0Z };
	nc(V0, sf, lm);

	s16 V1X = (s16)vy1_vx1;
	s16 V1Y = (s16)(vy1_vx1 >> 16);
	s16 V1Z = (s16)vz1;
	Vector64 V1{ (s64)V1X, (s64)V1Y, (s64)V1Z };
	nc(V1, sf, lm);

	s16 V2X = (s16)vy2_vx2;
	s16 V2Y = (s16)(vy2_vx2 >> 16);
	s16 V2Z = (s16)vz2;
	Vector64 V2{ (s64)V2X, (s64)V2Y, (s64)V2Z };
	nc(V2, sf, lm);
}

// NCDS
// 
// Normal Color Depth Cue (Single vector)
//
// Calculates a color from a surface normal and the light sources and colors. 
// Then performs depth cueing.
// The basic color of the plane or point the normal refers to is assumed to be white.
// 
// Opcode:  cop2 $0e80413 (13h)
// #TODO: 19 cycles
// 
// Fields:  none
// In:      V0                Normal vector                       [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          RGB               Primary color          R,G,B,CODE   [0,8,0]
//          LLM               Light matrix                        [1,3,12]
//          LCM               Color matrix                        [1,3,12]
//          IR0               Interpolation value                 [1,3,12]
// 
// Out:     RGBn              RGB fifo.              Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculation:
// [1,19,12] MAC1=A1[L11*VX0 + L12*VY0 + L13*VZ0]                 [1,19,24]
// [1,19,12] MAC2=A1[L21*VX0 + L22*VY0 + L23*VZ0]                 [1,19,24]
// [1,19,12] MAC3=A1[L31*VX0 + L32*VY0 + L33*VZ0]                 [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3]           [1,19,24]
// [1,19,12] MAC2=A1[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3]           [1,19,24]
// [1,19,12] MAC3=A1[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3]           [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [1,27,4]  MAC1=A1[R*IR1 + IR0*(Lm_B1[RFC-R*IR1])]              [1,27,16][lm=0]  <- Same as DPCL logic from here
// [1,27,4]  MAC2=A1[G*IR2 + IR0*(Lm_B2[GFC-G*IR2])]              [1,27,16][lm=0]
// [1,27,4]  MAC3=A1[B*IR3 + IR0*(Lm_B3[BFC-B*IR3])]              [1,27,16][lm=0]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,27,4][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,27,4][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,27,4][lm=1]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// From psx-spx:
// In: V0=Normal vector (for triple variants repeated with V1 and V2), BK=Background color, RGBC=Primary color/code, LLM=Light matrix, LCM=Color matrix, IR0=Interpolation value.
//
//  [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
//  [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
//  [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
//  [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for NCDx only
//  [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)       ;<--- for NCDx/NCCx
//  Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeNCDS(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 VX = (s16)vy0_vx0;
	s16 VY = (s16)(vy0_vx0 >> 16);
	s16 VZ = (s16)vz0;
	Vector64 V0{ (s64)VX, (s64)VY, (s64)VZ };
	ncd(V0, sf, lm);
}

void GTE::ncd(const Vector64& v, bool sf, bool lm)
{
	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
	// The rows of the light matrix are the directions to each of the 3 lights.
	// After the matrix multiplication IR0, IR1, and IR2 will be the dot product of the normal
	// vector V and the light directions.
	multiplyMatrixVector(v, lightSourceMatrix, sf);

	// Usually lm would be 1 here so saturate the result to [0,1]
	copyMAC123toIR123(lm);

	// Now scale by the colour of each light and add background (ambient) light colour.
	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
	Vector64 IR{ (s64)(s16)ir1, (s64)(s16)ir2, (s64)(s16)ir3 };
	Vector64 BK{ (s64)(s32)rbk, (s64)(s32)gbk, (s64)(s32)bbk };
	multiplyMatrixVectorAndBias(IR, lightColorMatrix, BK, sf);

	copyMAC123toIR123(lm);

	// Rest of logic is DPCL
	dpcl(sf, lm);
}

// NCDT
// 
// Normal Color Depth Cue.
//
// Same as NCDS but repeats for V1 and V2.
// 
void GTE::executeNCDT(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 V0X = (s16)vy0_vx0;
	s16 V0Y = (s16)(vy0_vx0 >> 16);
	s16 V0Z = (s16)vz0;
	Vector64 V0{ (s64)V0X, (s64)V0Y, (s64)V0Z };
	ncd(V0, sf, lm);

	s16 V1X = (s16)vy1_vx1;
	s16 V1Y = (s16)(vy1_vx1 >> 16);
	s16 V1Z = (s16)vz1;
	Vector64 V1{ (s64)V1X, (s64)V1Y, (s64)V1Z };
	ncd(V1, sf, lm);

	s16 V2X = (s16)vy2_vx2;
	s16 V2Y = (s16)(vy2_vx2 >> 16);
	s16 V2Z = (s16)vz2;
	Vector64 V2{ (s64)V2X, (s64)V2Y, (s64)V2Z };
	ncd(V2, sf, lm);
}

// NCCS
//
// Normal Color Color single vector
//
// Calculates a color from the normal of a point or plane and the light sources and colors.
// Same NCS/NCT, but the base color of the plane or point is taken into account.
// 
// Opcode:  cop2 $108041B
//
// Cycles: 17
// 
// In:      V0                Normal vector                       [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          RGB               Primary color          R,G,B,CODE   [0,8,0]
//          LLM               Light matrix                        [1,3,12]
//          LCM               Color matrix                        [1,3,12]
// 
// Out:     RGBn              RGB fifo.              Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculation:
// 
// [1,19,12] MAC1=A1[L11*VX0 + L12*VY0 + L13*VZ0]                  [1,19,24]
// [1,19,12] MAC2=A2[L21*VX0 + L22*VY0 + L23*VZ0]                  [1,19,24]
// [1,19,12] MAC3=A3[L31*VX0 + L32*VY0 + L33*VZ0]                  [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                      [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                      [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                      [1,19,12][lm=1]
// [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3]            [1,19,24]
// [1,19,12] MAC2=A2[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3]            [1,19,24]
// [1,19,12] MAC3=A3[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3]            [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                      [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                      [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                      [1,19,12][lm=1]
// [1,27,4]  MAC1=A1[R*IR1]                                        [1,27,16]
// [1,27,4]  MAC2=A2[G*IR2]                                        [1,27,16]
// [1,27,4]  MAC3=A3[B*IR3]                                        [1,27,16]
// [1,3,12]  IR1= Lm_B1[MAC1]                                      [1,27,4][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                      [1,27,4][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                      [1,27,4][lm=1]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                              [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                              [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                              [1,27,4]
//
// From psx-spx:
//
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
//   [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
//   [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)       ;<--- for NCDx/NCCx
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeNCCS(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 VX = (s16)vy0_vx0;
	s16 VY = (s16)(vy0_vx0 >> 16);
	s16 VZ = (s16)vz0;
	Vector64 V0{ (s64)VX, (s64)VY, (s64)VZ };
	ncc(V0, sf, lm);
}

void GTE::ncc(const Vector64& v, bool sf, bool lm)
{
	// [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
	// The rows of the light matrix are the directions to each of the 3 lights.
	// After the matrix multiplication IR0, IR1, and IR2 will be the dot product of the normal
	// vector V and the light directions.
	multiplyMatrixVector(v, lightSourceMatrix, sf);

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	// Usually lm would be 1 here so saturate the result to [0,1]
	copyMAC123toIR123(lm);

	// Now scale by the colour of each light and add background (ambient) light colour.
	// [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
	Vector64 IR{ (s64)(s16)ir1, (s64)(s16)ir2, (s64)(s16)ir3 };
	Vector64 BK{ (s64)(s32)rbk, (s64)(s32)gbk, (s64)(s32)bbk };
	multiplyMatrixVectorAndBias(IR, lightColorMatrix, BK, sf);

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	copyMAC123toIR123(lm);

	// Multiply RGB by IR vector to find colour.
	// [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
	s64 r = (u64)rgbc & 0xffu;
	s64 g = ((u64)rgbc >> 8) & 0xffu;
	s64 b = ((u64)rgbc >> 16) & 0xffu;
	m1 = mac123_s64_to_s44(1, (r * (s64)(s16)ir1) << 4);
	m2 = mac123_s64_to_s44(2, (g * (s64)(s16)ir2) << 4);
	m3 = mac123_s64_to_s44(3, (b * (s64)(s16)ir3) << 4);

	if (sf)
	{
		m1 >>= 12;
		m2 >>= 12;
		m3 >>= 12;
	}

	mac1 = (s32)m1;
	mac2 = (s32)m2;
	mac3 = (s32)m3;

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	copyMAC123toIR123(lm);

	// Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE]
	pushMAC123toColorFIFO();
}

void GTE::executeNCCT(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	s16 V0X = (s16)vy0_vx0;
	s16 V0Y = (s16)(vy0_vx0 >> 16);
	s16 V0Z = (s16)vz0;
	Vector64 V0{ (s64)V0X, (s64)V0Y, (s64)V0Z };
	ncc(V0, sf, lm);

	s16 V1X = (s16)vy1_vx1;
	s16 V1Y = (s16)(vy1_vx1 >> 16);
	s16 V1Z = (s16)vz1;
	Vector64 V1{ (s64)V1X, (s64)V1Y, (s64)V1Z };
	ncc(V1, sf, lm);

	s16 V2X = (s16)vy2_vx2;
	s16 V2Y = (s16)(vy2_vx2 >> 16);
	s16 V2Z = (s16)vz2;
	Vector64 V2{ (s64)V2X, (s64)V2Y, (s64)V2Z };
	ncc(V2, sf, lm);
}

// COP2 0A00428h+sf*80000h - 5 Cycles - SQR(sf) - Square vector
// 
// Calculates the square of a vector.
//
//                                                        sf=0    sf=1
// in:      [IR1,IR2,IR3]     vector                      [1,15,0][1,3,12]
// out:     [IR1,IR2,IR3]     vector^2                    [1,15,0][1,3,12]
//          [MAC1,MAC2,MAC3]  vector^2                    [1,31,0][1,19,12]
//
// Calculation: (left format sf=0, right format sf=1)
// 
// [1,31,0][1,19,12] MAC1=A1[IR1*IR1]                     [1,43,0][1,31,12]
// [1,31,0][1,19,12] MAC2=A2[IR2*IR2]                     [1,43,0][1,31,12]
// [1,31,0][1,19,12] MAC3=A3[IR3*IR3]                     [1,43,0][1,31,12]
// [1,15,0][1,3,12]  IR1=Lm_B1[MAC1]                      [1,31,0][1,19,12][lm=1]
// [1,15,0][1,3,12]  IR2=Lm_B2[MAC2]                      [1,31,0][1,19,12][lm=1]
// [1,15,0][1,3,12]  IR3=Lm_B3[MAC3]                      [1,31,0][1,19,12][lm=1]
//
// From psx-spx:
//     [MAC1,MAC2,MAC3] = [IR1*IR1,IR2*IR2,IR3*IR3] SHR (sf*12)
//     [IR1,IR2,IR3]    = [MAC1,MAC2,MAC3]    ;IR1,IR2,IR3 saturated to max 7FFFh
// 
// The result is, of course, always positive, so the "lm" flag for negative saturation has no effect.
//
void GTE::executeSQR(u32 opcode)
{
	unsigned int sf = (opcode >> 19) & 1;

//	unsigned int lm = (opcode >> 10) & 1;
//	HP_ASSERT(lm == 0); assert redundant: result always positive so the "lm" flag for negative saturation has no effect.

	// IR1, IR2 and IR3 are treated as 16-bit signed integers, but the multiplication is done in 32-bit precision, and the result is stored in 32-bit MAC1, MAC2 and MAC3 registers.
	mac1 = ((s32)(s16)ir1 * (s32)(s16)ir1);
	mac2 = ((s32)(s16)ir2 * (s32)(s16)ir2);
	mac3 = ((s32)(s16)ir3 * (s32)(s16)ir3);

	// If IRx are in Q3.12 format, then the product needs to be divided by 2^12.
	if (sf)
	{
		mac1 >>= 12;
		mac2 >>= 12;
		mac3 >>= 12;
	}

	// MAC cannot overflow because we are multiplying two 16-bit values and storing in a 32-bit type.

	if (mac1 > 0x7fff) // larger than 15 bits?
	{
		ir1 = 0x7fff;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else
		ir1 = mac1;

	if (mac2 > 0x7fff) // larger than 15 bits?
	{
		ir2 = 0x7fff;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else
		ir2 = mac2;

	if (mac3 > 0x7fff) // larger than 15 bits?
	{
		ir3 = 0x7fff;
		flag.IR3Saturated = 1; // bit 22
		// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
	}
	else
		ir3 = mac3;
}

// AVSZ3    5        Average of three Z values
// fields:
// Opcode:  cop2 $158002D
// 
// in:      SZ1, SZ2, SZ3     Z-Values                            [0,16,0]
//          ZSF3              Divider                             [1,3,12]
// out:     OTZ               Average.                            [0,16,0]
//          MAC0              Average.                            [1,31,0]
// 
// Calculation:
// [1,31,0] MAC0=F[ZSF3*SZ1 + ZSF3*SZ2 + ZSF3*SZ3]                [1,31,12]
// [0,16,0] OTZ=Lm_D[MAC0]                                        [1,31,0]
// Where:
//     F    means error flag bits 15 MAC0NegativeOverflow and 16 MAC0PositiveOverflow can be set as a result of the calculation.
//     D    Value negative or larger than 16 bits. Flag bit 18 SZ3_OTZSaturated
void GTE::executeAVSZ3()
{
	s64 result = ((s64)(s16)zsf3 * (u64)(u16)sz1) + ((s64)(s16)zsf3 * (u64)(u16)sz2) + ((s64)(s16)zsf3 * (u64)(u16)sz3);

	// AFAICT MAC0 is not saturated
	mac0 = (s32)result;

	// Set flags if MAC0 overflowed signed 32 bit storage
	if (result > INT32_MAX)
	{
		flag.MAC0PositiveOverflow = 1;
		flag.ErrorFlag = 1;
	}
	else if (result < INT32_MIN)
	{
		flag.MAC0NegativeOverflow = 1;
		flag.ErrorFlag = 1;
	}

	// Convert result from Q31.12 to Q31.0 for OTZ
	result >>= 12;

	if (result < 0)
	{
		otz = 0;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else if (result > UINT16_MAX)
	{
		otz = UINT16_MAX;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else
		otz = (u16)result;
}

// AVSZ4    6        Average of four Z values
// Fields:
// Opcode:  cop2 $168002E
// 
// in:      SZ1,SZ2,SZ3,SZ4   Z-Values                            [0,16,0]
//          ZSF4              Divider                             [1,3,12]
// out:     OTZ               Average.                            [0,16,0]
//          MAC0              Average.                            [1,31,0]
// 
// Calculation:
// [1,31,0] MAC0=F[ZSF4*SZ0 + ZSF4*SZ1 + ZSF4*SZ2 + ZSF4*SZ3]     [1,31,12]
// [0,16,0] OTZ=Lm_D[MAC0]                                        [1,31,0]
// Where:
//     F    means error flag bits 15 MAC0NegativeOverflow and 16 MAC0PositiveOverflow can be set as a result of the calculation.
//     D    Value negative or larger than 16 bits. Flag bit 18 SZ3_OTZSaturated
void GTE::executeAVSZ4()
{
	s64 result = ((s64)(s16)zsf4 * (u64)(u16)sz0) + ((s64)(s16)zsf4 * (u64)(u16)sz1) + ((s64)(s16)zsf4 * (u64)(u16)sz2 + ((s64)(s16)zsf4 * (u64)(u16)sz3));

	// AFAICT MAC0 is not saturated
	mac0 = (s32)result;

	// Set flags if MAC0 overflowed signed 32 bit storage
	if (result > INT32_MAX)
	{
		flag.MAC0PositiveOverflow = 1;
		flag.ErrorFlag = 1;
	}
	else if (result < INT32_MIN)
	{
		flag.MAC0NegativeOverflow = 1;
		flag.ErrorFlag = 1;
	}

	// Convert result from Q31.12 to Q31.0 for OTZ
	result >>= 12;

	if (result < 0)
	{
		otz = 0;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else if (result > UINT16_MAX)
	{
		otz = UINT16_MAX;
		flag.SZ3_OTZSaturated = 1;
		flag.ErrorFlag = 1;
	}
	else
		otz = (u16)result;
}

// OP       6        Outer product (cross product) of 2 signed 16-bit vectors
// Fields:  sf
// Opcode:  cop2 $170000C
// 
// in:      [R11R12,R22R23,R33] vector 1
//          [IR1,IR2,IR3]      vector 2
// out:     [IR1,IR2,IR3]      outer product
//          [MAC1,MAC2,MAC3]   outer product
// 
// Calculation: (D1=R11R12,D2=R22R23,D3=R33)
// 
//          MAC1=A1[D2*IR3 - D3*IR2]
//          MAC2=A2[D3*IR1 - D1*IR3]
//          MAC3=A3[D1*IR2 - D2*IR1]
//          IR1=Lm_B1[MAC1]
//          IR2=Lm_B2[MAC2]
//          IR3=Lm_B3[MAC3]
//
// Where
// - A means flag bits 30:25 are set if MAC values exceed range
// - Lm_B means flag bits 24:22 are set if value negative(lm=1) or larger than 15 bits(lm=0)
//
// From psx-spx:
//   [MAC1,MAC2,MAC3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2] SAR (sf*12)
//   [IR1,IR2,IR3]    = [MAC1,MAC2,MAC3]                        ;copy result
//
//  Note: D1,D2,D3 are meant to be the RT11,RT22,RT33 elements of the RT matrix "misused" as a vector.
//
// Derivation:
// 
// The cross product of two 3D vectors a and b results in another vector:
//
//     a x b = |x^ y^ z^|     where ^ means unit vector
//             |ax ay az|
//             |bx by bz|
// 
//           = (ay*bz - az-by, -(ax*bz - az*bx), ax*by - ay*bx)
//
// If a = D and b = IR and (x,y,z) dimensions are represented by indices (1,2,3) then this is the
// same as the form givn in the spec:
//
//           = (D2*IR3 - D3-IR2, -(D1*IR3 - D3*IR1), D1*IR2 - D2*IR1)
//
void GTE::executeOP(u32 opcode)
{
	unsigned int sf = (opcode >> 19) & 1;

	// Perform the calcuation in signed 32 bits
	s32 d1 = (s32)(s16)rt12_rt11; // R11
	s32 d2 = (s32)(s16)rt23_rt22; // R22
	s32 d3 = (s32)(s16)rt33;      // R33
	mac1 = (s32)(s16)ir3 * d2 - (s32)(s16)ir2 * d3;
	mac2 = (s32)(s16)ir1 * d3 - (s32)(s16)ir3 * d1;
	mac3 = (s32)(s16)ir2 * d1 - (s32)(s16)ir1 * d2;

	if (sf)
	{
		mac1 >>= 12;
		mac2 >>= 12;
		mac3 >>= 12;
	}

	// MAC cannot overflow because we are multiplying two 16-bit values and storing in a 32-bit type.

	unsigned int lm = (opcode >> 10) & 1;
	copyMAC123toIR123(lm);
}

// GPF      5        General purpose interpolation
// Fields:  sf
// Opcode:  cop2 $190003D
// 
// in:      IR0               scaling factor
//          CODE              code field of RGB
//          [IR1,IR2,IR3]     vector
// out:     [IR1,IR2,IR3]     vector
//          [MAC1,MAC2,MAC3]  vector
//          RGB2              RGB fifo.
// 
// Calculation:
// 
//          MAC1=A1[IR0 * IR1]
//          MAC2=A2[IR0 * IR2]
//          MAC3=A3[IR0 * IR3]
//          IR1=Lm_B1[MAC1]
//          IR2=Lm_B2[MAC2]
//          IR3=Lm_B3[MAC3]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]
//
// - Lm_B means flag bits 24:22 are set if value negative(lm=1) or larger than 15 bits(lm=0)
// - Lm_C means flag bits 21:19 are set if colour component negative or larger than 8 bits.
//
void GTE::executeGPF(u32 opcode)
{
	unsigned int sf = (opcode >> 19) & 1;
	unsigned int lm = (opcode >> 10) & 1;

	// Perform the calcuation in signed 32 bits
	mac1 = (s32)(s16)ir0 * (s32)(s16)ir1;
	mac2 = (s32)(s16)ir0 * (s32)(s16)ir2;
	mac3 = (s32)(s16)ir0 * (s32)(s16)ir3;

	if (sf)
	{
		mac1 >>= 12;
		mac2 >>= 12;
		mac3 >>= 12;
	}

	// The 32-bit MAC123 registers cannot overflow because we are multiplying two 16-bit values and storing in a 32-bit type.

	copyMAC123toIR123(lm);
	pushMAC123toColorFIFO();
}

// GPL      5        General purpose interpolation
// Fields:  sf
// Opcode:  cop2 $1A0003E
// 
// in:      IR0               scaling factor
//          CODE              code field of RGB
//          [IR1,IR2,IR3]     vector
//          [MAC1,MAC2,MAC3]  vector
// out:     [IR1,IR2,IR3]     vector
//          [MAC1,MAC2,MAC3]  vector
//          RGB2              RGB fifo.
// 
// Calculation:
// 
//          MAC1=A1[MAC1 + IR0 * IR1]
//          MAC2=A2[MAC2 + IR0 * IR2]
//          MAC3=A3[MAC3 + IR0 * IR3]
//          IR1=Lm_B1[MAC1]
//          IR2=Lm_B2[MAC2]
//          IR3=Lm_B3[MAC3]
// [0,8,0]  Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]  R0<-R1<-R2<- Lm_C1[MAC1]
// [0,8,0]  G0<-G1<-G2<- Lm_C2[MAC2]
// [0,8,0]  B0<-B1<-B2<- Lm_C3[MAC3]
//
// - Lm_B means flag bits 24:22 are set if value negative(lm=1) or larger than 15 bits(lm=0)
// - Lm_C means flag bits 21:19 are set if colour component negative or larger than 8 bits.
//
// From psx-spx:
//
//   [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SHL (sf*12)
//   [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeGPL(u32 opcode)
{
	unsigned int sf = (opcode >> 19) & 1;
	unsigned int lm = (opcode >> 10) & 1;

	// The internal ALU registers are 44 bits.
	// The emulation works in 64 bits.
	m1 = (s64)mac1;
	m2 = (s64)mac2;
	m3 = (s64)mac3;

	if (sf)
	{
		m1 <<= 12;
		m2 <<= 12;
		m3 <<= 12;
	}

	m1 = mac123_s64_to_s44(1, m1 + (s64)(s16)ir0 * (s64)(s16)ir1);
	m2 = mac123_s64_to_s44(2, m2 + (s64)(s16)ir0 * (s64)(s16)ir2);
	m3 = mac123_s64_to_s44(3, m3 + (s64)(s16)ir0 * (s64)(s16)ir3);

	if (sf)
	{
		m1 >>= 12;
		m2 >>= 12;
		m3 >>= 12;
	}

	mac1 = (s32)m1;
	mac2 = (s32)m2;
	mac3 = (s32)m3;

	copyMAC123toIR123(lm);
	pushMAC123toColorFIFO();
}

// CDP
//
// Color Depth Que
//
// Opcode:  cop2 $1280414
//
// A color is calculated from a light vector.
// Base color is assumed to be white.
// Depth cueing is performed (like DPCS).
//
// In:      [IR1,IR2,IR3]     Vector                              [1,3,12]
//          RGB               Primary color          R,G,B,CODE   [0,8,0]
//          IR0               Interpolation value                 [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          LCM               Color matrix                        [1,3,12]
//          FC                Far color              RFC,GFC,BFC  [1,27,4]
// Out:     RGBn              RGB fifo               Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculation:
// [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3]           [1,19,24]
// [1,19,12] MAC2=A2[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3]           [1,19,24]
// [1,19,12] MAC3=A3[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3]           [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [1,27,4]  MAC1=A1[R*IR1 + IR0*(Lm_B1[RFC-R*IR1])]              [1,27,16][lm=0]  <-- dpcl logic from here
// [1,27,4]  MAC2=A2[G*IR2 + IR0*(Lm_B2[GFC-G*IR2])]              [1,27,16][lm=0]
// [1,27,4]  MAC3=A3[B*IR3 + IR0*(Lm_B3[BFC-B*IR3])]              [1,27,16][lm=0]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,27,4][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,27,4][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,27,4][lm=1]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// From psx-spx:
//
//   [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
//   [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
//   [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
//   [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeCDP(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
	Vector64 IR{ (s64)(s16)ir1, (s64)(s16)ir2, (s64)(s16)ir3 };
	Vector64 backgroundColor{ (s64)(s32)rbk, (s64)(s32)gbk, (s64)(s32)bbk };
	multiplyMatrixVectorAndBias(IR, lightColorMatrix, backgroundColor, sf);
	copyMAC123toIR123(lm);

	dpcl(sf, lm);
}

// CC
// "Color Color"
// 
// Cycles: 11
// Fields:  none
// Opcode:  cop2 $138041C
// In:      [IR1,IR2,IR3]     Vector                              [1,3,12]
//          BK                Background color       RBK,GBK,BBK  [1,19,12]
//          RGB               Primary color          R,G,B,CODE   [0,8,0]
//          LCM               Color matrix                        [1,3,12]
// Out:     RGBn              RGB fifo.              Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculations:
// [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3]           [1,19,24]
// [1,19,12] MAC2=A2[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3]           [1,19,24]
// [1,19,12] MAC3=A3[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3]           [1,19,24]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,19,12][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,19,12][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,19,12][lm=1]
// [1,27,4]  MAC1=A1[R*IR1]                                       [1,27,16]
// [1,27,4]  MAC2=A2[G*IR2]                                       [1,27,16]
// [1,27,4]  MAC3=A3[B*IR3]                                       [1,27,16]
// [1,3,12]  IR1= Lm_B1[MAC1]                                     [1,27,4][lm=1]
// [1,3,12]  IR2= Lm_B2[MAC2]                                     [1,27,4][lm=1]
// [1,3,12]  IR3= Lm_B3[MAC3]                                     [1,27,4][lm=1]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// From psx-spx:
// 
//  [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
//  [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
//  [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
//  Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
void GTE::executeCC(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	// [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
	Vector64 IR{ (s64)(s16)ir1, (s64)(s16)ir2, (s64)(s16)ir3 };
	Vector64 backgroundColor{ (s64)(s32)rbk, (s64)(s32)gbk, (s64)(s32)bbk };
	multiplyMatrixVectorAndBias(IR, lightColorMatrix, backgroundColor, sf);
	copyMAC123toIR123(lm);

	// [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
	s64 r = (u64)rgbc & 0xffu;
	s64 g = ((u64)rgbc >> 8) & 0xffu;
	s64 b = ((u64)rgbc >> 16) & 0xffu;

	m1 = mac123_s64_to_s44(1, (r * (s64)(s16)ir1) << 4);
	m2 = mac123_s64_to_s44(2, (g * (s64)(s16)ir2) << 4);
	m3 = mac123_s64_to_s44(3, (b * (s64)(s16)ir3) << 4);

	// [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
	if (sf)
	{
		m1 >>= 12;
		m2 >>= 12;
		m3 >>= 12;
	}

	mac1 = (s32)m1;
	mac2 = (s32)m2;
	mac3 = (s32)m3;

	// Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
	copyMAC123toIR123(lm);
	pushMAC123toColorFIFO();
}

// DPCS - Depth Cueing (Single)
//
// Interpolation of RGBC color code register and far color.
//
// Opcode:  cop2 $0780010
// 
// In:      IR0               Interpolation value                 [1,3,12]
//          RGB               Color                  R,G,B,CODE   [0,8,0]
//          FC                Far color              RFC,GFC,BFC  [1,27,4]
// 
// Out:     RGBn              RGB fifo               Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculations:
// [1,27,4]  MAC1=A1[(R + IR0*(Lm_B1[RFC - R])]                   [1,27,16][lm=0]
// [1,27,4]  MAC2=A2[(G + IR0*(Lm_B1[GFC - G])]                   [1,27,16][lm=0]
// [1,27,4]  MAC3=A3[(B + IR0*(Lm_B1[BFC - B])]                   [1,27,16][lm=0]
// [1,11,4]  IR1=Lm_B1[MAC1]                                      [1,27,4][lm=0]
// [1,11,4]  IR2=Lm_B2[MAC2]                                      [1,27,4][lm=0]
// [1,11,4]  IR3=Lm_B3[MAC3]                                      [1,27,4][lm=0]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// - Ax means flag bits 30:25 are set if MACx values exceed range
// - Lm_B means flag bits 24:22 (IR123 saturated) are set if value negative(lm=1) or larger than 15 bits(lm=0)
// - Lm_C means flag bits 21:19 (ColorFIFO rgb saturated) are set if colour component negative or larger than 8 bits.
//
// From psx-spx:
//
//   [MAC1,MAC2,MAC3] = [R,G,B] SHL 16
//   [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
//   [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeDPCS(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	// The interpolation is performed in fixed point [1,27,16] format.
	// Convert 8-bit color to 8.16
	s64 r = ((u64)rgbc & 0xffu) << 16;
	s64 g = (((u64)rgbc >> 8) & 0xffu) << 16;
	s64 b = (((u64)rgbc >> 16) & 0xffu) << 16;

	interpolateColorAndFarColor(r, g, b, sf, lm);
}

//
// Repeats DPCS three times: on the RGB0,RGB1,RGB2 colours in the FIFO
//
void GTE::executeDPCT(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	// The interpolation is performed in fixed point [1,27,16] format, so 8-bit colour components must be shifted left 16 bits.

	// The color FIFO moves each time, so perform operation on RGB0 thrice.
	for (unsigned int i = 0; i < 3; i++)
	{
		s64 r = ((u64)rgb0 & 0xffu) << 16;
		s64 g = (((u64)rgb0 >> 8) & 0xffu) << 16;
		s64 b = (((u64)rgb0 >> 16) & 0xffu) << 16;
		interpolateColorAndFarColor(r, g, b, sf, lm);
	}
}

// INTPL
//
// Interpolation of vector (IR1,IR2,IR3) and far color.
//
// #TODO: 8 cycles
// 
// Fields:  none
// Opcode:  cop2 $0980011
// 
// In:      [IR1,IR2,IR3]     Vector                              [1,3,12]
//          IR0               Interpolation value                 [1,3,12]
//          CODE              Code value from RGB.           CODE [0,8,0]
//          FC                Far color              RFC,GFC,BFC  [1,27,4]
// Out:     RGBn              RGB fifo               Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculations:
// [1,27,4]  MAC1=A1[IR1 + IR0*(Lm_B1[RFC - IR1])]                [1,27,16]
// [1,27,4]  MAC2=A2[IR2 + IR0*(Lm_B1[GFC - IR2])]                [1,27,16]
// [1,27,4]  MAC3=A3[IR3 + IR0*(Lm_B1[BFC - IR3])]                [1,27,16]
// [1,11,4]  IR1=Lm_B1[MAC1]                                      [1,27,4]
// [1,11,4]  IR2=Lm_B2[MAC2]                                      [1,27,4]
// [1,11,4]  IR3=Lm_B3[MAC3]                                      [1,27,4]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
void GTE::executeINTPL(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;

	// The interpolation is performed in fixed point [1,27,16] format.
	// Convert [1.11.4] IR color vector to .16
	s64 r = ((s64)(s16)ir1) << 12;
	s64 g = ((s64)(s16)ir2) << 12;
	s64 b = ((s64)(s16)ir3) << 12;

	interpolateColorAndFarColor(r, g, b, sf, lm);
}

//-------------------------------------------------------------------------------------------------

// Helper function for DPCS, DPCT and INTPL
//
// Interpolates an RGB colour and the far colour.
//
void GTE::interpolateColorAndFarColor(s64 r, s64 g, s64 b, bool sf, bool lm)
{
	// Convert Far Color components from [1,27,4] to [1,26,16]
	s64 rfc = (s64)(s32)m_rfc << 12;
	s64 gfc = (s64)(s32)m_gfc << 12;
	s64 bfc = (s64)(s32)m_bfc << 12;

	// Calculate the colour channel deltas in 44-bit and set any MAC123 overflow flags.
	m1 = mac123_s64_to_s44(1, rfc - r);
	m2 = mac123_s64_to_s44(2, gfc - g);
	m3 = mac123_s64_to_s44(3, bfc - b);

	if (sf)
	{
		// Convert colour channels from .16 to .4
		m1 >>= 12;
		m2 >>= 12;
		m3 >>= 12;
	}

	mac1 = (s32)m1;
	mac2 = (s32)m2;
	mac3 = (s32)m3;

	// The colour deltas also end up in the 16-bit color interpolation registers and set associated IR123 saturated flags.
	// lm is not respected at this stage; the interpolation factors are always clamped to [-8000,7fff]
	copyMAC123toIR123(/*lm*/false);

	s64 t = (s64)(s16)ir0; // interpolation factor

	// Multiply by the deltas by the saturated interpolation factors and set MAC123 overflow flags if required.
#if 0 // #TODO: Does the hardware perform a fused multiply and add? If so, can reduce into a single operation per channel e.g. m1 = mac123_s64_to_s44(1, m1 + t * (s64)ir1);
	// Separate multiply and add
	m1 = mac123_s64_to_s44(1, t * (s64)(s16)ir1);
	m2 = mac123_s64_to_s44(2, t * (s64)(s16)ir2);
	m3 = mac123_s64_to_s44(3, t * (s64)(s16)ir3);

	// Add the base value and set MAC123 overflow flags if required.
	m1 = mac123_s64_to_s44(1, r + m1);
	m2 = mac123_s64_to_s44(2, g + m2);
	m3 = mac123_s64_to_s44(3, b + m3);
#else
	// Fused multiply and add
	m1 = mac123_s64_to_s44(1, r + t * (s64)(s16)ir1);
	m2 = mac123_s64_to_s44(2, g + t * (s64)(s16)ir2);
	m3 = mac123_s64_to_s44(3, b + t * (s64)(s16)ir3);
#endif

	// Copy to the final values to the 32-bit MAC123 registers, respecting the sf bit.
	if (sf)
	{
		// Convert colour channels from .16 to .4
		mac1 = (s32)(m1 >> 12);
		mac2 = (s32)(m2 >> 12);
		mac3 = (s32)(m3 >> 12);
	}
	else
	{
		mac1 = (s32)m1;
		mac2 = (s32)m2;
		mac3 = (s32)m3;
	}

	copyMAC123toIR123(lm);

	pushMAC123toColorFIFO();
}

// DPCL (aka DCPL)
// 
// Depth cue light color
//
// First calculates a color from a light vector (normal vector of a plane
// multiplied with the light matrix and zero limited) and a provided RGB value.
// Then performs depth cueing by interpolating between the far color vector and
// the newfound color.
//
// Opcode:  cop2 $0680029
// In:      RGB               Primary color.         R,G,B,CODE   [0,8,0]
//          IR0               interpolation value.                [1,3,12]
//          [IR1,IR2,IR3]     Local color vector.                 [1,3,12]
//          CODE              Code value from RGB.           CODE [0,8,0]
//          FC                Far color.                          [1,27,4]
// Out:     RGBn              RGB fifo               Rn,Gn,Bn,CDn [0,8,0]
//          [IR1,IR2,IR3]     Color vector                        [1,11,4]
//          [MAC1,MAC2,MAC3]  Color vector                        [1,27,4]
// 
// Calculation:
// [1,27,4]  MAC1=A1[R*IR1 + IR0*(Lm_B1[RFC - R * IR1])]          [1,27,16]
// [1,27,4]  MAC2=A2[G*IR2 + IR0*(Lm_B1[GFC - G * IR2])]          [1,27,16]
// [1,27,4]  MAC3=A3[B*IR3 + IR0*(Lm_B1[BFC - B * IR3])]          [1,27,16]
// [1,11,4]  IR1=Lm_B1[MAC1]                                      [1,27,4]
// [1,11,4]  IR2=Lm_B2[MAC2]                                      [1,27,4]
// [1,11,4]  IR3=Lm_B3[MAC3]                                      [1,27,4]
// [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
// [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1]                             [1,27,4]
// [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2]                             [1,27,4]
// [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3]                             [1,27,4]
//
// From psx-spx:
//
//   [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
//   [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
//   [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
//   Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
//
void GTE::executeDPCL(u32 opcode)
{
	bool sf = (opcode >> 19) & 1;
	bool lm = (opcode >> 10) & 1;
	dpcl(sf, lm);
}

void GTE::dpcl(bool sf, bool lm)
{
	// Multiply RGB by IR vector to find colour.
	// [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
	s64 r = (u64)rgbc & 0xffu;
	s64 g = ((u64)rgbc >> 8) & 0xffu;
	s64 b = ((u64)rgbc >> 16) & 0xffu;

	m1 = mac123_s64_to_s44(1, (r * (s64)(s16)ir1) << 4);
	m2 = mac123_s64_to_s44(2, (g * (s64)(s16)ir2) << 4);
	m3 = mac123_s64_to_s44(3, (b * (s64)(s16)ir3) << 4);

	mac1 = (s32)m1;
	mac2 = (s32)m2;
	mac3 = (s32)m3;

	interpolateColorAndFarColor(mac1, mac2, mac3, sf, lm);
}

void GTE::multiplyMatrixVector(const Vector64& v, const Matrix33& rot, bool sf)
{
#if 0
	// This naive approach is not correct. The internal ALU registers are 44-bit and this needs to be accounted
	// for on every operation.
	m1 = ((m[0][0] * v.x) + (m[0][1] * v.y) + (m[0][2] * v.z);
	m2 = ((m[1][0] * v.x) + (m[1][1] * v.y) + (m[1][2] * v.z);
	m3 = ((m[2][0] * v.x) + (m[2][1] * v.y) + (m[2][2] * v.z);
#else
	m1 = mac123_s64_to_s44(1, ((s64)rot[0][0] * v.x));
	m1 = mac123_s64_to_s44(1, m1 + ((s64)rot[0][1] * v.y));
	m1 = mac123_s64_to_s44(1, m1 + ((s64)rot[0][2] * v.z));

	m2 = mac123_s64_to_s44(2, ((s64)rot[1][0] * v.x));
	m2 = mac123_s64_to_s44(2, m2 + ((s64)rot[1][1] * v.y));
	m2 = mac123_s64_to_s44(2, m2 + ((s64)rot[1][2] * v.z));

	m3 = mac123_s64_to_s44(3, ((s64)rot[2][0] * v.x));
	m3 = mac123_s64_to_s44(3, m3 + ((s64)rot[2][1] * v.y));
	m3 = mac123_s64_to_s44(3, m3 + ((s64)rot[2][2] * v.z));
#endif

	if (sf)
	{
		mac1 = (s32)(m1 >> 12);
		mac2 = (s32)(m2 >> 12);
		mac3 = (s32)(m3 >> 12);
	}
	else
	{
		mac1 = (s32)m1;
		mac2 = (s32)m2;
		mac3 = (s32)m3;
	}
}

//-------------------------------------------------------------------------------------------------

//
// Rotation matrix components are fixed point Q3.12 format (1 bit sign, 3 bits integer, 12 bits fraction)
//
void GTE::multiplyMatrixVectorAndBias(const Vector64& v, const Matrix33& rot, const Vector64& translation, bool sf)
{
#if 0
	// This naive approach is not correct. The internal ALU registers are 44-bit and this needs to be accounted
	// for on every operation.
	m1 = ((translation.x << 12) + (m[0][0] * v.x) + (m[0][1] * v.y) + (m[0][2] * v.z);
	m2 = ((translation.y << 12) + (m[1][0] * v.x) + (m[1][1] * v.y) + (m[1][2] * v.z);
	m3 = ((translation.z << 12) + (m[2][0] * v.x) + (m[2][1] * v.y) + (m[2][2] * v.z);
#else
	m1 = (translation.x << 12);
	m1 = mac123_s64_to_s44(1, m1 + ((s64)rot[0][0] * v.x));
	m1 = mac123_s64_to_s44(1, m1 + ((s64)rot[0][1] * v.y));
	m1 = mac123_s64_to_s44(1, m1 + ((s64)rot[0][2] * v.z));

	m2 = (translation.y << 12);
	m2 = mac123_s64_to_s44(2, m2 + ((s64)rot[1][0] * v.x));
	m2 = mac123_s64_to_s44(2, m2 + ((s64)rot[1][1] * v.y));
	m2 = mac123_s64_to_s44(2, m2 + ((s64)rot[1][2] * v.z));

	m3 = (translation.z << 12);
	m3 = mac123_s64_to_s44(3, m3 + ((s64)rot[2][0] * v.x));
	m3 = mac123_s64_to_s44(3, m3 + ((s64)rot[2][1] * v.y));
	m3 = mac123_s64_to_s44(3, m3 + ((s64)rot[2][2] * v.z));
#endif

	if (sf)
	{
		mac1 = (s32)(m1 >> 12);
		mac2 = (s32)(m2 >> 12);
		mac3 = (s32)(m3 >> 12);
	}
	else
	{
		mac1 = (s32)m1;
		mac2 = (s32)m2;
		mac3 = (s32)m3;
	}
}

// Copies MAC1, MAC2, MAC3 into the interpolation registers IR1, IR2, IR3, respecting the lm bit.
// 
void GTE::copyMAC123toIR123(bool lm)
{
	// MAC1 -> IR1
	if (mac1 > INT16_MAX)
	{
		ir1 = (u32)INT16_MAX;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else if (lm == 0 && mac1 < INT16_MIN)
	{
		ir1 = (u32)INT16_MIN;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else if (lm == 1 && mac1 < 0)
	{
		ir1 = 0;
		flag.IR1Saturated = 1; // bit 24
		flag.ErrorFlag = 1;
	}
	else
		ir1 = (s16)mac1;

	// MAC2 -> IR2
	if (mac2 > INT16_MAX)
	{
		ir2 = (u32)INT16_MAX;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else if (lm == 0 && mac2 < INT16_MIN)
	{
		ir2 = (u32)INT16_MIN;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else if (lm == 1 && mac2 < 0)
	{
		ir2 = 0;
		flag.IR2Saturated = 1; // bit 23
		flag.ErrorFlag = 1;
	}
	else
		ir2 = (s16)mac2;

	// MAC3 -> IR3
	if (mac3 > INT16_MAX)
	{
		ir3 = (u32)INT16_MAX;
		flag.IR3Saturated = 1; // bit 22
		// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
	}
	else if (lm == 0 && mac3 < INT16_MIN)
	{
		ir3 = (u32)INT16_MIN;
		flag.IR3Saturated = 1; // bit 22
		// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
	}
	else if (lm == 1 && mac3 < 0)
	{
		ir3 = 0;
		flag.IR3Saturated = 1; // bit 22
		// n.b. Error flag does not include bit 22! I don't know why. Hardware bug?
	}
	else
		ir3 = (s16)mac3;
}

//
// Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE]
//
void GTE::pushMAC123toColorFIFO()
{
	// Copy the final values into the color stack, 
	// Always divide by 16 (>>4) here, which is usually converting from .4 to .0 format.
	s32 r = mac1 >> 4;
	s32 g = mac2 >> 4;
	s32 b = mac3 >> 4;

	if (r < 0)
	{
		r = 0;
		flag.ColorFIFORSaturated = 1; // bit 21
	}
	else if (r > UINT8_MAX)
	{
		r = UINT8_MAX;
		flag.ColorFIFORSaturated = 1; // bit 21
	}

	if (g < 0)
	{
		g = 0;
		flag.ColorFIFOGSaturated = 1; // bit 20
	}
	else if (g > UINT8_MAX)
	{
		g = UINT8_MAX;
		flag.ColorFIFOGSaturated = 1; // bit 20
	}

	if (b < 0)
	{
		b = 0;
		flag.ColorFIFOBSaturated = 1; // bit 19
	}
	else if (b > UINT8_MAX)
	{
		b = UINT8_MAX;
		flag.ColorFIFOBSaturated = 1; // bit 19
	}

	// Advance the color FIFO
	rgb0 = rgb1;
	rgb1 = rgb2;
	rgb2 = (rgbc & 0xff000000) | ((u8)b << 16) | ((u8)g << 8) | (u8)r; // cgbr
}

//-------------------------------------------------------------------------------------------------

//
// Internal 32 bit register overflow.
//
// Sets flag bit 16 if MAC0 larger than 31 bits and positive
// Sets flag bit 15 if MAC0 larger than 31 bits and negative.
//
void GTE::checkMac0Overflow(s64 m0)
{
	if (m0 < INT32_MIN)
	{
		flag.MAC0NegativeOverflow = 1;
		flag.ErrorFlag = 1;
	}
	else if (m0 > INT32_MAX)
	{
		flag.MAC0PositiveOverflow = 1;
		flag.ErrorFlag = 1;
	}
}

//
// The internal registers are 44 bit: 1 bit sign + 43
// The emulation performs calculations in 64 bits, and needs to check for 44-bit overflow.
//
s64 GTE::mac123_s64_to_s44(unsigned int index, s64 val_64)
{
	HP_DEBUG_ASSERT(index >= 1 && index <= 3);

	// Set flags on 44-bit overflow
	static constexpr s64 kMac123Min = -((s64)1 << 43);
	static constexpr s64 kMac123Max = ((s64)1 << 43) - 1;
	if (val_64 < kMac123Min)
	{
		// bit 27 MAC1NegativeOverflow
		// bit 26 MAC2NegativeOverflow
		// bit 25 MAC3NegativeOverflow
		flag.val |= (1 << (28 - index));
		flag.ErrorFlag = 1;

		s64 val_44 = (val_64 << 20) >> 20; // sign-extend by shifting sign bit to MSb then back down
		HP_DEBUG_ASSERT(val_44 != val_64);
		return val_44;
	}
	else if (val_64 > kMac123Max)
	{
		// bit 30 MAC1PositiveOverflow
		// bit 29 MAC2PositiveOverflow
		// bit 28 MAC3PositiveOverflow
		flag.val |= (1 << (31 - index));
		flag.ErrorFlag = 1;

		s64 val_44 = (val_64 << 20) >> 20; // sign-extend by shifting sign bit to MSb then back down
		HP_DEBUG_ASSERT(val_44 != val_64);
		return val_44;
	}

	s64 val_44 = (val_64 << 20) >> 20; // sign-extend by shifting sign bit to MSb then back down
	HP_DEBUG_ASSERT(val_44 == val_64);
	return val_44;
}

// Update error flag
// n.b. The error flag bit 31 is Bit30..23, and 18..13 ORed together. Read only.
// This set of bits seems a bit strange: it doesn't capture bit 22 IR3 saturated.
// Maybe a hardware bug or something I don't understand.
void GTE::updateErrorFlag()
{
	flag.ErrorFlag = (flag.val & kErrorFlagMask) != 0 ? 1 : 0;
}
