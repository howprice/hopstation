#include "MDEC.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED
#include "core/ArrayHelpers.h"
#include "core/MathsHelpers.h"

#include <string.h> // memcpy

static constexpr bool kGenerateTables = false;
static constexpr bool kUseFastIDCT = true;

//--------------------------------------------------------------------------------------------------
// Inverse Discrete Cosine Transform (IDCT) constants and lookup tables

// cos((0..7)*90'/8) ;for [1..7]: multiplied by sqrt(2)
// https://psx-spx.consoledev.net/macroblockdecodermdec/#scalefactor07-cos07908-for-17-multiplied-by-sqrt2
static const float kScaleFactor[] =
{
	1.000000000f, 1.387039845f, 1.306562965f, 1.175875602f,
	1.000000000f, 0.785694958f, 0.541196100f, 0.275899379f
};
static_assert(COUNTOF_ARRAY(kScaleFactor) == 8);

// Block traversal order for zig-zag scan of 8x8 block.
static const u8 kZigZag[] =
{
	0,  1,  5,  6,  14, 15, 27, 28,
	2,  4,  7,  13, 16, 26, 29, 42,
	3,  8,  12, 17, 25, 30, 41, 43,
	9,  11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};
static_assert(COUNTOF_ARRAY(kZigZag) == 64);

static const u8 kZagZig[64] =
{
	0,	1,	8,	16,	9,	2,	3,	10,
	17,	24,	32,	25,	18,	11,	4,	5,
	12,	19,	26,	33,	40,	48,	41,	34,
	27,	20,	13,	6,	7,	14,	21,	28,
	35,	42,	49,	56,	57,	50,	43,	36,
	29,	22,	15,	23,	30,	37,	44,	51,
	58,	59,	52,	45,	38,	31,	39,	46,
	53,	60,	61,	54,	47,	55,	62,	63,
};
static_assert(COUNTOF_ARRAY(kZigZag) == 64);

static void generate_zagzig(u8 zagzig[64])
{
	for (unsigned int i = 0; i < 64; i++)
	{
		unsigned int zigZagIndex = kZigZag[i];
		zagzig[zigZagIndex] = (u8)i;
	}
}

// scalezag[0..63] (precalulated factors, for "fast_idct_core")
//  for y=0 to 7
//   for x=0 to 7
//    scalezag[zigzag[x+y*8]] = scalefactor[x] * scalefactor[y] / 8
//   next x
//  next y
// https://psx-spx.consoledev.net/macroblockdecodermdec/#scalezag063-precalulated-factors-for-fast_idct_core
// #TODO: Use modern C++ to generate the scalezag table at compile time
static void generate_scalezag(float scalezag[64])
{
	for (unsigned int y = 0; y < 8; y++)
	{
		for (unsigned int x = 0; x < 8; x++)
		{
			unsigned int zigZagIndex = kZigZag[x + y * 8];
			scalezag[zigZagIndex] = kScaleFactor[x] * kScaleFactor[y] / 8.0f;
		}
	}
}

static const float kScaleZag[64] =
{
	0.125000f,	0.173380f,	0.173380f,	0.163320f,	0.240485f,	0.163320f,	0.146984f,	0.226532f,
	0.226532f,	0.146984f,	0.125000f,	0.203873f,	0.213388f,	0.203873f,	0.125000f,	0.098212f,
	0.173380f,	0.192044f,	0.192044f,	0.173380f,	0.098212f,	0.067650f,	0.136224f,	0.163320f,
	0.172835f,	0.163320f,	0.136224f,	0.067650f,	0.034487f,	0.093833f,	0.128320f,	0.146984f,
	0.146984f,	0.128320f,	0.093833f,	0.034487f,	0.047835f,	0.088388f,	0.115485f,	0.125000f,
	0.115485f,	0.088388f,	0.047835f,	0.045060f,	0.079547f,	0.098212f,	0.098212f,	0.079547f,
	0.045060f,	0.040553f,	0.067650f,	0.077165f,	0.067650f,	0.040553f,	0.034487f,	0.053152f,
	0.053152f,	0.034487f,	0.027097f,	0.036612f,	0.027097f,	0.018664f,	0.018664f,	0.009515f,
};
static_assert(COUNTOF_ARRAY(kScaleZag) == 64);

// One-off utility function to generate kZagZig and  kScaleZag table code.
[[maybe_unused]]
static void generateTables()
{
	u8 zagzig[64]{};
	generate_zagzig(zagzig);
	// print the table in a format suitable for copy-pasting into C++ source code
	printf("static const u8 kZagZig[64] =\n{\n");
	for (unsigned int i = 0; i < COUNTOF_ARRAY(zagzig); i++)
	{
		printf("    %u,\n", zagzig[i]);
	}
	printf("};\n");

	float scalezag[64]{};
	generate_scalezag(scalezag);
	// print the table in a format suitable for copy-pasting into C++ source code
	printf("static const float kScaleZag[64] =\n{\n");
	for (unsigned int i = 0; i < COUNTOF_ARRAY(scalezag); i++)
	{
		printf("    %ff,\n", scalezag[i]);
	}
	printf("};\n");
}

//--------------------------------------------------------------------------------------------------

// Forward declarations
static void fast_idct_core(s16 blk[64]);
static void yuv_to_rgb(
	unsigned int x0, unsigned int y0,
	const s16 Yblk[64], const s16 CbBlk[64], const s16 CrBlk[64],
	u8* dstBlk,
	bool isUnsigned, bool output15bpp, bool setBit15);

