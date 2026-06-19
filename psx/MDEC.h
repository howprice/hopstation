// PSX Macroblock Decoder (MDEC)
//
// Very similar to JPEG. Great Branch Education video "How are Images Compressed? https://youtu.be/Kv1Hiv3ox8I
// 
// Comprehensive spec: https://psx-spx.consoledev.net/macroblockdecodermdec/
//
// MDEC decoding makes heavy use of DMA:
// 1. DMA3 (CDROM)    copy compressed data from CDROM to RAM
// 2. DMA0 (MDEC.In)  copy compressed data from RAM to MDEC
// 3. DMA1 (MDEC.Out) copy uncompressed macroblocks from MDEC to RAM
// 4. DMA2 (GPU)      copy uncompressed macroblocks from RAM to GPU
//
// Y'CbCr colour space:
//   Y' = luma (luma is gamma-encoded, whereas luminance Y is linear)
//   Cb = blue difference chroma
//   Cr = red difference chroma
//

#pragma once

#include "core/Types.h"

inline bool g_logMDEC = false;
inline bool g_logMDECDataRegister = false;

class MDEC
{
public:

	MDEC();
	~MDEC();

	void Reset();

	// Write Command/Parameter Register
	// Used by CPU and DMA0 (MDEC.In) to send commands and parameters to MDEC.
	void WriteMDEC0(u32 val, u32 pc);

	// Write Control/Reset Register
	void WriteMDEC1(u32 val);

	// Read Data/Response Register
	u32 ReadMDEC0();

	// Read Status Register
	u32 ReadMDEC1() const;

	// Optimisation for immediate DMA to copy directly from RAM into MDEC.
	void WriteDataBlock(const u8* data, unsigned int numWords);

	// DMA1 - MDECout (MDEC to RAM) helper
	// Optimisation for immediate DMA to copy directly from sector data buffer into RAM.
	// Caller should check that required number of bytes are available and ensure that dst buffer is large enough.
	void ReadDataBlock(u8* dst, unsigned int numBytes);

private:

	void command0(u32 val);
	void command1(u32 val);
	void command2(u32 val);
	void command3(u32 val);

	void decode(const u8* src, unsigned int srcSizeBytes);
	void decodeColoredMacroblocks(const u16* src, unsigned int srcSizeHalfwords);
	void decodeMonochromeMacroblocks(const u16* src, unsigned int srcSizeHalfwords);
	void decodeBlock(s16 blk[64], const u16* & src, unsigned int & srcSizeBytes, const u8 q[64]);
	void runLengthDecodeBlock(const u16* & src, unsigned int & srcSizeHalfwords, s16 blk[64], const u8 q[64], bool fastIDCT);
	void real_idct_core(s16 blk[64]) const;

	bool isOutputFIFOEmpty() const;

	enum class State
	{
		Idle,
		ReceivingLuminanceQuantTable,
		ReceivingColorQuantTable,
		ReceivingScaleTable,
		Decompressing,
	};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

	enum class DataOutDepth : u32
	{
		k4Bit = 0,
		k8Bit = 1,
		k24Bit = 2,
		k15Bit = 3,

		Max = k15Bit
	};

	enum class Block : u32
	{
		Y1 = 0,
		Y2 = 1,
		Y3 = 2,
		Y4 = 3,
		Cr = 4,
		Cb = 5,
		Max = Cb
	};

	// MDEC Status Register
	//
	//  Bit(s)
	//   31     Data-Out Fifo Empty (0=No, 1=Empty)
	//   30     Data-In Fifo Full   (0=No, 1=Full, or Last word received)
	//   29     Command Busy  (0=Ready, 1=Busy receiving or processing parameters)
	//   28     Data-In Request  (set when DMA0 enabled and ready to receive data)
	//   27     Data-Out Request (set when DMA1 enabled and ready to send data)
	//   26-25  Data Output Depth  (0=4bit, 1=8bit, 2=24bit, 3=15bit)      ;CMD.28-27
	//   24     Data Output Signed (0=Unsigned, 1=Signed)                  ;CMD.26
	//   23     Data Output Bit15  (0=Clear, 1=Set) (for 15bit depth only) ;CMD.25
	//   22-19  Not used (seems to be always zero)
	//   18-16  Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always 4=Y)
	//   15-0   Number of Parameter Words remaining minus 1  (FFFFh=None)  ;CMD.Bit0-15
	// 
	// https://psx-spx.consoledev.net/macroblockdecodermdec/#1f801824h-mdec1-mdec-status-register-r
	//
	struct Status
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 ParameterWordsRemainingMinus1 : 16; // 15:0
				Block CurrentBlock : 3;                 // 18:16
				u32 unusedBits22to19 : 4;               // 22:19
				u32 DataOutputBit15 : 1;                // 23
				u32 DataOutputSigned : 1;               // 24
				DataOutDepth DataOutputDepth : 2;       // 26:25
				u32 DataOutRequest : 1;                 // 27
				u32 DataInRequest : 1;                  // 28
				u32 CommandBusy : 1;                    // 29
				u32 DataInFifoFull : 1;                 // 30
				u32 DataOutFifoEmpty : 1;               // 31
			};
		};
	};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

	State m_state = State::Idle;
	unsigned int m_compressedDataSizeWords = 0;
	bool m_expectColorTable = false; // command 2 state
	unsigned int m_parameterWordsReceived = 0; // for current command

	Status m_status{};
	bool m_dmaInEnabled = false;
	bool m_dmaOutEnabled = false;

	u8 m_luminanceQuantTable[64]{}; // aka iq_y
	u8 m_colorQuantTable[64]{}; // aka iq_uv
	s16 m_scaleTable[64]{}; // IDCT scale matrix. See https://psx-spx.consoledev.net/macroblockdecodermdec/#set_scale_table-mdec3-command

	// Output FIFO containing decoded data
	u8* m_outputBuffer = nullptr;
	unsigned int m_outputWriteIndex = 0; // How many bytes of decoded data are currently in the buffer, which is used for DMA1 output.
	unsigned int m_outputReadIndex = 0;
};