//--------------------------------------------------------------------------------------------------

// MDEC processing is performed in 16 x 240 pixel strips.
// 16 * 240 * 3 bytes per pixel = 3,840 = 2D00h bytes per strip
// This is seen in titles sucha as Megaman X4 which uses DMA1 MDEC to RAM to transfer B40h words = 2D00h bytes at once.
// 
// However, in a real machine, DMA and MDEC decompression would be performed cycle by cycle, but in this emulation
// DMA and decompression are performed in one go, so we need a buffer large enough for the whole 320x240 3 bytes per pixel screen
//static const unsigned int kDecodedDataBufferCapacityBytes = 0x2D00;
static constexpr unsigned int kDecodedDataBufferCapacityBytes = 2 * 320 * 240 * 3;

//--------------------------------------------------------------------------------------------------

MDEC::MDEC()
{
	m_outputBuffer = new u8[kDecodedDataBufferCapacityBytes]; // Buffer for one macroblock's worth of decoded data (16 bytes per block, 6 blocks per macroblock)
	memset(m_outputBuffer, 0, kDecodedDataBufferCapacityBytes);

	if constexpr (kGenerateTables)
		generateTables();
}

MDEC::~MDEC()
{
	delete[] m_outputBuffer;
}


void MDEC::Reset()
{
	m_state = State::Idle;
	m_compressedDataSizeWords = 0;
	m_expectColorTable = false; // command 2 state
	m_parameterWordsReceived = 0;

	// Set status to 80040000h = Bit 31 Data-Out Fifo Empty | 18-16 Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always 4=Y)
	// This is the value set when MDEC1 is written with Reset bit set.
	m_status.val = 0x80040000; // #TODO: Is this the correct MDEC initial value?
	m_status.ParameterWordsRemainingMinus1 = 0xffff;

	m_dmaInEnabled = false;
	m_dmaOutEnabled = false;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_luminanceQuantTable); i++)
	{
		m_luminanceQuantTable[i] = 0;
	}

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_colorQuantTable); i++)
	{
		m_colorQuantTable[i] = 0;
	}

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_scaleTable); i++)
	{
		m_scaleTable[i] = 0;
	}

	memset(m_outputBuffer, 0, kDecodedDataBufferCapacityBytes);
	m_outputWriteIndex = 0;
	m_outputReadIndex = 0;
}

// 1F801820h - MDEC0 - MDEC Command/Parameter Register (W)
// Used to send command word, followed by parameter words to the MDEC.
// Usually only the command word is written to this register, and the parameter words are transferred via DMA0.
void MDEC::WriteMDEC0(u32 val, u32 pc)
{
	if (g_logMDEC)
		LOG_INFO("[MDEC] Write MDEC0 Command/Parameter Register value %08X PC: %08X\n", val, pc);

	switch (m_state)
	{
		case State::Idle:
		{
			// Copy bits 28:25 to stat bits 26:23 (common to all commands)
			m_status.DataOutputDepth = (DataOutDepth)((val >> 27) & 3); // bits 28:27 -> 26:25
			m_status.DataOutputSigned = (val >> 26) & 1; // bit 26 -> 24
			m_status.DataOutputBit15 = (val >> 25) & 1; // bit 25 -> 23

			// Decode command from bits 31:29
			// Only commands 0..3 are valid. Commands 4..7 are invalid/undefined and the same as command 0
			u32 command = (val >> 29) & 7;

			switch (command)
			{
				case 0:
					command0(val);
					break;
				case 1:
					command1(val);
					break;
				case 2:
					command2(val); // Set Quant Table(s)
					break;
				case 3:
					command3(val); // Set Scale Table
					break;
				default: // 4..7 are invalid/undefined and the same as command 0
					command0(val);
			}
			
			break;
		}

		case State::ReceivingLuminanceQuantTable:
		{
			// 1 word (4 bytes) are received at a time (usually by DMA0)
			HP_ASSERT(m_parameterWordsReceived < 16, "Received too many parameter words for luminance quant table");
			unsigned int index = m_parameterWordsReceived * 4;

			// #TODO: Is this the correct order?
			m_luminanceQuantTable[index++] = val & 0xff;
			m_luminanceQuantTable[index++] = (val >> 8) & 0xff;
			m_luminanceQuantTable[index++] = (val >> 16) & 0xff;
			m_luminanceQuantTable[index++] = (val >> 24) & 0xff;

			m_parameterWordsReceived++;
			if (m_parameterWordsReceived == 16)
			{
				if (m_expectColorTable)
					m_state = State::ReceivingColorQuantTable;
				else
					m_state = State::Idle;
			}
			break;
		}

		case State::ReceivingColorQuantTable:
		{
			// 32-bits (4 bytes) are received at a time (usually by DMA0)
			HP_ASSERT(m_parameterWordsReceived >= 16, "Expect to have already received the luminance quant table");
			unsigned int colorTableWordsReceived = m_parameterWordsReceived - 16;
			HP_ASSERT(colorTableWordsReceived < 64 / 4, "Received too many parameter words for color quant table");
			unsigned int index = colorTableWordsReceived * 4;

			// #TODO: Is this the correct order?
			m_colorQuantTable[index++] = val & 0xff;
			m_colorQuantTable[index++] = (val >> 8) & 0xff;
			m_colorQuantTable[index++] = (val >> 16) & 0xff;
			m_colorQuantTable[index++] = (val >> 24) & 0xff;

			m_parameterWordsReceived++;
			if (m_parameterWordsReceived == 2 * 16)
			{
				m_state = State::Idle;
			}
			break;
		}

		case State::ReceivingScaleTable:
			HP_FATAL_ERROR("Not implemented"); // Expect this to be received in a single DMA burst, so we can copy it in one go in ReceiveDMA.
			break;

		case State::Decompressing:
			HP_FATAL_ERROR("Not implemented"); // Expect compressed data to be received via DMA block copies rather than word by word.
			break;
	}
}

//
//   Bit
//   31    Reset MDEC (0=No change, 1=Abort any command, and set status=80040000h)
//   30    Enable Data-In Request  (0=Disable, 1=Enable DMA0 and Status.bit28)
//   29    Enable Data-Out Request (0=Disable, 1=Enable DMA1 and Status.bit27)
//   28-0  Unknown/Not used - usually zero
// https://psx-spx.consoledev.net/macroblockdecodermdec/#1f801824h-mdec1-mdec-controlreset-register-w
//
void MDEC::WriteMDEC1(u32 val)
{
	static constexpr u32 kResetFlag = 1u << 31;
	static constexpr u32 kDataInRequestFlag = 1u << 30;
	static constexpr u32 kDataOutRequestFlag = 1u << 29;

	if (g_logMDEC)
		LOG_INFO("[MDEC] Write MDEC1 Control/Reset Register value %08X%s%s%s\n",
			val,
			(val & kResetFlag) ? " Reset" : "",
			(val & kDataInRequestFlag) ? " Data-In Request" : "",
			(val & kDataOutRequestFlag) ? " Data-Out Request" : "");

	if (val & kResetFlag) // Reset MDEC
	{
		HP_ASSERT(m_state == State::Idle, "Abort existing command not implemented"); // #TODO: Abort any existing command
		
		// Set status to 80040000h = Bit 31 Data-Out Fifo Empty | 18-16 Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always 4=Y)
		m_status.val = 0x80040000;
		m_status.ParameterWordsRemainingMinus1 = 0xffff;

		// Reset output FIFO
		m_outputWriteIndex = 0;
		m_outputReadIndex = 0;
	}

	m_dmaInEnabled = val & kDataInRequestFlag; // bit 30
	m_dmaOutEnabled = val & kDataOutRequestFlag; // bit 29

	bool inputFifoHasSpace = true; // assume always has space
	m_status.DataInRequest = m_dmaInEnabled && inputFifoHasSpace; // status bit 28

	m_status.DataOutRequest = m_dmaOutEnabled && !isOutputFIFOEmpty();// status bit 27
}

//
// Read Data/Response Register
//
// Implementation note: DMA1 - MDECout (MDEC to RAM) currently uses ReadDataBlock() to read a block in a single call.
//
u32 MDEC::ReadMDEC0()
{
	HP_FATAL_ERROR("Not tested");

	HP_DEBUG_ASSERT(m_outputWriteIndex > m_outputReadIndex, "Output buffer underflow");
	if (m_outputReadIndex + 4 > m_outputWriteIndex)
	{
		HP_FATAL_ERROR("Output buffer empty. Path not tested");
		// Output buffer is empty, return 0. Think should return garbage.
		return 0;
	}

	u32 val = *(u32*)(m_outputBuffer + m_outputReadIndex);
	m_outputReadIndex += 4;

	if (m_outputReadIndex == m_outputWriteIndex)
	{
		m_status.DataOutFifoEmpty = true;
		m_status.DataOutRequest = false; // status bit 27. No data to output, so false even if output DMA enabled.

		// Reset FIFO
		m_outputReadIndex = 0;
		m_outputWriteIndex = 0;
	}
	return val;
}

u32 MDEC::ReadMDEC1() const
{
	if (g_logMDEC)
		LOG_INFO("[MDEC] Read MDEC1 MDEC Status Register val %08X\n", m_status.val);
	return m_status.val;
}

void MDEC::WriteDataBlock(const u8* data, unsigned int numWords)
{
	switch (m_state)
	{
		case State::Idle:
			HP_FATAL_ERROR("Not implemented");
			break;

		case State::ReceivingLuminanceQuantTable:
		{
			// We can receive the entire table in one go if enough data is available, which is usually the case with DMA0.
			HP_ASSERT(m_parameterWordsReceived == 0);
			[[maybe_unused]] unsigned int numBytes = numWords * 4; // validation only
			HP_ASSERT(numBytes >= COUNTOF_ARRAY(m_luminanceQuantTable));
			memcpy(m_luminanceQuantTable, data, COUNTOF_ARRAY(m_luminanceQuantTable));
			numBytes -= COUNTOF_ARRAY(m_luminanceQuantTable);
			m_parameterWordsReceived += COUNTOF_ARRAY(m_luminanceQuantTable) / 4;
			data += COUNTOF_ARRAY(m_luminanceQuantTable);

			if (!m_expectColorTable)
			{
				m_state = State::Idle;
				break;
			}
			
			HP_ASSERT(numBytes >= COUNTOF_ARRAY(m_colorQuantTable));
			memcpy(m_colorQuantTable, data, COUNTOF_ARRAY(m_colorQuantTable));
			numBytes -= COUNTOF_ARRAY(m_colorQuantTable);
			HP_ASSERT(numBytes == 0, "Could there be more commands in the stream?");
			m_parameterWordsReceived += COUNTOF_ARRAY(m_colorQuantTable) / 4;
			m_expectColorTable = false;
			m_state = State::Idle;
#if 0
			// This seems like the correct behaviour, but Duckstation does not do this.
			m_status.ParameterWordsRemainingMinus1 = 0xffff; // done
#else
			m_status.ParameterWordsRemainingMinus1 = 0x000f; // Duckstation does this!
#endif
			break;
		}

		case State::ReceivingColorQuantTable:
			HP_FATAL_ERROR("Not implemented");
			break;

		case State::ReceivingScaleTable:
		{
			// We can receive the entire table in one go if enough data is available, which is usually the case with DMA0.
			HP_ASSERT(m_parameterWordsReceived == 0);
			unsigned int numBytes = numWords * 4;
			HP_ASSERT(numBytes == sizeof(m_scaleTable), "Current implementation expects whole scale table in one go.");
			memcpy(m_scaleTable, data, numBytes);
			m_parameterWordsReceived += numBytes / 4;
			m_state = State::Idle;
#if 0
			// This seems like the correct behaviour, but Duckstation does not do this.
			m_status.ParameterWordsRemainingMinus1 = 0xffff; // done
#else
			m_status.ParameterWordsRemainingMinus1 = 0x000f; // Duckstation does this!
#endif
			break;
		}

		case State::Decompressing:
		{
			unsigned int numBytes = numWords * 4;
			HP_ASSERT(numWords == m_compressedDataSizeWords, "Can the amount configured to decompress differ from the amount DMAed in?");
			decode(data, numBytes);
			m_state = State::Idle;
			break;
		}
	}
}

void MDEC::ReadDataBlock(u8* dst, unsigned int numBytes)
{
	HP_DEBUG_ASSERT(m_outputWriteIndex >= m_outputReadIndex, "Output buffer underflow");
	HP_ASSERT(m_outputWriteIndex - m_outputReadIndex >= numBytes); // #TODO: Might have to make this >= to support reading a subset of the buffer.
	memcpy(dst, m_outputBuffer + m_outputReadIndex, numBytes);
	m_outputReadIndex += numBytes; // Clear buffer after reading

	if (m_outputReadIndex == m_outputWriteIndex)
	{
		m_status.DataOutFifoEmpty = true;
		m_status.DataOutRequest = false; // status bit 27. No data to output, so false even if output DMA enabled.

		// Reset FIFO
		m_outputReadIndex = 0;
		m_outputWriteIndex = 0;
	}
}

// MDEC(0) - No function
// 
// This command has no function.
// 
// Command bits 25-28 are reflected to Status bits 23-26 as usual.
// Command bits 0-15 are reflected to Status bits 0-15, similar as the "number of parameter words" for MDEC(1),
// but without the "minus 1" effect, and without actually expecting any parameters.
// 
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec0-no-function
//
void MDEC::command0(u32 val)
{
	m_status.ParameterWordsRemainingMinus1 = (u16)val - 1; // Contrary to the docs, I think this should actually be -1 i.e. 0xffff for zero

	m_parameterWordsReceived = 0;
}

// MDEC(1) - Decode Macroblock(s)
//
// Command word format:
//   31-29 Command (1=decode_macroblock)
//   28-27 Data Output Depth  (0=4bit, 1=8bit, 2=24bit, 3=15bit)      ;STAT.26-25
//   26    Data Output Signed (0=Unsigned, 1=Signed)                  ;STAT.24
//   25    Data Output Bit15  (0=Clear, 1=Set) (for 15bit depth only) ;STAT.23
//   24-16 Not used (should be zero)
//   15-0  Number of Parameter Words (size of compressed data)
//
// This command is followed by one or more Macroblock parameters.
// Usually, all macroblocks for the whole image are sent at once.
//
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec1-decode-macroblocks
void MDEC::command1(u32 val)
{
	m_compressedDataSizeWords = val & 0xffff;

	if (g_logMDEC)
		LOG_INFO("[MDEC] MDEC(1) - Decode Macroblock(s) %04x words (hex)\n", m_compressedDataSizeWords);

	m_state = State::Decompressing;
	m_parameterWordsReceived = 0;

	m_status.ParameterWordsRemainingMinus1 = m_compressedDataSizeWords - 1;
}

// MDEC(2) - Set Quant Table(s)
//
// Command word format:
//  31-29 Command (2=set_iqtab)
//  28-1  Not used (should be zero)  ;Bit25-28 are copied to STAT.23-26 though
//  0     Color   (0=Luminance only, 1=Luminance and Color)
//
// The command word is followed by 64 unsigned parameter bytes for the Luminance Quant Table (used for Y1..Y4),
// and if Command.Bit0 was set, by another 64 unsigned parameter bytes for the Color Quant Table (used for Cb and Cr).
// 
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec2-set-quant-tables
//
void MDEC::command2(u32 val)
{
	m_expectColorTable = (val & 1) != 0; // bit 0

	m_state = State::ReceivingLuminanceQuantTable;
	m_parameterWordsReceived = 0;

	if (g_logMDEC)
		LOG_INFO("[MDEC] MDEC(2) - Set Quant Table (%s)\n", m_expectColorTable ? "luminance then color" : "luminance only");

	m_status.ParameterWordsRemainingMinus1 = m_expectColorTable ? 32 - 1 : 16 - 1;
}

// MDEC(3) - Set Scale Table
//
// Command word format:
//   31-29 Command (3=set_scale)
//   28-0  Not used (should be zero)  ;Bit25-28 are copied to STAT.23-26 though
//
// The command is followed by 64 signed halfwords with 14 bit fractional part.
// The values should be usually/always the same values, based on the standard JPEG constants,
// although, MDEC(3) allows to use other values than that constants.
//
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec3-set-scale-table
//
void MDEC::command3(u32 /*val*/)
{
	m_state = State::ReceivingScaleTable;
	m_parameterWordsReceived = 0;

	if (g_logMDEC)
		LOG_INFO("[MDEC] MDEC(3) - Set Scale Table\n");

	m_status.ParameterWordsRemainingMinus1 = 32 - 1;
}

//
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec-decompression
//
void MDEC::decode(const u8* src, unsigned int srcSizeBytes)
{
	const u16* srcHalfwords = (const u16*)src;
	HP_ASSERT((srcSizeBytes & 1) == 0, "Compressed data size should be a whole number of halfwords");
	unsigned int srcHalfwordsSizeBytes = srcSizeBytes / 2;
	switch (m_status.DataOutputDepth)
	{
		case DataOutDepth::k4Bit:
			decodeMonochromeMacroblocks(srcHalfwords, srcHalfwordsSizeBytes);
			break;
		case DataOutDepth::k8Bit:
			decodeMonochromeMacroblocks(srcHalfwords, srcHalfwordsSizeBytes);
			break;
		case DataOutDepth::k24Bit:
			decodeColoredMacroblocks(srcHalfwords, srcHalfwordsSizeBytes);
			break;
		case DataOutDepth::k15Bit:
			decodeColoredMacroblocks(srcHalfwords, srcHalfwordsSizeBytes);
			break;
	}
}

// Theory: https://psx-spx.consoledev.net/macroblockdecodermdec/#colored-macroblocks-16x16-pixels-in-15bpp-or-24bpp-depth-mode
// Pseudo-code:https://psx-spx.consoledev.net/macroblockdecodermdec/#decode_colored_macroblock-mdec1-command-at-15bpp-or-24bpp-depth
//
// Colored macroblock decoding notes
// =================================
// 
// Decode 2500h words = 9400h bytes
// Read B40h words = 2D00h bytes
// In 24-bit format, 3 bytes per pixel, so 2D00h/3 = F00h = 3,840 pixels
// Each block is 8x8. 3,840/8 = 480
// Screen height is 240, so each iteration is two 8 pixel columns of pixels
// The chroma blocks are half resolution: each 8x8 block covers 16x16 pixels.
// So processing is performed in 16x240 pixel strips.
//
void MDEC::decodeColoredMacroblocks(const u16* src, unsigned int srcSizeHalfwords)
{
	const bool unsingedSignedOutput = !m_status.DataOutputSigned;
	const bool ouput15bpp = m_status.DataOutputDepth == DataOutDepth::k15Bit;
	const bool setBit15 = m_status.DataOutputBit15;

	HP_ASSERT(m_status.DataOutputDepth == DataOutDepth::k15Bit || m_status.DataOutputDepth == DataOutDepth::k24Bit, "Invalid data output depth for colored macroblock");
	constexpr unsigned int kPixelsPerMacroblock = 16 * 16; // 16x16 pixel macroblocks
	constexpr unsigned int kBlockSizeBytes15bpp = kPixelsPerMacroblock * 2; // 2 bytes per pixel for 15bpp A1B5G5R5
	constexpr unsigned int kBlockSizeBytes24bpp = kPixelsPerMacroblock * 3; // 3 bytes per pixel for 24bpp R8G8B8 tightly packed
	const unsigned int blockSizeBytes = m_status.DataOutputDepth == DataOutDepth::k15Bit ? kBlockSizeBytes15bpp : kBlockSizeBytes24bpp;

	s16 CrBlk[64];
	s16 CbBlk[64];
	s16 Yblk[64];
	HP_ASSERT(m_outputWriteIndex == 0); // Expect buffer to be empty before decoding a new macroblock
	u8* output = m_outputBuffer;

	[[maybe_unused]] unsigned int blockCount = 0; // For logging only. Expect 240/16 = 15 blocks

	while (srcSizeHalfwords > 0)
	{
		HP_ASSERT(m_outputWriteIndex + blockSizeBytes <= kDecodedDataBufferCapacityBytes, "Decoded data buffer overflow"); // Expect each macroblock to fit in the buffer

		// Decode the component blocks for 16x16 pixels
		decodeBlock(CrBlk, src, srcSizeHalfwords, m_colorQuantTable); // Cr (low resolution)
		decodeBlock(CbBlk, src, srcSizeHalfwords, m_colorQuantTable); // Cb (low resolution)

		decodeBlock(Yblk, src, srcSizeHalfwords, m_luminanceQuantTable); // Y1 (and upper-left Cr,Cb)
		yuv_to_rgb(0, 0, Yblk, CbBlk, CrBlk, output, unsingedSignedOutput, ouput15bpp, setBit15);
		decodeBlock(Yblk, src, srcSizeHalfwords, m_luminanceQuantTable); // Y2 (and upper-right Cr,Cb)
		yuv_to_rgb(8, 0, Yblk, CbBlk, CrBlk, output, unsingedSignedOutput, ouput15bpp, setBit15);
		decodeBlock(Yblk, src, srcSizeHalfwords, m_luminanceQuantTable); // Y3 (and lower-left Cr,Cb)
		yuv_to_rgb(0, 8, Yblk, CbBlk, CrBlk, output, unsingedSignedOutput, ouput15bpp, setBit15);
		decodeBlock(Yblk, src, srcSizeHalfwords, m_luminanceQuantTable); // Y4 (and lower-right Cr,Cb)
		yuv_to_rgb(8, 8, Yblk, CbBlk, CrBlk, output, unsingedSignedOutput, ouput15bpp, setBit15);

		output += blockSizeBytes; // Move output pointer to next block position for next macroblock (if any)
		m_outputWriteIndex += blockSizeBytes; // Track size of decoded data in buffer
		blockCount++;
	}

	HP_ASSERT(srcSizeHalfwords == 0);

	HP_ASSERT(m_outputWriteIndex >= m_outputReadIndex);
	m_status.DataOutFifoEmpty = m_outputWriteIndex == m_outputReadIndex;
	m_status.DataOutRequest = m_dmaOutEnabled && !m_status.DataOutFifoEmpty; // status bit 27. No data to output, so false even if output DMA enabled.
}

void MDEC::decodeBlock(s16 blk[64], const u16* & src, unsigned int & srcSizeHalfwords, const u8 q[64])
{
	runLengthDecodeBlock(src, srcSizeHalfwords, blk, q, kUseFastIDCT);

	if constexpr (kUseFastIDCT)
		fast_idct_core(blk);
	else
		real_idct_core(blk);
}

// sign-extend 10-bit value.
static inline s16 signed10bit(s16 val)
{
	// n.b. The cast before shifting back down is crucial because val << 6 is of type int due to integer promotion rules,
	// so we need to cast it back to s16 before shifting back down to get the correct sign extension.
	return (s16)(val << 6) >> 6;  
}

//
// Within each block the DCT information and RLE compressed data is stored:
//  DCT               ;1 halfword
//  RLE,RLE,RLE,etc.  ;0..63 halfwords
//  EOB               ;1 halfword
// 
// DCT data has the quantization factor and the Direct Current (DC) reference.
//  15-10 Q    Quantization factor (6 bits, unsigned)
//  9-0   DC   Direct Current reference (10 bits, signed)
//
// RLE (Run length data, for 2nd through 64th value)
//   15-10 LEN  Number of zero AC values to be inserted (6 bits, unsigned)
//   9-0   AC   Relative AC value (10 bits, signed)
// Example: AC values "000h,000h,123h" would be compressed as "(2 shl 10)+123h".
//
// Decompression algorithm (for one block):
// 
//   for i=0 to 63, blk[i]=0, next i   ;initially zerofill all entries (for skip)
//  @@skip:
//   n=[src], src=src+2, k=0           ;get first entry, init dest addr k=0
//   if n=FE00h then @@skip            ;ignore padding (FE00h as first halfword)
//   q_scale=(n SHR 10) AND 3Fh        ;contains scale value (not "skip" value)
//   val=signed10bit(n AND 3FFh)*qt[k] ;calc first value (without q_scale/8) (?)
//  @@lop:
//   if q_scale=0 then val=signed10bit(n AND 3FFh)*2   ;special mode without qt[k]
//   val=minmax(val,-400h,+3FFh)            ;saturate to signed 11bit range
//   val=val*scalezag[i]                    ;<-- for "fast_idct_core" only
//   if q_scale>0 then blk[zagzig[k]]=val   ;store entry (normal case)
//   if q_scale=0 then blk[k]=val           ;store entry (special, no zigzag)
//   n=[src], src=src+2                     ;get next entry (or FE00h end code)
//   k=k+((n SHR 10) AND 3Fh)+1             ;skip zerofilled entries
//   val=(signed10bit(n AND 3FFh)*qt[k]*q_scale+4)/8  ;calc value for next entry
//   if k<=63 then jump @@lop          ;should end with n=FE00h (that sets k>63)
//   idct_core(blk)
//   return (with "src" address advanced)
// 
// https://psx-spx.consoledev.net/macroblockdecodermdec/#rl_decode_blockblksrcqt
//
void MDEC::runLengthDecodeBlock(const u16* & src, unsigned int & srcSizeHalfwords, s16 blk[64], const u8 qt[64], bool fastIDCT)
{
	// initially zerofill all entries to support skipping zero entries
	for (unsigned int i = 0; i < 64; i++)
	{
		blk[i] = 0;
	}

	HP_ASSERT(srcSizeHalfwords > 0);
	u16 n = *src++;
	srcSizeHalfwords--;
	while (n == 0xfe00) // ignore padding (FE00h as first halfword)
	{
		HP_ASSERT(srcSizeHalfwords > 0);
		n = *src++;
		srcSizeHalfwords--;
	}

	unsigned int k = 0;
	u16 q_scale = (n >> 10) & 0x3f; // bits 15:10 Quantization factor (6 bits, unsigned)
	s32 val = (s32)signed10bit((s16)(n & 0x3ff)) * qt[k];

	while (k < 64)
	{
		if (q_scale == 0)
			val = signed10bit((s16)(n & 0x3ff)) * 2;   // special mode without qt[k]

		val = Clamp(val, -0x400, 0x3ff); // saturate to signed 11-bit range

		// I think this step is only required for the "fast_idct_core" version
		if (fastIDCT)
			val = (u16)(val * kScaleZag[k]);

		if (q_scale > 0)
			blk[kZagZig[k]] = (s16)val; // store entry (normal case) n.b. zagzig is inverse of zigzag
		else if (q_scale == 0)
			blk[k] = (s16)val; // store entry(special, no zigzag)

		// get next entry (or FE00h end code)
		HP_ASSERT(srcSizeHalfwords > 0);
		n = *src++;
		srcSizeHalfwords--;

		// skip zerofilled entries
		// should end with n=FE00h which will sets k>63
		k += ((n >> 10) & 0x3F) + 1;

		val = (signed10bit(n & 0x3FF) * qt[k] * q_scale + 4) / 8; // calc value for next entry
	}

	// Skip any "dummy halfword" padding FE00 bytes, which can be present at end of final block in DMA block.
	// https://psx-spx.consoledev.net/macroblockdecodermdec/#dummy-halfwords
	while (srcSizeHalfwords > 0)
	{
		n = *src;
		if (n != 0xfe00)
			break; // Expect any padding to be at the end of the block, so we can stop processing once we see a non-padding value
		src++;
		srcSizeHalfwords--;
	}
}

// Inverse Discrete Cosine Transform (IDCT)
//
// Low level code with 1024 multiplications, using the scaletable from the MDEC(3) command.
// Computes dst=src*scaletable (using normal matrix maths, but with "src" being diagonally mirrored,
// i.e. the matrices are processed column by column, instead of row by column, repeated with src/dst exchanged.
//
// https://psx-spx.consoledev.net/macroblockdecodermdec/#real_idct_coreblk-low-level-idct_core-version
//
void MDEC::real_idct_core(s16 blk[64]) const
{
	s16* src = (s16*)blk;
	s16 temp_buffer[64];
	s16* dst = temp_buffer;
	for (unsigned int pass = 0; pass < 2; pass++)
	{
		for (unsigned int x = 0; x < 8; x++)
		{
			for (unsigned int y = 0; y < 8; y++)
			{
				int sum = 0; // #TODO: Is this type suitable?
				for (unsigned int z = 0; z < 8; z++)
				{
					sum = sum + (int)src[y + z * 8] * ((int)m_scaleTable[x + z * 8] / 8); // #TODO: Are (int) casts required here?
				}
				dst[x + y * 8] = (s16)((sum + 0x0fff) / 0x2000);
			}
		}
		Swap(src, dst);
	}
}

bool MDEC::isOutputFIFOEmpty() const
{
	return m_outputWriteIndex == m_outputReadIndex;
}

//
// Inverse Discrete Cosine Transform (IDCT)
// 
// fast_idct_core(blk) fast "idct_core" version  of the decompression algorithm
//
//  src=blk, dst=temp_buffer
//  for pass=0 to 1
//    for i=0 to 7
//      if src[(1..7)*8+i]=0 then      ;when src[(1..7)*8+i] are all zero:
//        dst[i*8+(0..7)]=src[0*8+i]   ;quick fill by src[0*8+i]
//      else
//        z10=src[0*8+i]+src[4*8+i], z11=src[0*8+i]-src[4*8+i]
//        z13=src[2*8+i]+src[6*8+i], z12=src[2*8+i]-src[6*8+i]
//        z12=(1.414213562*z12)-z13          ;=sqrt(2)
//        tmp0=z10+z13, tmp3=z10-z13, tmp1=z11+z12, tmp2=z11-z12
//        z13=src[3*8+i]+src[5*8+i], z10=src[3*8+i]-src[5*8+i]
//        z11=src[1*8+i]+src[7*8+i], z12=src[1*8+i]-src[7*8+i]
//        z5  =(1.847759065*(z12-z10))       ;=sqrt(2)*scalefactor[2]
//        tmp7=z11+z13
//        tmp6=(2.613125930*(z10))+z5-tmp7   ;=scalefactor[2]*2
//        tmp5=(1.414213562*(z11-z13))-tmp6  ;=sqrt(2)
//        tmp4=(1.082392200*(z12))-z5+tmp5   ;=sqrt(2)/scalefactor[2]
//        dst[i*8+0]=tmp0+tmp7, dst[i*8+7]=tmp0-tmp7
//        dst[i*8+1]=tmp1+tmp6, dst[i*8+6]=tmp1-tmp6
//        dst[i*8+2]=tmp2+tmp5, dst[i*8+5]=tmp2-tmp5
//        dst[i*8+4]=tmp3+tmp4, dst[i*8+3]=tmp3-tmp4
//      endif
//    next i
//    swap(src,dst)
//  next pass
// 
// https://psx-spx.consoledev.net/macroblockdecodermdec/#fast_idct_coreblk-fast-idct_core-version
//
static void fast_idct_core(s16 blk[64])
{
	static s16 temp_buffer[64]; // 8x8 block of coefficients or pixel values

	s16* src = blk;
	s16* dst = (s16*)temp_buffer;

	for (unsigned int pass = 0; pass < 2; pass++)
	{
		for (unsigned int i = 0; i < 8; i++)
		{
			// when src[(1..7)*8+i] are all zero
			bool zero = true;
			for (unsigned int j = 1; j < 8; j++)
			{
				if (src[j * 8 + i] != 0)
				{
					zero = false;
					break;
				}
			}

			if (zero)
			{
				// dst[i*8+(0..7)]=src[0*8+i]   ;quick fill by src[0*8+i]
				for (unsigned int j = 0; j < 8; j++)
				{
					dst[i * 8 + j] = src[i];
				}
			}
			else
			{
				float z10 = (float)src[0 * 8 + i] + (float)src[4 * 8 + i];
				float z11 = (float)src[0 * 8 + i] - (float)src[4 * 8 + i];

				float z13 = (float)src[2 * 8 + i] + (float)src[6 * 8 + i];
				float z12 = (float)src[2 * 8 + i] - (float)src[6 * 8 + i];

				z12 = (1.414213562f * z12) - z13; // ; = sqrt(2)

				float tmp0 = z10 + z13, tmp3 = z10 - z13, tmp1 = z11 + z12, tmp2 = z11 - z12;

				z13 = (float)src[3 * 8 + i] + (float)src[5 * 8 + i];
				z10 = (float)src[3 * 8 + i] - (float)src[5 * 8 + i];
				z11 = (float)src[1 * 8 + i] + (float)src[7 * 8 + i];
				z12 = (float)src[1 * 8 + i] - (float)src[7 * 8 + i];
				float z5 = (1.847759065f * (z12 - z10)); // sqrt(2) * scalefactor[2]
				float tmp7 = z11 + z13;
				float tmp6 = (2.613125930f * (z10)) + z5 - tmp7; // = scalefactor[2] * 2
				float tmp5 = (1.414213562f * (z11 - z13)) - tmp6; // = sqrt(2)
				float tmp4 = (1.082392200f * (z12)) - z5 + tmp5; // = sqrt(2) / scalefactor[2]
				dst[i * 8 + 0] = (s16)(tmp0 + tmp7), dst[i * 8 + 7] = (s16)(tmp0 - tmp7);
				dst[i * 8 + 1] = (s16)(tmp1 + tmp6), dst[i * 8 + 6] = (s16)(tmp1 - tmp6);
				dst[i * 8 + 2] = (s16)(tmp2 + tmp5), dst[i * 8 + 5] = (s16)(tmp2 - tmp5);
				dst[i * 8 + 4] = (s16)(tmp3 + tmp4), dst[i * 8 + 3] = (s16)(tmp3 - tmp4);
			}
		}
		Swap(src, dst);
	}
}

//
// Each macroblock consists of six blocks:
// - Two low-resolution blocks with color information (Cr,Cb)
// - Four full-resolution blocks with luminance (grayscale) information (Y1,Y2,Y3,Y4).
// The color blocks are zoomed from 8x8 to 16x16 pixel size, merged with the luminance blocks, and then converted from YUV to RGB format.
//
//    .-----.       .-----.       .-----.         .-----.
//    |     |       |     |       |Y1|Y2|         |     |
//    | Cr  |   +   | Cb  |   +   |--+--|  ---->  | RGB |
//    |     |       |     |       |Y3|Y4|         |     |
//    '-----'       '-----'       '-----'         '-----'
// 
// https://psx-spx.consoledev.net/macroblockdecodermdec/#yuv_to_rgbxxyy
//
static void yuv_to_rgb(
	unsigned int x0, unsigned int y0,
	const s16 Yblk[8*8], const s16 CbBlk[8 * 8], const s16 CrBlk[8 * 8], // YCbCr order
	u8* dstBlk, // 16x16 #TODO: This will need to be a u8* so can append bytes for desired output depth 15bpp or 24bpp. It can't be a u16[]
	bool isUnsigned, bool output15bpp, bool setBit15)
{
	for (unsigned int iy = 0; iy < 8; iy++)
	{
		unsigned int y = y0 + iy;
		for (unsigned int ix = 0; ix < 8; ix++)
		{
			unsigned int x = x0 + ix;

			// Sample luminance at full resolution (8x8 luma blocks)
			s16 Y = Yblk[ix + (iy * 8)];

			// Sample chroma at half resolution (8x8 chroma covers 16x16 luma)
			unsigned int chromaIndex = (x >> 1) + ((y >> 1) * 8); // (x/2) + ((y/2)*8)
			s16 Cr = CrBlk[chromaIndex]; // R=[Crblk+((x+xx)/2)+((y+yy)/2)*8]
			s16 Cb = CbBlk[chromaIndex]; // B=[Cbblk+((x+xx)/2)+((y+yy)/2)*8]

			int R = (int)(Y + (1.402f * (float)Cr));
			int B = (int)(Y + (1.772f * (float)Cb));
			int G = (int)(Y - (0.3437f * (float)Cb) - (0.7143f * (float)Cr));

			R = Clamp(R, -128, 127);
			G = Clamp(G, -128, 127);
			B = Clamp(B, -128, 127);
			if (isUnsigned)
			{
				// BGR = BGR xor 808080h; // add 128 to the R, G, B values
				R += 128;
				G += 128;
				B += 128;
			}

			if (output15bpp)
			{
				constexpr unsigned int kBytesPerPixel = 2;
				u16* pixel = (u16*)(dstBlk + ((x + (y * 16)) * kBytesPerPixel));

				// Convert to A1B5G5R5 format, with optional bit 15 set based on setBit15 parameter
				u16 r5 = ((u8)R >> 3) & 0x1F;
				u16 g5 = ((u8)G >> 3) & 0x1F;
				u16 b5 = ((u8)B >> 3) & 0x1F;
				u16 a1 = setBit15 ? 0x8000 : 0;
				*pixel = a1 | (b5 << 10) | (g5 << 5) | r5;
			}
			else // 24bpp
			{
				constexpr unsigned int kBytesPerPixel = 3;
				u8* pixel = dstBlk + ((x + (y * 16)) * kBytesPerPixel);

				// PSX stores 24 bpp pixels in B8G8R8 order (Blue, Green, Red bytes)
				pixel[0] = (u8)R;
				pixel[1] = (u8)G;
				pixel[2] = (u8)B;
			}
		}
	}
}

// https://psx-spx.consoledev.net/macroblockdecodermdec/#decode_monochrome_macroblock-mdec1-command-at-4bpp-or-8bpp-depth
void MDEC::decodeMonochromeMacroblocks(const u16* src, unsigned int srcSizeHalfwords)
{
	HP_FATAL_ERROR("Not implemented");
	HP_UNUSED(src);
	HP_UNUSED(srcSizeHalfwords);
}
