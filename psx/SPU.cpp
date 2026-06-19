/*

SPU I/O Port Summary
  1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
  1F801D80h..1F801D87h - SPU Control (volume)
  1F801D88h..1F801D9Fh - Voice 0..23 Flags (six 1 bit flags per voice)
  1F801DA2h..1F801DBFh - SPU Control (memory, control, etc.)
  1F801DC0h..1F801DFFh - Reverb configuration area
  1F801E00h..1F801E5Fh - Voice 0..23 Internal Registers
  1F801E60h..1F801E7Fh - Unknown?
  1F801E80h..1F801FFFh - Unused?

1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
  1F801C00h+N*10h 4   Voice 0..23 Volume Left/Right
  1F801C04h+N*10h 2   Voice 0..23 ADPCM Sample Rate
  1F801C06h+N*10h 2   Voice 0..23 ADPCM Start Address
  1F801C08h+N*10h 4   Voice 0..23 ADSR Attack/Decay/Sustain/Release
  1F801C0Ch+N*10h 2   Voice 0..23 ADSR Current Volume
  1F801C0Eh+N*10h 2   Voice 0..23 ADPCM Repeat Address

SPU Control Registers
  1F801D80h 4  Main Volume Left/Right
  1F801D84h 4  Reverb Output Volume Left/Right
  1F801D88h 4  Voice 0..23 Key ON (Start Attack/Decay/Sustain) (W)
  1F801D8Ch 4  Voice 0..23 Key OFF (Start Release) (W)
  1F801D90h 4  Voice 0..23 Channel FM (pitch LFO) mode (R/W)
  1F801D94h 4  Voice 0..23 Channel Noise mode (R/W)
  1F801D98h 4  Voice 0..23 Channel Reverb mode (R/W)
  1F801D9Ch 4  Voice 0..23 Channel ON/OFF (status) (R)
  1F801DA0h 2  Unknown? (R) or (W)
  1F801DA2h 2  Sound RAM Reverb Work Area Start Address
  1F801DA4h 2  Sound RAM IRQ Address
  1F801DA6h 2  Sound RAM Data Transfer Address
  1F801DA8h 2  Sound RAM Data Transfer FIFO
  1F801DAAh 2  SPU Control Register (SPUCNT)
  1F801DACh 2  Sound RAM Data Transfer Control
  1F801DAEh 2  SPU Status Register (SPUSTAT) (R)
  1F801DB0h 4  CD Volume Left/Right
  1F801DB4h 4  Extern Volume Left/Right
  1F801DB8h 4  Current Main Volume Left/Right
  1F801DBCh 4  Unknown? (R/W)

SPU Reverb Configuration Area
  1F801DC0h 2  dAPF1  Reverb APF Offset 1
  1F801DC2h 2  dAPF2  Reverb APF Offset 2
  1F801DC4h 2  vIIR   Reverb Reflection Volume 1
  1F801DC6h 2  vCOMB1 Reverb Comb Volume 1
  1F801DC8h 2  vCOMB2 Reverb Comb Volume 2
  1F801DCAh 2  vCOMB3 Reverb Comb Volume 3
  1F801DCCh 2  vCOMB4 Reverb Comb Volume 4
  1F801DCEh 2  vWALL  Reverb Reflection Volume 2
  1F801DD0h 2  vAPF1  Reverb APF Volume 1
  1F801DD2h 2  vAPF2  Reverb APF Volume 2
  1F801DD4h 4  mSAME  Reverb Same Side Reflection Address 1 Left/Right
  1F801DD8h 4  mCOMB1 Reverb Comb Address 1 Left/Right
  1F801DDCh 4  mCOMB2 Reverb Comb Address 2 Left/Right
  1F801DE0h 4  dSAME  Reverb Same Side Reflection Address 2 Left/Right
  1F801DE4h 4  mDIFF  Reverb Different Side Reflection Address 1 Left/Right
  1F801DE8h 4  mCOMB3 Reverb Comb Address 3 Left/Right
  1F801DECh 4  mCOMB4 Reverb Comb Address 4 Left/Right
  1F801DF0h 4  dDIFF  Reverb Different Side Reflection Address 2 Left/Right
  1F801DF4h 4  mAPF1  Reverb APF Address 1 Left/Right
  1F801DF8h 4  mAPF2  Reverb APF Address 2 Left/Right
  1F801DFCh 4  vIN    Reverb Input Volume Left/Right

SPU Internal Registers

  1F801E00h+N*04h  4 Voice 0..23 Current Volume Left/Right
  1F801E60h      20h Unknown? (R/W)
  1F801E80h     180h Unknown? (Read: FFh-filled) (Unused or Write only?)
*/

#include "SPU.h"

#include "INTC.h"
#include "CDROM.h"
#include "Scheduler.h"

#include "core/RingBuffer.h"
#include "core/Log.h"
#include "core/ArrayHelpers.h"
#include "core/Helpers.h" // ENUM_COUNT
#include "core/MathsHelpers.h"
#include "core/hp_assert.h"

#include <string.h> // memset

#if defined(_MSC_VER)
#include <intrin.h> // _tzcnt_u32 for COUNT_TRAILING_ZEROS
#endif

#define SPU_REVERB_DEBUG_BUFFERS_ENABLED 0

static const u32 kSPUBaseAddress = 0x1F801C00;

static_assert(COUNTOF_ARRAY(kADSRPhaseNames) == ENUM_COUNT(SPU::ADSRPhase));
static_assert(COUNTOF_ARRAY(kSoundRAMTransferModeNames) == ENUM_COUNT(SPU::SoundRAMTransferMode));
static_assert(COUNTOF_ARRAY(kReverbRegisterNames) == COUNTOF_ARRAY(SPU::ReverbRegisters::reg));

static const char* kVoiceRegNames[] =
{
	"Volume Left",
	"Volume Right",
	"ADPCM Sample Rate",
	"ADPCM Start Address",
	"ADSR Low",
	"ADSR High",
	"ADSR Current Volume",
	"ADPCM Repeat Address",
};

// 4-point Gaussian sample interpolation table
// Each multiplier represents N/8000h
// https://psx-spx.consoledev.net/soundprocessingunitspu/#4-point-gaussian-interpolation
static const s16 kGaussianInterpolationTable[] =
{
	-0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
	-0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, //
	0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003, //
	0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007, //
	0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, //
	0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018, // entry
	0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025, // 000h..07Fh
	0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038, //
	0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050, //
	0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F, //
	0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096, //
	0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7, //
	0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101, //
	0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148, //
	0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C, //

	0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200, //
	0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273, //
	0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9, //
	0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392, //
	0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441, //
	0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506, //
	0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4, // entry
	0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC, // 080h..0FFh
	0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF, //
	0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E, //
	0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C, //
	0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8, //
	0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63, //
	0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F, //
	0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB, //
	0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7, //

	0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4, //
	0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700, //
	0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B, //
	0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3, //
	0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37, //
	0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4, //
	0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389, // entry
	0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653, // 100h..17Fh
	0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E, //
	0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18, //
	0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D, //
	0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209, //
	0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509, //
	0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807, //
	0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00, //
	0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF, //

	0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0, //
	0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C, //
	0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651, //
	0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9, //
	0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F, //
	0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0, //
	0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7, // entry
	0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0, // 180h..1FFh
	0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397, //
	0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529, //
	0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684, //
	0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3, //
	0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886, //
	0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A, //
	0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F, //
	0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3  // 
};
static_assert(COUNTOF_ARRAY(kGaussianInterpolationTable) == 0x200);

// 39-tap PSX SPU FIR filter
// https://psx-spx.consoledev.net/soundprocessingunitspu/#reverb-buffer-resampling
// Each coefficient is a volume multipler N/0x8000, so after multiplying by the input sample, the result needs to be shift right by 15 to rescale.
// Note that every other sample except the peak is zero, so this is effectively a 20-tap filter with zeros interleaved for downsampling by 2.
// 
// The filter is also used for upsampling!
//
static const s16 kFiniteImpulseResponse[] =
{
	-0x0001,  0x0000,  0x0002,  0x0000, -0x000A,  0x0000,  0x0023,  0000,
	-0x0067,  0x0000,  0x010A,  0x0000, -0x0268,  0x0000,  0x0534,  0000,
	-0x0B90,  0x0000,  0x2806,  0x4000,  0x2806,  0x0000, -0x0B90,  0000,
	 0x0534,  0x0000, -0x0268,  0x0000,  0x010A,  0x0000, -0x0067,  0000,
	 0x0023,  0x0000, -0x000A,  0x0000,  0x0002,  0x0000, -0x0001,
};
static_assert(COUNTOF_ARRAY(kFiniteImpulseResponse) == SPU::kFIRFilterSize);

//-----------------------------------------------------------------------------------------------------------------------------------------------------

//
// Saturates signed 32-bit value to signed 16-bit range
//
static s16 saturateS32toS16(s32 val)
{
	return (s16)Clamp(val, (s32)INT16_MIN, (s32)INT16_MAX);
}

//-----------------------------------------------------------------------------------------------------------------------------------------------------

static bool s_bRecordCD = false;
static FILE* s_pCDAudioFile;

static bool s_bRecordOutput = false;
static FILE* s_pOutputFile;

static bool s_bRecordReverb = false;
static FILE* s_pReverbFile;

//-----------------------------------------------------------------------------------------------------------------------------------------------------

SPU::SPU(INTC& intc, Scheduler& scheduler, CDROM& cdrom)
	: m_intc(intc)
	, m_scheduler(scheduler)
	, m_cdrom(cdrom)
{
	m_ram = new u8[kRamSizeBytes];
	memset(m_ram, 0, kRamSizeBytes);

	scheduleClockCallback();

	if (s_bRecordReverb)
	{
		s_pReverbFile = fopen("hpsx_reverb.raw", "wb");
		HP_ASSERT(s_bRecordReverb);
	}

	if (s_bRecordOutput)
	{
		s_pOutputFile = fopen("hpsx_spu_out.raw", "wb");
		HP_ASSERT(s_pOutputFile);
	}

	if (s_bRecordCD)
	{
		s_pCDAudioFile = fopen("hpsx_cd_audio.raw", "wb");
		HP_ASSERT(s_pCDAudioFile);
	}
}

SPU::~SPU()
{
	delete[] m_ram;

	if (s_pReverbFile)
	{
		fclose(s_pReverbFile);
		s_pReverbFile = nullptr;
	}

	if (s_pOutputFile)
	{
		fclose(s_pOutputFile);
		s_pOutputFile = nullptr;
	}

	if (s_pCDAudioFile)
	{
		fclose(s_pCDAudioFile);
		s_pCDAudioFile = nullptr;
	}
}

void SPU::Reset()
{
	for (unsigned int i = 0; i < kVoiceCount; i++)
	{
		m_voices[i] = {};
	}

	m_mainVolumeLeft = 0;
	m_mainVolumeRight = 0;
	m_reverbOutputVolumeLeft = 0;
	m_reverbOutputVolumeRight = 0;
	m_voiceFlags = {};
	m_soundRAMReverbWorkAreaStartAddressDiv8 = 0;
	m_reverbBufferStartAddress = 0;
	m_currentReverbBufferHead = 0;
	m_reverbBufferSizeBytes = 0;
	m_soundRAMIRQAddress = 0;
	m_soundRAMDataTransferAddress = 0;
	m_currentSoundRAMDataTransferAddress = 0;
	m_stat.val = 0;
	m_spucnt.val = 0;
	m_soundRAMDataTransferControl = 0;
	m_cdVolumeLeft = 0;
	m_cdVolumeRight = 0;
	m_externalVolumeLeft = 0;
	m_externalVolumeRight = 0;
	m_currentMainVolumeLeft = 0;
	m_currentMainVolumeRight = 0;

	m_rev = {};
	m_reverbCounter = false;
	memset(m_downsamplerRingbufferL, 0, sizeof(m_downsamplerRingbufferL));
	memset(m_downsamplerRingbufferR, 0, sizeof(m_downsamplerRingbufferR));
	m_downsamplerRingbufferIndex = 0; // Circular buffer index for downsampler input
	memset(m_upsamplerRingbufferL, 0, sizeof(m_upsamplerRingbufferL));
	memset(m_upsamplerRingbufferR, 0, sizeof(m_upsamplerRingbufferR));
	m_upsamplerRingbufferIndex = 0; // Circular buffer index for upsampler input

	memset(m_ram, 0, kRamSizeBytes);
	m_captureBufferWriteOffset = 0;

	m_noiseTimer = 0;
	m_noiseLevel = 0;

	scheduleClockCallback();
}

void SPU::Write16(unsigned int address, u16 val)
{
	if (address < 0x180) // 1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
	{
		unsigned int voiceIndex = address >> 4; // eight 2 byte registers = 16 bytes per voice
		HP_DEBUG_ASSERT(voiceIndex < kVoiceCount);
		Voice& voice = m_voices[voiceIndex];
		unsigned int voiceRegIndex = (address >> 1) & 7;
		voice.reg[voiceRegIndex] = val;
		if (g_logSPU)
			LOG_INFO("[SPU] Voice %02Xh %s write val %04X\n", voiceIndex, kVoiceRegNames[voiceRegIndex], val);
	}
	else if (address == 0x180) // 1F801D80h - Main volume left
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Main volume left %04X -> %04X\n", m_mainVolumeLeft, val);
		m_mainVolumeLeft = val;
	}
	else if (address == 0x182) // 1F801D82h - Main volume right
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Main volume right %04X -> %04X\n", m_mainVolumeRight, val);
		m_mainVolumeRight = val;
	}
	else if (address == 0x184) // 1F801D84h
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Reverb output volume left %04X -> %04X\n", m_reverbOutputVolumeLeft, val);
		m_reverbOutputVolumeLeft = val;
	}
	else if (address == 0x186) // 1F801D86h
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Reverb output volume right %04X -> %04X\n", m_reverbOutputVolumeRight, val);
		m_reverbOutputVolumeRight = val;
	}
	else if (address == 0x188) // 1F801D88h (32 bits) - Voice 0..23 Key ON (Start Attack/Decay/Sustain) (KON) (W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KON low 16-bit write value %04X\n", val);

		if (val)
		{
			u32 voiceBits = (u32)val;
			keyOn(voiceBits);
		}

		// KON bits are supposed to be write-only, but can be read, so need to store the halfword that was written
		m_voiceFlags.kon = (m_voiceFlags.kon & 0xffff0000) | val;
	}
	else if (address == 0x18A)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KON high 16-bit write value %04X\n", val);

		if (val)
		{
			u32 voiceBits = (u32)val << 16;
			keyOn(voiceBits);
		}

		// KON bits are supposed to be write-only, but can be read, so need to store the halfword that was written
		m_voiceFlags.kon = ((u32)val << 16) | (m_voiceFlags.kon & 0x0000ffff);
	}
	else if (address == 0x18C) // 1F801D8Ch (32 bit) - Voice 0..23 Key OFF (Start Release) (KOFF) (W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KOFF low 16-bit write value %04X\n", val);

		if (val)
		{
			u32 voiceBits = (u32)val;
			keyOff(voiceBits);
		}

		// KOFF bits are supposed to be write-only, but can be read, so need to store the halfword that was written
		m_voiceFlags.koff = (m_voiceFlags.koff & 0xffff0000) | val;
	}
	else if (address == 0x18E)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KOFF high 16-bit write value %04X\n", val);

		if (val)
		{
			u32 voiceBits = (u32)val << 16;
			keyOff(voiceBits);
		}

		// KOFF bits are supposed to be write-only, but can be read, so need to store the halfword that was written
		m_voiceFlags.koff = ((u32)val << 16) | (m_voiceFlags.koff & 0x0000ffff);
	}
	else if (address == 0x190) // 1F801D90h (32 bit)  Voice 0..23 Channel FM (pitch LFO) mode (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags FM mode (PMON) low 16-bit write value %04X\n", val);
		m_voiceFlags.pmon = (m_voiceFlags.pmon & 0xffff0000) | val; // Little endian
	}
	else if (address == 0x192)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags FM mode (PMON) high 16-bit write value %04X\n", val);
		m_voiceFlags.pmon = ((u32)val << 16) | (m_voiceFlags.pmon & 0x0000ffff); // Little endian
	}
	else if (address == 0x194) // 1F801D94h (32 bit) Voice 0..23 Noise mode enable (NON) (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Noise mode enable (NON) low 16-bit write value %04X\n", val);
		m_voiceFlags.non = (m_voiceFlags.non & 0xffff0000) | val; // Little endian
	}
	else if (address == 0x196)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Noise mode enable (NON) high 16-bit write value %04X\n", val);
		m_voiceFlags.non = ((u32)val << 16) | (m_voiceFlags.non & 0x0000ffff); // Little endian
	}
	else if (address == 0x198) // 1F801D98h (32 bit) Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Reverb mode (EON) low 16-bit write value %04X\n", val);
		m_voiceFlags.eon = (m_voiceFlags.eon & 0xffff0000) | val; // Little endian
	}
	else if (address == 0x19A) // 1F801D98h (32 bit) Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Reverb mode (EON) high 16-bit write value %04X\n", val);
		m_voiceFlags.eon = ((u32)val << 16) | (m_voiceFlags.eon & 0x0000ffff); // Little endian
	}
	else if (address == 0x19C) // 1F801D9Ch (32 bits - Voice 0..23 ON/OFF (status) (ENDX) (Read-only)
	{
		// "The on/off (status) (ENDX) register should be treated read-only (writing is possible in so far that the written
		// value can be read-back for a short moment, however, thereafter the hardware is overwriting that value).
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags on/off status (ENDX) low 16-bit write value %04X\n", val);
		m_voiceFlags.endx = (m_voiceFlags.endx & 0xffff0000) | val; // Little endian
	}
	else if (address == 0x19E)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags on/off status (ENDX) high 16-bit write value %04X\n", val);
		m_voiceFlags.endx = ((u32)val << 16) | (m_voiceFlags.endx & 0x0000ffff); // Little endian
	}
	else if (address == 0x1A2) // 1F801DA2h Sound RAM Reverb Work Area Start Address
	{
		// "mBASE" Reverb Work Area Start Address in Sound RAM
		m_soundRAMReverbWorkAreaStartAddressDiv8 = val;

		m_reverbBufferStartAddress = m_soundRAMReverbWorkAreaStartAddressDiv8 * 8; // Convert from address in sound ram (divided by 8) to byte address

		// Calculate the reverb buffer size, which is required for accessing reverb buffer memory.
		HP_DEBUG_ASSERT(m_reverbBufferStartAddress <= SPU::kRamSizeBytes);
		m_reverbBufferSizeBytes = SPU::kRamSizeBytes - m_reverbBufferStartAddress;

		// Writing to mBASE set the current buffer address to that value.
		m_currentReverbBufferHead = m_reverbBufferStartAddress;

		if (g_logSPU)
			LOG_INFO("[SPU] Reverb buffer start address reg write value %04X. Buffer address: %08X Buffer size: %08X\n", val, m_reverbBufferStartAddress, m_reverbBufferSizeBytes);
	}
	else if (address == 0x1A4) // 1F801DA4h - Sound RAM IRQ Address (IRQ9)
	{
		m_soundRAMIRQAddress = val << 3; // bits 15:0 are address/8. Store actual address.
		if (g_logSPU)
			LOG_INFO("[SPU] Sound RAM IRQ Address write value %04X = %08X\n", val, m_soundRAMIRQAddress);
	}
	else if (address == 0x1A6) // 1F801DA6h - Sound RAM Data Transfer Address
	{
		m_soundRAMDataTransferAddress = val; // address/8 is stored
		m_currentSoundRAMDataTransferAddress = (u32)val * 8; // actual address = val * 8
		if (g_logSPU)
			LOG_INFO("[SPU] Sound RAM Data Transfer Address write value %04X = %08X\n", val, m_currentSoundRAMDataTransferAddress);
	}
	else if (address == 0x1A8) // 1F801DA8h 2  Sound RAM Data Transfer FIFO
	{
		// Most games use SPU DMA4 to transfer data into SPU RAM.
		// Some games and the BIOS use the CPU to write to this register instead.
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801da8h-sound-ram-data-transfer-fifo
		// Actual hardware would write into a FIFO, but don't think that needs to be emulated (at this point).
		if (g_logSPUDataTransfer) // On a different flag because BIOS hammers this during startup
			LOG_INFO("[SPU] Manual sound RAM write to address %08X value %04X \n", m_currentSoundRAMDataTransferAddress, val);

		HP_ASSERT(m_currentSoundRAMDataTransferAddress + 2 <= kRamSizeBytes); // #TODO: Wrap?
		*(u16*)(m_ram + m_currentSoundRAMDataTransferAddress) = val; // assumes host is little endian

		// n.b. This internal register is incremented, but 1F801DA6h is not.
		m_currentSoundRAMDataTransferAddress += 2;
	}
	else if (address == 0x1AA) // 1F801DAAh - SPU Control Register (SPUCNT)
	{
		const SPUCNT spucntPrev = m_spucnt;
		m_spucnt.val = val;

		if (g_logSPU)
			LOG_INFO(
				"[SPU] Write SPUCNT value %04X\n"
				"[SPU]   SPU Enable: %u\n"
				"[SPU]   Mute SPU: %u\n"
				"[SPU]   Noise Frequency Shift: %u\n"
				"[SPU]   Noise Frequency Step: %u = %u\n"
				"[SPU]   Reverb Master Enable: %u\n"
				"[SPU]   IRQ9 Enable: %u\n"
				"[SPU]   Sound RAM Transfer Mode: %u %s\n"
				"[SPU]   External Audio Reverb: %u\n"
				"[SPU]   CD AudioReverb: %u\n"
				"[SPU]   External Audio Enable: %u\n"
				"[SPU]   CD Audio Enable: %u\n",
				m_spucnt.val,
				m_spucnt.SPUEnable,
				m_spucnt.MuteSPU,
				m_spucnt.NoiseFrequencyShift,
				m_spucnt.NoiseFrequencyStep, m_spucnt.NoiseFrequencyStep + 4, // [0,3] = [4,7]
				m_spucnt.ReverbMasterEnable,
				m_spucnt.IRQ9Enable,
				(int)m_spucnt.soundRAMTransferMode, kSoundRAMTransferModeNames[(int)m_spucnt.soundRAMTransferMode],
				m_spucnt.ExternalAudioReverb,
				m_spucnt.CDAudioReverb,
				m_spucnt.ExternalAudioEnable,
				m_spucnt.CDAudioEnable);

		if (g_logSPU && m_spucnt.IRQ9Enable != spucntPrev.IRQ9Enable)
			LOG_INFO("[SPU] IRQ9 %s\n", m_spucnt.IRQ9Enable ? "enabled" : "disabled");

		// "Games acknowledge SPU IRQs by disabling them in the SPUCNT register, which should immediately clear the internal SPU IRQ flag"
		// https://jsgroth.dev/blog/posts/ps1-spu-part-4/#spu-irqs
		if (!m_spucnt.IRQ9Enable)
		{
			if (m_stat.irq)
			{
				m_stat.irq = 0;
				if (g_logSPU)
					LOG_INFO("[SPU] IRQ9 acknowledged\n");
			}
		}
	}
	else if (address == 0x1AC) // 1F801DACh - Sound RAM Data Transfer Control (should be 0004h)
	{
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801dach-sound-ram-data-transfer-control-should-be-0004h
		if (g_logSPU)
			LOG_INFO("[SPU] Sound RAM Data Transfer Control write %04X\n", val);
		HP_ASSERT(val == 4, "Docs say value should be 4");
		m_soundRAMDataTransferControl = val;
	}
	else if (address == 0x1B0) // 1F801DB0h CD volume left
	{
		if (g_logSPU)
			LOG_INFO("[SPU] CD volume left %04X -> %04X\n", m_cdVolumeLeft, val); // #TODO: Display -ve hex
		m_cdVolumeLeft = val;
	}
	else if (address == 0x1B2) // 1F801DB2h CD volume right
	{
		if (g_logSPU)
			LOG_INFO("[SPU] CD volume right %04X -> %04X\n", m_cdVolumeRight, val); // #TODO: Display -ve hex
		m_cdVolumeRight = val;
	}
	else if (address == 0x1B4) // 1F801DB4h External volume left
	{
		if (g_logSPU)
			LOG_INFO("[SPU] External volume left %04X -> %04X\n", m_externalVolumeLeft, val);
		m_externalVolumeLeft = val;
	}
	else if (address == 0x1B6) // 1F801DB6h External volume right
	{
		if (g_logSPU)
			LOG_INFO("[SPU] External volume right %04X -> %04X\n", m_externalVolumeRight, val);
		m_externalVolumeRight = val;
	}
	else if (address == 0x1B8) // 1F801DB8h Current Main Volume Left
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Main volume left %04X -> %04X\n", m_currentMainVolumeLeft, val);
		m_currentMainVolumeLeft = val;
	}
	else if (address == 0x1BA) // 1F801DBAh Current Main Volume Right
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Main volume right %04X -> %04X\n", m_currentMainVolumeRight, val);
		m_currentMainVolumeRight = val;
	}
	else if (address >= 0x1C0 && address < 0x200) // 1F801DC0h..1F801DFFh - Reverb configuration area
	{
		// 32 16-bit registers
		unsigned int revRegIndex = (address - 0x1c0) >> 1;
		HP_DEBUG_ASSERT(revRegIndex < COUNTOF_ARRAY(m_rev.reg));
		m_rev.reg[revRegIndex] = val;
		if (g_logSPU)
			LOG_INFO("[SPU] Reverb register rev%02x %s write address %08X value %04X\n", revRegIndex, kReverbRegisterNames[revRegIndex], kSPUBaseAddress + address, val);
	}
	else
	{
#if SPU_FATAL_UNIMPLEMENTED
		HP_FATAL_ERROR("[SPU] Unimplemented 16-bit write to address %08X (%08X) value %04X\n", address, kSPUBaseAddress + address, val);
#else
		if (g_logSPU)
			LOG_INFO("[SPU] Unimplemented 16-bit write to address %08X (%08X) value %04X\n", address, kSPUBaseAddress + address, val);
#endif
	}
}

void SPU::Write32(unsigned int address, u32 val)
{
	if (address < 0x180) // 1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
	{
		HP_ASSERT((address & 3) == 0, "Not word aligned");
		unsigned int voiceIndex = address >> 4; // eight 2 byte registers = 16 bytes per voice
		HP_DEBUG_ASSERT(voiceIndex < kVoiceCount);
		Voice& voice = m_voices[voiceIndex];
		unsigned int voiceRegIndex = (address >> 1) & 7;

		// Little-endian: lower 16 bits go to first register, upper 16 bits to second
		voice.reg[voiceRegIndex] = val & 0xffff;
		voice.reg[voiceRegIndex + 1] = val >> 16;  // Freen squiggles on this line, but voiceRegIndex is always even (0,2,4,6) due to 32-bit alignment, so +1 is safe
		if (g_logSPU)
			LOG_INFO("[SPU] Voice %02Xh 32-bit write val %08X: %s <- %04X, %s <- %04X\n", voiceIndex, val, kVoiceRegNames[voiceRegIndex], voice.reg[voiceRegIndex], kVoiceRegNames[voiceRegIndex + 1], voice.reg[voiceRegIndex + 1]);
	}
	else if (address == 0x180) // 1F801D80h - Main volume
	{
		// Little-endian: lower 16 bits go to first register, upper 16 bits to second
		m_mainVolumeLeft = val & 0xffff;
		m_mainVolumeRight = val >> 16;
		if (g_logSPU)
			LOG_INFO("[SPU] Main volume 32-bit write val %08X left <- %04X right <- %04X\n", val, m_mainVolumeLeft, m_mainVolumeRight);
	}
	else if (address == 0x184) // 1F801D84h
	{
		// Little-endian: lower 16 bits go to first register, upper 16 bits to second
		m_reverbOutputVolumeLeft = val & 0xffff;
		m_reverbOutputVolumeRight = val >> 16;
		if (g_logSPU)
			LOG_INFO("[SPU] Reverb output volume 32-bit write %08X left <- %04X right <- %04X\n", val, m_reverbOutputVolumeLeft, m_reverbOutputVolumeRight);
	}
	else if (address == 0x188) // 1F801D88h (32 bits) - Voice 0..23 Key ON (Start Attack/Decay/Sustain) (KON) (W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KON 32-bit write value %08X\n", val);

		if (val)
			keyOn(val);

		// KON bits are supposed to be write-only, but can be read, so need to store the value that was written.
		m_voiceFlags.kon = val;
	}
	else if (address == 0x18C) // 1F801D8Ch (32 bit) - Voice 0..23 Key OFF (Start Release) (KOFF) (W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] KOFF 32-bit write value %08X\n", val);

		if (val)
			keyOff(val);

		// KOFF bits are supposed to be write-only, but can be read, so need to store the halfword that was written.
		m_voiceFlags.koff = val;
	}
	else if (address == 0x190) // 1F801D90h (32 bit)  Voice 0..23 Channel FM (pitch LFO) mode (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags pitch modulation (PMON) 32-bit write value %08X\n", val);
		m_voiceFlags.pmon = val;
	}
	else if (address == 0x194) // 1F801D94h (32 bit) Voice 0..23 Noise mode enable (NON) (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Noise mode enable (NON) 32-bit write value %08X\n", val);
		m_voiceFlags.non = val;
	}
	else if (address == 0x198) // 1F801D98h (32 bit) Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Reverb mode (EON) 32-bit write value %08X\n", val);
		m_voiceFlags.eon = val;
	}
	else
	{
#if SPU_FATAL_UNIMPLEMENTED
		HP_FATAL_ERROR("[SPU] Unimplemented 32-bit write to address %08X (%08X) value %08X\n", address, kSPUBaseAddress + address, val);
#else
		if (g_logSPU)
			LOG_INFO("[SPU] Unimplemented 32-bit write to address %08X (%08X) value %08X\n", address, kSPUBaseAddress + address, val);
#endif
	}
}

u16 SPU::Read16(unsigned int address)
{
	if (address < 0x180) // 1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
	{
		unsigned int voiceIndex = address >> 4; // eight 2 byte registers = 16 bytes per voice
		HP_DEBUG_ASSERT(voiceIndex < kVoiceCount);
		const Voice& voice = m_voices[voiceIndex];
		unsigned int voiceRegIndex = (address >> 1) & 7;
		u16 val = voice.reg[voiceRegIndex];
		if (g_logSPU)
			LOG_INFO("[SPU] Voice %02Xh %s read val %04X\n", voiceIndex, kVoiceRegNames[voiceRegIndex], val);
		return val;
	}
	else if (address == 0x188) // 1F801D88h (32 bits) Voice 0..23 Key ON (Start Attack/Decay/Sustain) (KON) (W)
	{
		// Reads from this write-only register are allowed. https://psx-spx.consoledev.net/soundprocessingunitspu/#rw
		u16 val = m_voiceFlags.kon & 0xffff; // lower 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] KON lower 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x18A)
	{
		// Reads from this write-only register are allowed. https://psx-spx.consoledev.net/soundprocessingunitspu/#rw
		u16 val = m_voiceFlags.kon >> 16; // upper 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] KON upper 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x18C) // 1F801D8Ch (32 bits) Voice 0..23 Key OFF (Start Release) (KOFF) Write only, but Reads from this *are* allowed. https://psx-spx.consoledev.net/soundprocessingunitspu/#rw
	{
		u16 val = m_voiceFlags.koff & 0xffff; // lower 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] KOFF lower 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x18E) // Reads from this write-only register are allowed. https://psx-spx.consoledev.net/soundprocessingunitspu/#rw
	{
		u16 val = m_voiceFlags.koff >> 16; // upper 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] KOFF upper 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x190) // 1F801D90h (32 bit) Voice 0..23 Pitch Modulation "PMON" (FM LFO) Enable Flags (R/W)
	{
		u16 val = m_voiceFlags.pmon & 0xffff; // little endian; lower 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags pitch modulation (PMON) low 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x192) // 1F801D92h (as above)
	{
		u16 val = m_voiceFlags.pmon >> 16; // little endian; upper 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags pitch modulation (PMON) high 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x194) // 1F801D94h (32-bit) Voice 0..23 Noise mode enable (NON) (R/W)
	{
		u16 val = m_voiceFlags.non & 0xffff; // little endian; lower 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags noise mode enable (NON) low 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x196) // 1F801D96h (as above)
	{
		u16 val = m_voiceFlags.non >> 16; // little endian; upper 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags noise mode enable (NON) high 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x198) // 1F801D98h (32 bit) Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
	{
		u16 val = m_voiceFlags.eon & 0xffff; // little endian; lower 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Reverb mode (EON) low 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x19A) // 1F801D98h (32 bit) Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
	{
		u16 val = m_voiceFlags.eon >> 16; // little endian; upper 16-bits
		if (g_logSPU)
			LOG_INFO("[SPU] Voice flags Reverb mode (EON) high 16-bit read value %04X\n", val);
		return val;
	}
	else if (address == 0x1A6) // 1F801DA6h - Sound RAM Data Transfer Address
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Read Sound RAM Data Transfer Address value %04X (address %08X)\n", m_soundRAMDataTransferAddress, (u32)m_soundRAMDataTransferAddress * 8);
		return m_soundRAMDataTransferAddress;
	}
	else if (address == 0x1AA) // 1F801DAAh - SPU Control Register (SPUCNT)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Read SPUCNT value %04X\n", m_spucnt.val);
		return m_spucnt.val;
	}
	else if (address == 0x1AC) // 1F801DACh - Sound RAM Data Transfer Control (should be 0004h)
	{
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801dach-sound-ram-data-transfer-control-should-be-0004h
		if (g_logSPU)
			LOG_INFO("[SPU] Sound RAM Data Transfer Control read %04X\n", m_soundRAMDataTransferControl);
		return m_soundRAMDataTransferControl;
	}
	else if (address == 0x1AE) // 1F801DAEh 2  SPU Status Register (SPUSTAT) (R)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] SPUSTAT 16-bit read value %04X\n", m_stat.val);
		return m_stat.val;
	}
	else if (address == 0x1B8) // 1F801DB8h Current main volume left
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Current main volume left read %04X\n", m_currentMainVolumeLeft);
		return m_currentMainVolumeLeft;
	}
	else if (address == 0x1BA) // 1F801DBAh Current main volume right
	{
		if (g_logSPU)
			LOG_INFO("[SPU] Current main volume right read %04X\n", m_currentMainVolumeRight);
		return m_currentMainVolumeRight;
	}
	else if (address >= 0x200 && address < (0x200 + kVoiceCount * 4)) // 1F801E00h+voice*04h - Voice 0..23 Current Volume Left/Right https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801e00hvoice04h-voice-023-current-volume-leftright
	{
		// #TODO: What value should "current volume" be? Search on emudev for this. Make sure it is maintained.
		unsigned int voiceIndex = (address - 0x200) >> 2; // 4 bytes per voice
		HP_DEBUG_ASSERT(voiceIndex < kVoiceCount);
		const Voice& voice = m_voices[voiceIndex];
		u16 val = (address & 2) ? voice.volumeRight : voice.volumeLeft; // #TODO: Is this the "current volume"?
		if (g_logSPU)
			LOG_INFO("[SPU] Voice %02Xh current volume %s read current volume val %04X\n", voiceIndex, (address & 2) ? "right" : "left",  val);
		return val;
	}
	else
	{
#if SPU_FATAL_UNIMPLEMENTED
		HP_FATAL_ERROR("[SPU] Unimplemented 16-bit read from address %08X (%08X)\n", address, kSPUBaseAddress + address);
#else
		if (g_logSPU)
			LOG_INFO("[SPU] Unimplemented 16-bit read from address %08X (%08X)\n", address, kSPUBaseAddress + address);
#endif
	}
	return 0;
}

void SPU::WriteData32(u32 val)
{
	// Most games use SPU DMA4 to transfer data into SPU RAM.
	// Some games and the BIOS use the CPU to write to this register instead.
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801da8h-sound-ram-data-transfer-fifo
	// Actual hardware would write into a FIFO, but don't think that needs to be emulated (at this point).
	if (g_logSPUDataTransfer) // On a different flag because BIOS hammers this during startup
		LOG_INFO("[SPU] Write SPU RAM write address %08X value %08X \n", m_currentSoundRAMDataTransferAddress, val);

	HP_ASSERT(m_currentSoundRAMDataTransferAddress + 4 <= kRamSizeBytes); // #TODO: Wrap?
	*(u32*)(m_ram + m_currentSoundRAMDataTransferAddress) = val; // assumes host is little endian

	// Check IRQ9
	// Data Transfers (usually via DMA4) to/from SPU-RAM do also trap SPU interrupts.
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == m_currentSoundRAMDataTransferAddress)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 32-bit write to address %08X\n", m_soundRAMIRQAddress);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}

	// n.b. This internal register is incremented, but 1F801DA6h is not.
	m_currentSoundRAMDataTransferAddress += 4;
}

void SPU::WriteDataBlock(const u8* data, unsigned int numWords)
{
	unsigned int numBytes = numWords * 4;

	if (g_logSPUDataTransfer) // On a different flag because BIOS hammers this during startup
		LOG_INFO("[SPU] Receive DMA block size %08X bytes to SPU RAM address %08X\n", numBytes, m_currentSoundRAMDataTransferAddress);

	if (m_currentSoundRAMDataTransferAddress + numBytes <= kRamSizeBytes)
	{
		memcpy(m_ram + m_currentSoundRAMDataTransferAddress, data, numBytes);

		// Check IRQ9
		// Data Transfers (usually via DMA4) to/from SPU-RAM do also trap SPU interrupts.
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
		// IRQ9 does not trigger SPUSTAT bit 6 is not set.
		if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress >= m_currentSoundRAMDataTransferAddress && m_soundRAMIRQAddress < (m_currentSoundRAMDataTransferAddress + numBytes))
		{
			if (g_logSPU)
				LOG_INFO("[SPU] IRQ9 triggered by DMA write to address %08X\n", m_soundRAMIRQAddress);
			m_intc.SetIRQ(IRQ::IRQ9_SPU);
			m_stat.irq = 1;
		}

		// n.b. This internal register is incremented, but 1F801DA6h is not.
		m_currentSoundRAMDataTransferAddress += numBytes;
		if (m_currentSoundRAMDataTransferAddress == kRamSizeBytes)
		{
			// Wrap around to the beginning of RAM
			m_currentSoundRAMDataTransferAddress = 0;
		}
	}
	else
	{
		// Writing past the end of RAM. Wrap and copy in two blocks.
		// Note: This will stomp capture buffers, which are located at the beginning of SPU RAM and is probably a software bug.

		unsigned int bytesToEndOfRAM = kRamSizeBytes - m_currentSoundRAMDataTransferAddress;
		memcpy(m_ram + m_currentSoundRAMDataTransferAddress, data, bytesToEndOfRAM);

		unsigned int remainingBytes = numBytes - bytesToEndOfRAM;
		HP_ASSERT(remainingBytes <= kRamSizeBytes);
		memcpy(m_ram, data + bytesToEndOfRAM, remainingBytes);

		// Check IRQ9
		if (m_spucnt.IRQ9Enable)
		{
			if ((m_soundRAMIRQAddress >= m_currentSoundRAMDataTransferAddress && m_soundRAMIRQAddress < kRamSizeBytes)
				|| (m_soundRAMIRQAddress < remainingBytes))
			{
				if (g_logSPU)
					LOG_INFO("[SPU] IRQ9 triggered by DMA write to address %08X\n", m_soundRAMIRQAddress);
				m_intc.SetIRQ(IRQ::IRQ9_SPU);
				m_stat.irq = 1;
			}
		}

		m_currentSoundRAMDataTransferAddress = remainingBytes;
	}
}

void SPU::ReadDataBlock(u8 * dst, unsigned int numWords)
{
	unsigned int numBytes = numWords * 4;

	if (g_logSPUDataTransfer) // On a different flag because BIOS hammers this during startup
		LOG_INFO("[SPU] Read DMA block size %08X bytes to SPU RAM address %08X\n", numBytes, m_currentSoundRAMDataTransferAddress);

	HP_ASSERT(m_currentSoundRAMDataTransferAddress + numBytes <= kRamSizeBytes, "#TODO: Support wrapping split reads, see WriteDataBlock()");

	memcpy(dst, m_ram + m_currentSoundRAMDataTransferAddress, numBytes);

	// Check IRQ9
	// Data Transfers (usually via DMA4) to/from SPU-RAM do also trap SPU interrupts.
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress >= m_currentSoundRAMDataTransferAddress && m_soundRAMIRQAddress < (m_currentSoundRAMDataTransferAddress + numBytes))
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by DMA read from address %08X\n", m_soundRAMIRQAddress);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}

	// n.b. This internal register is incremented, but 1F801DA6h is not.
	m_currentSoundRAMDataTransferAddress += numBytes;
	if (m_currentSoundRAMDataTransferAddress == kRamSizeBytes)
	{
		// Wrap around to the beginning of RAM
		m_currentSoundRAMDataTransferAddress = 0;
	}
}

[[maybe_unused]]
static void generateTestTone(s16 frame[2])
{
	// #TEST: Play 440 Hz square wave
	float frequency = 440.0f; // periods per second
	unsigned int samplesPerPeriod = (unsigned int)(SPU::kOutputSampleRate / frequency); // (samples / second) / (period / second) = samples / period
	static unsigned int s_frameCount = 0; // total audio frames generated

	s16 val = (s_frameCount % samplesPerPeriod) < (samplesPerPeriod / 2) ? INT16_MIN : INT16_MAX;
	s_frameCount++;

	frame[0] = val;
	frame[1] = val;
}

// From https://jsgroth.dev/blog/posts/ps1-spu-part-2/#volume-multipliers
// #TODO: Replace with more accurate implementation
static inline s16 applyVolume(s16 sample, s16 volume)
{
	s32 result = ((s32)sample * (s32)volume) >> 15;
	HP_DEBUG_ASSERT(result >= INT16_MIN && result <= INT16_MAX); // overflow
	return (s16)result;
}

static void applyVoiceVolume(const SPU::Voice& voice, s16 sample, s16& outputL, s16& outputR)
{
	// Apply ADSR envelope first
//	HP_DEBUG_ASSERT(voice.currentADSREnvelopeLevel >= 0); // #TODO: Why is this assert here? Think negative volumes are fine - they just invert the phase.
	s16 envelopeSample = applyVolume(sample, voice.currentADSREnvelopeLevel);

	// Apply L/R volumes second
	HP_DEBUG_ASSERT((voice.volumeLeft & 0x8000) == 0 && (voice.volumeRight & 0x8000) == 0, "Volume sweep not implemented");

	// In volume mode 0, the signed volume is stored in bits 14:0 (negative sign is used to invert the phase)
	u16 volumeLeft = voice.volumeLeft << 1;
	u16 volumeRight = voice.volumeRight << 1;

	outputL = applyVolume(envelopeSample, volumeLeft);
	outputR = applyVolume(envelopeSample, volumeRight);
}

// Decodes an ADPCM compressed 16-byte block to 28 16-bit PCM samples
// 
// ADPCM = adaptive differential pulse-code modulation.
// Samples encoded as differences relative to previous samples.
//
// Format: 2 byte header + 14 data bytes (28 nybbles) of compressed 4-bit samples.
//
// First header byte contains shift and filter values
// Second header byte contains loop flags.
//
// https://jsgroth.dev/blog/posts/ps1-spu-part-1/#adpcm
//
static void decodeADPCMBlock(const u8 block[16], /*out*/s16 decoded[16], s16 decodedPrev, s16 decodedPrevPrev)
{
	unsigned int shift = block[0] & 0x0f;
	if (shift > 12) // shift [13,15] are equivalent to 9
		shift = 9;

	unsigned int filter = Min((u32)block[0] >> 4, 4u); // [0,4]

	// Decode the 28 sample nybbles
	for (unsigned int sampleIndex = 0; sampleIndex < 28; sampleIndex++)
	{
		u8 sample_byte = block[2 + sampleIndex / 2];
		u8 sample_nibble = (sample_byte >> (4 * (sampleIndex & 1))) & 0x0F; // little endian within byte

		// Sign extend from 4 bits to 32 bits
		s32 raw_sample = (s8)(sample_nibble << 4) >> 4; // n.b. The (s8) cast is extremely important because << promotes to 32-bit integer

		// Apply the shift; a shift value of N is decoded by shifting left (12 - N)
		s32 shifted_sample = raw_sample << (12 - shift);

		// Apply the filter formula.
		// In real code you can do this with tables instead of a match
		s32 old = (s32)decodedPrev;
		s32 older = (s32)decodedPrevPrev;
		s32 filtered_sample;
		switch (filter)
		{
			case 0: // No filtering
				filtered_sample = shifted_sample;
				break;
			case 1: // Filter using previous sample
				filtered_sample = shifted_sample + (60 * old + 32) / 64;
				break;

			// 2-4: Filter using previous 2 samples
			case 2:
				filtered_sample = shifted_sample + (115 * old - 52 * older + 32) / 64;
				break;
			case 3:
				filtered_sample = shifted_sample + (98 * old - 55 * older + 32) / 64;
				break;
			/*case 4:*/default: // warning C4701: potentially uninitialized local variable 'filtered_sample' used. But we know it is clamped to [0,4]
				filtered_sample = shifted_sample + (122 * old - 60 * older + 32) / 64;
				break;
		}

		// Finally, clamp to signed 16-bit
		s16 clamped_sample = (s16)Clamp(filtered_sample, -0x8000, 0x7FFF);
		decoded[sampleIndex] = clamped_sample;

		// Update sliding window for filter
		decodedPrevPrev = decodedPrev;
		decodedPrev = clamped_sample;
	}
}

void SPU::scheduleClockCallback()
{
	// SPU clock is 44100 Hz, which is exactly CPU clock / 768
	m_scheduler.Schedule(clockCallback, this, 768, "SPU Clock");
}

// static
void SPU::clockCallback(void* userdata)
{
	SPU* pSPU = (SPU*)userdata;
	pSPU->Clock();

	// reschedule
	pSPU->scheduleClockCallback();
}

void SPU::writeRAM16(u32 address, u16 val)
{
	HP_DEBUG_ASSERT((address & 1) == 0); // #TODO: Just mask off LSb?
	HP_DEBUG_ASSERT(address < kRamSizeBytes); // #TODO: Just mask/wrap?
	*(u16*)(m_ram + address) = val; // assumes host is little endian

	// Check IRQ9
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-interrupt
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == address)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 16-bit write to address %08X\n", m_soundRAMIRQAddress);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}
}

// Called at 44100 Hz
void SPU::Clock()
{
	updateNoise();
	clockVoices();

	s16 cdSamples[2]{};
	readCDDAudioSamples(cdSamples);

	// The CD Audio capture buffers store the most recent CDDA or CDXA samples, before volume processing.
	updateCaptureBuffers(cdSamples);

	s16 finalOutput[2]{}; // stereo
	
	static constexpr bool s_testTone = false;
	if (s_testTone)
		generateTestTone(finalOutput);
	else
	{
		if (m_spucnt.MuteSPU == 1) // n.b. active low: 0 = mute, 1 = not mute n.b. This should not affect CDROM audio
		{
			s16 spuSamples[2]{};
			s32 reverbSamples[2]{};
			sample(spuSamples, reverbSamples);

			s16 reverbOutput[2]{};
			updateReverb(reverbSamples, cdSamples, reverbOutput);
			if (m_debugDisableReverb)
			{
				reverbOutput[0] = 0;
				reverbOutput[1] = 0;
			}
			mix(spuSamples, cdSamples, reverbOutput, finalOutput);
		}
	}

	if (m_pAudioFrameCallback)
		m_pAudioFrameCallback(finalOutput);

	if (s_pOutputFile)
	{
		fwrite((const void*)&finalOutput[0], 2, 1, s_pOutputFile); // left
		fwrite((const void*)&finalOutput[1], 2, 1, s_pOutputFile); // right
	}
}

//
// https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-noise-generator
//
void SPU::updateNoise()
{
	int step = 4 + m_spucnt.NoiseFrequencyStep; // 0..03h = Step "4,5,6,7"
	m_noiseTimer -= step;

	if (m_noiseTimer > 0)
		return;

	// LFSR
	// ParityBit = NoiseLevel.Bit15 xor Bit12 xor Bit11 xor Bit10 xor 1
	unsigned int parityBit = ((m_noiseLevel >> 15) ^ (m_noiseLevel >> 12) ^ (m_noiseLevel >> 11) ^ (m_noiseLevel >> 10) ^ 1) & 1;
	m_noiseLevel = (m_noiseLevel << 1) | (u16)parityBit;

	m_noiseTimer += 0x20000 >> m_spucnt.NoiseFrequencyShift;
	if (m_noiseTimer < 0)
		m_noiseTimer += 0x20000 >> m_spucnt.NoiseFrequencyShift;
}

void SPU::clockVoices()
{
	// #TODO: How to know which voices to update?
	// #TODO: Consider scheduling voice updates based on pitch
	for (unsigned int voiceIndex = 0; voiceIndex < kVoiceCount; voiceIndex++)
	{
		Voice& voice = m_voices[voiceIndex];
		if (voice.adsr.phase == ADSRPhase::None)
			continue;

		const u32 voiceFlag = 1 << voiceIndex;

		// Update pitch counter, including pitch modulation
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#pitch-counter
		s32 step = voice.sampleRate;

		// Pitch modulation
		// The previous voice's output modulates this voice's pitch, so modulation only applies to voices 1+.
		if (voiceIndex > 0 && (m_voiceFlags.pmon & voiceFlag))
		{
			const Voice& voicePrev = m_voices[voiceIndex - 1];

			// Sample from after ADSR envelope volume is applied but before voice L/R volume is applied.
			s32 factor = applyVolume(voicePrev.sample, voicePrev.currentADSREnvelopeLevel); // [-8000,7FFF]
			factor += 0x8000; // [0,FFFF]

			step = (s32)(s16)step; // SignExpand16to32. hardware glitch on VxPitch>7FFFh, make signed

			step = (step * factor) >> 15; // range 0..1FFFFh (glitchy if VxPitch>7FFFh)
//			HP_DEBUG_ASSERT(step >= 0 && step <= 0x1ffff); // disabled because could fire due to glitch

			step &= 0xffff; // hardware glitch on VxPitch>7FFFh, kill sign
		}

		if (step > 0x3fff) // documented as IF Step>3FFFh then Step=4000h
			step = 0x3fff;

		voice.pitchCounter += step;

		// Pitch counter bits 15:12 advance through samples.
		while (voice.pitchCounter >= 0x1000)
		{
			voice.pitchCounter -= 0x1000;
			voice.currentSampleIndex++;

			if (voice.currentSampleIndex == kADPCMSamplesPerBlock)
			{
				voice.currentSampleIndex = 0;
				decodeVoiceBlock(voice, voiceIndex);
			}

			// Advance the history for Gaussian interpolation
			voice.sampleHistory[3] = voice.sampleHistory[2];
			voice.sampleHistory[2] = voice.sampleHistory[1];
			voice.sampleHistory[1] = voice.sampleHistory[0];
			voice.sampleHistory[0] = voice.decodedSamples[voice.currentSampleIndex];
		}

		// Pitch counter bits 11:4 (8 bits) are used to interpolate between samples.
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#4-point-gaussian-interpolation
		// The right shifts by 15 are because each multiplier N represents (N / 0x8000)
		unsigned int i = (voice.pitchCounter >> 4) & 0xff;
		s32 sample = 0;
		sample += ((s32)kGaussianInterpolationTable[0x0ff - i] * (s32)voice.sampleHistory[3]) >> 15; // oldest
		sample += ((s32)kGaussianInterpolationTable[0x1ff - i] * (s32)voice.sampleHistory[2]) >> 15; // older
		sample += ((s32)kGaussianInterpolationTable[0x100 + i] * (s32)voice.sampleHistory[1]) >> 15; // old
		sample += ((s32)kGaussianInterpolationTable[0x000 + i] * (s32)voice.sampleHistory[0]) >> 15; // new
		voice.sample = (s16)sample;

		if (m_voiceFlags.non & voiceFlag)
		{
			// Noise mode - replace sample with noise
			voice.sample = (s16)m_noiseLevel;
		}

		clockVoiceADSREnvelope(voice);
	}
}

//
// This function does not apply volumes so result can be fed into CD Audio capture buffer which is before volume processing.
//
void SPU::readCDDAudioSamples(s16 cdSamples[2])
{
	// If there is data in the CDROM CDDA buffer, then use that too.
	RingBuffer& cddaBuffer = m_cdrom.GetCDDABuffer();
	RingBuffer& cdxaBuffer = m_cdrom.GetCDXABuffer();

	cdSamples[0] = 0;
	cdSamples[1] = 0;
	if (cddaBuffer.GetUsedSpaceBytes() >= 2 * sizeof(s16))
	{
		u32 dat;
		cddaBuffer.Read((u8*)&dat, 4);

		// #TODO: Confirm endianness here.
		cdSamples[0] = (s16)(dat & 0xffff); // L
		cdSamples[1] = (s16)(dat >> 16); // R
	}
	else if (cdxaBuffer.GetUsedSpaceBytes() >= 2 * sizeof(s16))
	{
		u32 dat;
		cdxaBuffer.Read((u8*)&dat, 4);

		if (!m_cdrom.IsXAMuted())
		{
			cdSamples[0] = (s16)(dat & 0xffff); // L
			cdSamples[1] = (s16)(dat >> 16); // R
		}
	}
}

//
//  00000h-003FFh  CD Audio left  (1Kbyte) ;\CD Audio before Volume processing
//  00400h-007FFh  CD Audio right (1Kbyte) ;/signed 16bit samples at 44.1kHz
//  00800h-00BFFh  Voice 1 mono   (1Kbyte) ;\Voice 1 and 3 after ADSR processing
//  00C00h-00FFFh  Voice 3 mono   (1Kbyte) ;/signed 16bit samples at 44.1kHz
// 
// https://jsgroth.dev/blog/posts/ps1-spu-part-4/#capture-buffers
//
void SPU::updateCaptureBuffers(const s16 cdSamples[2])
{
	// The CD Audio capture buffers store the most recent CDDA or CDXA samples, before volume processing.
	// If no CD audio is playing then zeroes are written to the CD capture buffers.

	writeRAM16(0x0000 + m_captureBufferWriteOffset, cdSamples[0]); // L
	writeRAM16(0x0400 + m_captureBufferWriteOffset, cdSamples[1]); // R

	// Voice 1 and 3 mono. Voice data is just a mono value and only the L/R volumes make it stereo.
	// Voice samples are from after ADSR envelope volume is applied but before voice L/R volumes are applied.
	s16 voice1mono = applyVolume(m_voices[1].sample, m_voices[1].currentADSREnvelopeLevel);
	s16 voice3mono = applyVolume(m_voices[3].sample, m_voices[3].currentADSREnvelopeLevel);
	writeRAM16(0x0800 + m_captureBufferWriteOffset, voice1mono);
	writeRAM16(0x0C00 + m_captureBufferWriteOffset, voice3mono);

	m_captureBufferWriteOffset += 2; // 16-bits per sample
	if (m_captureBufferWriteOffset >= 0x400) // 1 KiB per buffer
		m_captureBufferWriteOffset = 0;

	m_stat.captureBufferHalf = m_captureBufferWriteOffset >= 0x200;
}

void SPU::sample(s16 spuSamples[2], s32 reverbSamples[2]) const
{
	s32 mixedLeft = 0;
	s32 mixedRight = 0;

	reverbSamples[0] = 0;
	reverbSamples[1] = 0;

	for (unsigned int voiceIndex = 0; voiceIndex < kVoiceCount; voiceIndex++)
	{
		const Voice& voice = m_voices[voiceIndex];

		if (voice.adsr.phase == ADSRPhase::None)
			continue;

		s16 voiceLeft, voiceRight;
		applyVoiceVolume(voice, voice.sample, voiceLeft, voiceRight);

		if ((m_debugVoiceEnabled & (1 << voiceIndex)) == 0)
			continue;

		mixedLeft += (s32)voiceLeft;
		mixedRight += (s32)voiceRight;

		// Sum reverb contributions
		// Reverb uses the voice samples after the voice L/R volumes are applied but before the main L/R volumes are applied.
		// #TODO: Should SPCNT Reverb Master Enable flag be respected here?
		if (m_voiceFlags.eon & (1 << voiceIndex))
		{
			reverbSamples[0] += (s32)voiceLeft;
			reverbSamples[1] += (s32)voiceRight;
		}
	}

	// Clamp mixed output to 16 bits
	s16 clampedLeft = (s16)Clamp(mixedLeft, (s32)INT16_MIN, (s32)INT16_MAX);
	s16 clampedRight = (s16)Clamp(mixedRight, (s32)INT16_MIN, (s32)INT16_MAX);

	HP_DEBUG_ASSERT((m_mainVolumeLeft & 0x8000) == 0 && (m_mainVolumeRight & 0x8000) == 0, "Volume sweep not implemented");
	s16 outputLeft = applyVolume(clampedLeft, m_mainVolumeLeft);
	s16 outputRight = applyVolume(clampedRight, m_mainVolumeRight);

	spuSamples[0] = outputLeft;
	spuSamples[1] = outputRight;
}

void SPU::mix(const s16 spuSamples[2], const s16 cdSamples[2], const s16 reverbOutput[2], s16 mixedSamples[2]) const
{
	// Apply CD volume matrix
	s32 cdSumL = ((s32)cdSamples[0] * m_cdrom.GetCDLeftLeftVolume()   + (s32)cdSamples[1] * m_cdrom.GetCDRightLeftVolume()) >> 8;
	s32 cdSumR = ((s32)cdSamples[1] * m_cdrom.GetCDRightRightVolume() + (s32)cdSamples[0] * m_cdrom.GetCDLeftRightVolume()) >> 8;

	// Saturate. See https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-write-bank-3-atv3-r-l-volume
	s16 cdLeft = (s16)Clamp(cdSumL, (s32)INT16_MIN, (s32)INT16_MAX);
	s16 cdRight = (s16)Clamp(cdSumR, (s32)INT16_MIN, (s32)INT16_MAX);

	s32 l = (s32)spuSamples[0] + (s32)cdLeft + (s32)reverbOutput[0];
	s32 r = (s32)spuSamples[1] + (s32)cdRight + (s32)reverbOutput[1];

	mixedSamples[0] = (s16)Clamp(l, (s32)INT16_MIN, (s32)INT16_MAX);
	mixedSamples[1] = (s16)Clamp(r, (s32)INT16_MIN, (s32)INT16_MAX);

	if (s_pCDAudioFile)
	{
		fwrite((const void*)&cdLeft, 2, 1, s_pCDAudioFile);
		fwrite((const void*)&cdRight, 2, 1, s_pCDAudioFile);
	}
}

//
// Start ADSR envelope
//
// Implementation note
//
void SPU::keyOn(u32 voiceBits)
{
	HP_DEBUG_ASSERT((voiceBits & 0xff00'0000) == 0, "There are only 24 voices");

	while (voiceBits) // n.b. Count trailing zeros intrinsics not defined for zero
	{
		u32 voiceIndex = COUNT_TRAILING_ZEROS(voiceBits);
		voiceBits &= (voiceBits - 1); // resetLowestSetBit();
		if (g_logSPU)
			LOG_INFO("[SPU] Key On (KON) voice %u\n", voiceIndex);

		Voice& voice = m_voices[voiceIndex];

		voice.currentADPCMAddress = voice.startAddressDiv8 << 3; // voice register stores address/8

		decodeVoiceBlock(voice, voiceIndex);

		// Reset voice ADPCM decoding state
		voice.currentSampleIndex = 0;

		voice.sample = voice.decodedSamples[voice.currentSampleIndex];

		// Clear history for Gaussian interpolation
		voice.sampleHistory[0] = voice.sample;
		voice.sampleHistory[1] = 0;
		voice.sampleHistory[2] = 0;
		voice.sampleHistory[3] = 0;

		voice.pitchCounter = 0;

		// Reset voice envelope state
		transitionVoiceToAttack(voice);

		// Clear bit in ENDX https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d9ch-voice-023-onoff-status-endx-r
		m_voiceFlags.endx &= ~(1 << voiceIndex);
	}
}

//
// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d8ch-voice-023-key-off-start-release-koff-w
//
void SPU::keyOff(u32 voiceBits)
{
#if 0
	// Can't use this assert: BIOS writes 0xffff to 1F801D8Eh (KOFF high 16-bits)
	HP_DEBUG_ASSERT((voiceBits & 0xff00'0000) == 0, "There are only 24 voices");
#else
	voiceBits &= 0x00ffffff; // max 24 voices
#endif

	while (voiceBits) // n.b. Count trailing zeros intrinsics not defined for zero
	{
		u32 voiceIndex = COUNT_TRAILING_ZEROS(voiceBits);
		voiceBits &= (voiceBits - 1); // resetLowestSetBit();
		if (g_logSPU)
			LOG_INFO("[SPU] Key Off (KOFF) voice %u\n", voiceIndex);

		Voice& voice = m_voices[voiceIndex];
		transitionVoiceToRelease(voice);
	}
}

void SPU::decodeVoiceBlock(Voice& voice, unsigned int voiceIndex)
{
	// Decode the first 16-byte / 28-sample block at the new current address
	HP_ASSERT(voice.currentADPCMAddress + kADPCMEncodedBlockSizeBytes <= kRamSizeBytes);
	const u32 blockAddress = voice.currentADPCMAddress;
	const u8* pEncodedBlock = m_ram + blockAddress;
	s16 decodedSamplePrev = voice.decodedSamples[kADPCMSamplesPerBlock - 1];
	s16 decodedSamplePrevPrev = voice.decodedSamples[kADPCMSamplesPerBlock - 2];
	decodeADPCMBlock(pEncodedBlock, voice.decodedSamples, decodedSamplePrev, decodedSamplePrevPrev);

	// Respect second header byte (block[1]) loop flags
	if (pEncodedBlock[1] & 4) // loop start?
		voice.repeatAddressDiv8 = (u16)(blockAddress >> 3); // register stores address / 8

	if (pEncodedBlock[1] & 1) // Bit 0: Loop end
	{
		// Set ENDX flag and Jump to ADPCM repeat address
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#flag-bits-in-2nd-byte-of-adpcm-header
		m_voiceFlags.endx |= (1 << voiceIndex);
		voice.currentADPCMAddress = voice.repeatAddressDiv8 << 3;

		// bit 1: Loop Repeat. If 0 (and bit 0 set) then force Release and set ADSR Level to zero
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#flag-bits-in-2nd-byte-of-adpcm-header
		bool loopRepeat = pEncodedBlock[1] & 2;
		if (!loopRepeat)
		{
			transitionVoiceToRelease(voice);
			voice.currentADSREnvelopeLevel = 0;
		}
	}
	else
		voice.currentADPCMAddress += kADPCMEncodedBlockSizeBytes; // next block

	// Check IRQ9
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	// Implementation note: Could check IRQ9 in wrapped SPU RAM read/write functions, but more efficient to do it like this for voices.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress >= blockAddress && m_soundRAMIRQAddress < blockAddress + kADPCMEncodedBlockSizeBytes)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by voice %u reaching address %08X\n", voiceIndex, m_soundRAMIRQAddress);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}
}

void SPU::transitionVoiceToAttack(Voice& voice)
{
	Voice::ADSR& adsr = voice.adsr;
	adsr.phase = ADSRPhase::Attack;
	adsr.decreasing = false; // Attack is always increasing.
	adsr.exponential = voice.attackDecaySustainLevel.attackMode == 1;
	adsr.phaseNegative = false; // #TODO: Where does phaseNegative come from?
	voice.currentADSREnvelopeLevel = 0;
	transitionVoiceToADSRPhase(voice, voice.attackDecaySustainLevel.attackStep, voice.attackDecaySustainLevel.attackShift, Voice::kMaxAttackShift);
}

void SPU::transitionVoiceToDecay(Voice& voice)
{
	Voice::ADSR& adsr = voice.adsr;
	adsr.phase = ADSRPhase::Decay;
	constexpr unsigned int stepValue = 0; // Not configurable. In decreasing mode, 0 will map to -8
	adsr.decreasing = true; // Decay is always decreasing.
	adsr.exponential = true; // Decay mode is not configurable, it is always exponential.
	adsr.phaseNegative = false; // #TODO: Where does phaseNegative come from?
	voice.currentADSREnvelopeLevel = 0x7fff; // Attack always increases to 7FFFh, so decay should start there.
	transitionVoiceToADSRPhase(voice, stepValue, voice.attackDecaySustainLevel.decayShift, Voice::kMaxDecayShift);
}

void SPU::transitionVoiceToSustain(Voice& voice)
{
	Voice::ADSR& adsr = voice.adsr;
	adsr.phase = ADSRPhase::Sustain;
	adsr.decreasing = voice.sustainReleaseRate.sustainDirection == 1;
	adsr.exponential = voice.sustainReleaseRate.sustainMode == 1;
	adsr.phaseNegative = false; // #TODO: Where does phaseNegative come from?

#if 0
	voice.currentADSREnvelopeLevel = (voice.attackDecaySustainLevel.sustainLevel + 1) << 11; // Snap to sustain level, where Level=(N+1)*800h
#else
	// Don't set level - it will take current value.
#endif

	transitionVoiceToADSRPhase(voice, voice.sustainReleaseRate.sustainStep, voice.sustainReleaseRate.sustainShift, Voice::kMaxSustainShift);
}

void SPU::transitionVoiceToRelease(Voice& voice)
{
	Voice::ADSR& adsr = voice.adsr;
	adsr.phase = ADSRPhase::Release;
	adsr.decreasing = true; // Release direction is not configurable, it always decreases
	constexpr unsigned int stepValue = 0; // Not configurable. In decreasing mode, 0 will map to -8
	adsr.exponential = voice.sustainReleaseRate.releaseMode == 1;
	adsr.phaseNegative = false; // #TODO: Where does phaseNegative come from?
	// Don't set level - it will take current value.
	transitionVoiceToADSRPhase(voice, stepValue, voice.sustainReleaseRate.releaseShift, Voice::kMaxReleaseShift);
}

// Precalculate ADSR state
// https://psx-spx.consoledev.net/soundprocessingunitspu/#envelope-operation-depending-on-shiftstepmodedirection
//
// #TODO: Modify/remove this to allow parameters to be changed dynamically. See https://jsgroth.dev/blog/posts/ps1-spu-part-2/ "Final Fantasy VII"
//
void SPU::transitionVoiceToADSRPhase(Voice& voice, unsigned int stepValue, unsigned int shift, unsigned int maxShift)
{
	Voice::ADSR& adsr = voice.adsr;

	HP_DEBUG_ASSERT(stepValue < 4); // stepValue is a 2-bit value

	int step = 7 - stepValue;
	if (adsr.decreasing)
		step = ~step;// +7,+6,+5,+4 => -8,-7,-6,-5

	HP_DEBUG_ASSERT(shift <= maxShift); // A, S and R have 5-bit shift value; D has only 4 bits.

	/*
	; Precalculation, can be cached on phase begin.
	AdsrStep = 7 - StepValue
	IF Decreasing XOR PhaseNegative THEN
	  AdsrStep = NOT AdsrStep ; +7,+6,+5,+4 => -8,-7,-6,-5
	AdsrStep = AdsrStep SHL Max(0,11-ShiftValue)
	CounterIncrement = 8000h SHR Max(0,ShiftValue-11)
	IF exponential AND increase AND AdsrLevel>6000h THEN
	  IF ShiftValue < 10 THEN
		AdsrStep /= 4 ; SHR 2
	  ELSE IF ShiftValue >= 11 THEN
		CounterIncrement /= 4 ; SHR 2
	  ELSE
		AdsrStep /= 4 ; SHR 2
		CounterIncrement /= 4 ; SHR 2
	ELSE IF exponential AND decrease THEN
	  AdsrStep=AdsrStep*AdsrLevel/8000h
	
	IF (StepValue | (ShiftValue SHL 2)) != ALL_BITS THEN
	  CounterIncrement = MAX(CounterIncrement, 1)
	*/

#if 0
	adsr.step = step << (Max(0, 11 - (int)shift)); // shifting a negative value is undefined behaviour
#else
	adsr.step = step * (1 << Max(0, 11 - (int)shift));
#endif 
	adsr.counterIncrement = 0x8000 >> Max(0, (int)shift - 11);
	if (adsr.exponential && !adsr.decreasing && voice.currentADSREnvelopeLevel > 0x6000)
	{
		if (shift < 10)
			adsr.step >>= 2;
		else if (shift >= 11)
			adsr.counterIncrement >>= 2;
		else
		{
			adsr.step >>= 2;
			adsr.counterIncrement >>= 2;
		}
	}
#if 0 // Moved into clockVoiceADSREnvelope, which is essential for exponential decay
	else if (adsr.exponential && adsr.decreasing)
	{
		adsr.step = (adsr.step * voice.currentADSREnvelopeLevel) >> 15; // divide by 8000h
	}
#endif

	unsigned int bits = (shift << 2) | stepValue;
	unsigned int allBits = (maxShift << 2) | 0b11; // combine 4/5 bit and 2 bit values
	if (bits != allBits)
		adsr.counterIncrement = Max(adsr.counterIncrement, 1u);

	adsr.counter = 0;
}

//
// Call once per 44100 Hz clock to update voice ADSR state machine and level.
//
// See // https://psx-spx.consoledev.net/soundprocessingunitspu/#envelope-operation-depending-on-shiftstepmodedirection
//
void SPU::clockVoiceADSREnvelope(Voice& voice)
{
	Voice::ADSR& adsr = voice.adsr;

	adsr.counter += adsr.counterIncrement;
	if ((adsr.counter & 0x8000) == 0)
		return; // No step this cycle.

	adsr.counter = 0;

	int step = adsr.step;
	if (adsr.exponential && adsr.decreasing)
		step = (step * voice.currentADSREnvelopeLevel) >> 15; // divide by 8000h

	// Saturate depending on mode.
	int level = (int)voice.currentADSREnvelopeLevel + step;
	if (!adsr.decreasing)
		level = Clamp(level, -0x8000, 0x7fff);
	else if (adsr.phaseNegative)
		level = Clamp(level, -0x8000, 0);
	else
	{
#if 0
		HP_ASSERT(level <= INT16_MAX, "Think this should be clamped");
		level = Max(level, 0); // #TODO: Should this be a Clamp
#else
		level = Clamp(level, 0, (int)INT16_MAX);
#endif
	}

	voice.currentADSREnvelopeLevel = (s16)level;

	switch (adsr.phase)
	{
		case ADSRPhase::Attack:
		{
			if (level == 0x7fff)
				transitionVoiceToDecay(voice);
			break;
		}
		case ADSRPhase::Decay:
		{
			u16 sustainLevel = (voice.attackDecaySustainLevel.sustainLevel + 1) << 11; // Level=(N+1)*800h  #TODO[#opt]: Cache this value
			if (level <= sustainLevel)
				transitionVoiceToSustain(voice);
			break;
		}
		case ADSRPhase::Sustain:
			break;
		case ADSRPhase::Release:
		{
			if (level == 0)
			{
				adsr.phase = ADSRPhase::None;
			}
			break;
		}
		case ADSRPhase::None:
			break;
	}
}

const SPU::Voice& SPU::GetVoice(unsigned int voiceIndex) const
{
	HP_DEBUG_ASSERT(voiceIndex < kVoiceCount);
	return m_voices[voiceIndex];
}

//-----------------------------------------------------------------------------------------------------
// Reverb
//-----------------------------------------------------------------------------------------------------

//
// addressDiv8 is relative to the current buffer (head) address
//
u32 SPU::reverbAddrToRamAddr(u16 addressDiv8) const
{
	u32 offset = m_currentReverbBufferHead + (addressDiv8 * 8u); // relative to current buffer address
	HP_DEBUG_ASSERT(offset >= m_reverbBufferStartAddress);
	offset -= m_reverbBufferStartAddress;
	if (m_reverbBufferSizeBytes > 0)
		offset = offset % m_reverbBufferSizeBytes; // Reverb buffer wraps around
	u32 addr = m_reverbBufferStartAddress + offset;
	HP_DEBUG_ASSERT(addr >= m_reverbBufferStartAddress && addr < SPU::kRamSizeBytes); // The PSX SPU reverb buffer always extends to the end of RAM.
	return addr;
}

s16 SPU::readSampleFromReverbBuffer(u16 addressDiv8)
{
	u32 address = reverbAddrToRamAddr(addressDiv8);
	HP_ASSERT(address >= m_reverbBufferStartAddress && address + 2 <= SPU::kRamSizeBytes);
	s16 val = *(s16*)(m_ram + address);

	// Check IRQ9
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == address)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 16-bit reverb buffer sample read from address %08X\n", address);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}

	return val;
}


void SPU::writeSampleToReverbBuffer(u16 addressDiv8, s16 val)
{
	u32 address = reverbAddrToRamAddr(addressDiv8);
	HP_ASSERT(address >= m_reverbBufferStartAddress && address + 2 <= SPU::kRamSizeBytes);
	u16* pDst = (u16*)(m_ram + address);

	// #TODO: When the SPUCNT Reverb Master Enable flag is cleared, the SPU does not write any data to the Reverb buffer. m_spucnt.ReverbMasterEnable
	//        Does this mean it writes zeroes, or it just doesn't update the buffer at all?
	if (!m_spucnt.ReverbMasterEnable)
		val = 0;

	*pDst = val;

	// Check IRQ9
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == address)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 16-bit reverb buffer sample write to address %08X\n", address);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}
}

s16 SPU::readReverbSampleFromRAM(u32 address)
{
	HP_ASSERT(address + 2 <= SPU::kRamSizeBytes);

	// For the purposes of this application, we only expect to read from the referby buffer.
	HP_ASSERT(address >= m_reverbBufferStartAddress && address + 2 <= SPU::kRamSizeBytes);

	s16 val = *(s16*)(m_ram + address);

	// Check IRQ9
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == address)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 16-bit reverb buffer sample read from address %08X\n", address);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}

	return val;
}

void SPU::writeReverbSampleToRAM(u32 address, s16 val)
{
	HP_ASSERT(address + 2 <= SPU::kRamSizeBytes);

	// For the purposes of this application, we only expect to read from the referby buffer.
	HP_ASSERT(address >= m_reverbBufferStartAddress && address + 2 <= SPU::kRamSizeBytes);

	u16* pDst = (u16*)(m_ram + address);

	// #TODO: When the SPUCNT Reverb Master Enable flag is cleared, the SPU does not write any data to the Reverb buffer. m_spucnt.ReverbMasterEnable
	//        Does this mean it writes zeroes, or it just doesn't update the buffer at all?
	if (!m_spucnt.ReverbMasterEnable)
		val = 0;

	*pDst = val;

	// Check IRQ9
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#data-transfers
	// IRQ9 does not trigger SPUSTAT bit 6 is not set.
	if (m_spucnt.IRQ9Enable && m_soundRAMIRQAddress == address)
	{
		if (g_logSPU)
			LOG_INFO("[SPU] IRQ9 triggered by 16-bit reverb buffer sample write to address %08X\n", address);
		m_intc.SetIRQ(IRQ::IRQ9_SPU);
		m_stat.irq = 1;
	}
}

/*

Reverb Formula
==============

  ___Input from Mixer (Input volume multiplied with incoming data)_____________
  Lin = vLIN * LeftInput    ;from any channels that have Reverb enabled
  Rin = vRIN * RightInput   ;from any channels that have Reverb enabled
  ____Same Side Reflection (left-to-left and right-to-right)___________________
  [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR + [mLSAME-2]  ;L-to-L
  [mRSAME] = (Rin + [dRSAME]*vWALL - [mRSAME-2])*vIIR + [mRSAME-2]  ;R-to-R
  ___Different Side Reflection (left-to-right and right-to-left)_______________
  [mLDIFF] = (Lin + [dRDIFF]*vWALL - [mLDIFF-2])*vIIR + [mLDIFF-2]  ;R-to-L
  [mRDIFF] = (Rin + [dLDIFF]*vWALL - [mRDIFF-2])*vIIR + [mRDIFF-2]  ;L-to-R
  ___Early Echo (Comb Filter, with input from buffer)__________________________
  Lout=vCOMB1*[mLCOMB1]+vCOMB2*[mLCOMB2]+vCOMB3*[mLCOMB3]+vCOMB4*[mLCOMB4]
  Rout=vCOMB1*[mRCOMB1]+vCOMB2*[mRCOMB2]+vCOMB3*[mRCOMB3]+vCOMB4*[mRCOMB4]
  ___Late Reverb APF1 (All Pass Filter 1, with input from COMB)________________
  Lout=Lout-vAPF1*[mLAPF1-dAPF1], [mLAPF1]=Lout, Lout=Lout*vAPF1+[mLAPF1-dAPF1]
  Rout=Rout-vAPF1*[mRAPF1-dAPF1], [mRAPF1]=Rout, Rout=Rout*vAPF1+[mRAPF1-dAPF1]
  ___Late Reverb APF2 (All Pass Filter 2, with input from APF1)________________
  Lout=Lout-vAPF2*[mLAPF2-dAPF2], [mLAPF2]=Lout, Lout=Lout*vAPF2+[mLAPF2-dAPF2]
  Rout=Rout-vAPF2*[mRAPF2-dAPF2], [mRAPF2]=Rout, Rout=Rout*vAPF2+[mRAPF2-dAPF2]
  ___Output to Mixer (Output volume multiplied with input from APF2)___________
  LeftOutput  = Lout*vLOUT
  RightOutput = Rout*vROUT
  ___Finally, before repeating the above steps_________________________________
  BufferAddress = MAX(mBASE, (BufferAddress+2) AND 7FFFEh)
  Wait one 22050Hz cycle, then repeat the above stuff

The values written to memory are saturated to -8000h..+7FFFh.
The multiplication results are divided by +8000h, to fit them to 16bit range.
All memory addresses are relative to the current BufferAddress, and wrapped within mBASE..7FFFEh when exceeding that region.

All data in the Reverb buffer consists of signed 16bit samples.
The Left and Right Reverb Buffer addresses should be choosen so that one half of the buffer contains Left samples, and
the other half Right samples (ie. the data is L,L,L,L,... R,R,R,R,...; it is NOT interlaced like L,R,L,R,...).
During operation, when the buffer address increases, the Left half will overwrite the older samples of the Right half, and vice-versa.

The reverb hardware spends one 44100h cycle on left calculations, and the next 44100h cycle on right
calculations (unlike as shown in the above formula, where left/right are shown simultaneously at 22050Hz).

https://psx-spx.consoledev.net/soundprocessingunitspu/#reverb-formula

*/
void SPU::updateReverb(const s32 reverbVoiceSamples[2], const s16 cdSamples[2], s16 output[2])
{
	// The downsampler ring buffers must be fed every tick.

	s32 inputSampleL = reverbVoiceSamples[0];
	s32 inputSampleR = reverbVoiceSamples[1];

	// Include CD audio in the reverb if enabled
	if (m_spucnt.CDAudioReverb)
	{
		inputSampleL += (s32)cdSamples[0];
		inputSampleR += (s32)cdSamples[1];
	}

	m_downsamplerRingbufferL[m_downsamplerRingbufferIndex] = saturateS32toS16(inputSampleL); // Left sample of current stereo frame
	m_downsamplerRingbufferR[m_downsamplerRingbufferIndex] = saturateS32toS16(inputSampleR); // Right sample of current stereo frame

	// Processed reverb every other cycle i.e. at 22050 Hz
	// #TODO: Should we update both channels every other cycle, or alternate like the hardware does?
	if (m_reverbCounter == 0)
	{
		// Downsample from 44100 Hz to 22050 Hz to calculate the reverb unit input.
		s32 LeftInput = 0; // input to reverb unit
		s32 RightInput = 0;
		{
			for (size_t j = 0; j < COUNTOF_ARRAY(kFiniteImpulseResponse); j++)
			{
				// n.b. The filter is not centered so introduces a delay. To center the filter input index would need to
				// be offset by 19 (half of 39) but this would cause negative input index for the first 19 output samples,
				// so we start with input index of 0 and let the filter introduce the delay.
				int inputIndex = (int)m_downsamplerRingbufferIndex - (int)j;
				if (inputIndex < 0)
					inputIndex += kFIRFilterSize; // Wrap around; circular buffer

				s32 h = (s32)kFiniteImpulseResponse[j]; // impulse response
				LeftInput += (s32)m_downsamplerRingbufferL[inputIndex] * h;
				RightInput += (s32)m_downsamplerRingbufferR[inputIndex] * h;
			}

			LeftInput >>= 15; // Rescale by 0x8000
			RightInput >>= 15; // Rescale by 0x8000

#if 0 // #TODO: Debug output if required
			HP_DEBUG_ASSERT(downsampledBufferLenSamples + 2 <= downsampledBufferCapacitySamples); // should never fail by construction
			downsampledBuffer[downsampledBufferLenSamples++] = saturateS32toS16(LeftInput);
			downsampledBuffer[downsampledBufferLenSamples++] = saturateS32toS16(RightInput);
#endif
		}

		// Apply input volume vLIN, vRIN
		s32 Lin = ((s32)(s16)m_rev.vLIN * LeftInput) >> 15; // / 0x8000;
		s32 Rin = ((s32)(s16)m_rev.vRIN * RightInput) >> 15; // / 0x8000;

		// Reverb chain

		// Same Side Reflection (left-to-left and right-to-right)
		//   [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR + [mLSAME-2]  ;L-to-L
		//   [mRSAME] = (Rin + [dRSAME]*vWALL - [mRSAME-2])*vIIR + [mRSAME-2]  ;R-to-R
		s32 LSAME = applyReflection(Lin, m_rev.mLSAME, m_rev.dLSAME, m_rev.vIIR, m_rev.vWALL);
		s32 RSAME = applyReflection(Rin, m_rev.mRSAME, m_rev.dRSAME, m_rev.vIIR, m_rev.vWALL);
		// The outputs aren't used directly. They are written to memory and read by the subsequent comb filters.
#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		HP_DEBUG_ASSERT(sameSideReflectionBufferLenSamples + 2 <= sameSideReflectionBufferCapacitySamples); // should never fail by construction
		sameSideReflectionBuffer[sameSideReflectionBufferLenSamples++] = saturateS32toS16(LSAME);
		sameSideReflectionBuffer[sameSideReflectionBufferLenSamples++] = saturateS32toS16(RSAME);
#else
		HP_UNUSED(LSAME);
		HP_UNUSED(RSAME);
#endif

		// Different Side Reflection (left-to-right and right-to-left)
		//   [mLDIFF] = (Lin + [dRDIFF]*vWALL - [mLDIFF-2])*vIIR + [mLDIFF-2]  ;R-to-L   n.b. This uses the *right* delay tap dRDIFF to bounce the signal from left to right
		//   [mRDIFF] = (Rin + [dLDIFF]*vWALL - [mRDIFF-2])*vIIR + [mRDIFF-2]  ;L-to-R   n.b. This uses the *left* delay tap dLDIFF to bounce the signal from right to left
		s32 LDIFF = applyReflection(Lin, m_rev.mLDIFF, m_rev.dRDIFF, m_rev.vIIR, m_rev.vWALL);
		s32 RDIFF = applyReflection(Rin, m_rev.mRDIFF, m_rev.dLDIFF, m_rev.vIIR, m_rev.vWALL);
		// The outputs aren't used directly. They are written to memory and read by the subsequent comb filters.
#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		HP_DEBUG_ASSERT(differentSideReflectionBufferLenSamples + 2 <= differentSideReflectionBufferCapacitySamples); // should never fail by construction
		differentSideReflectionBuffer[differentSideReflectionBufferLenSamples++] = saturateS32toS16(LDIFF);
		differentSideReflectionBuffer[differentSideReflectionBufferLenSamples++] = saturateS32toS16(RDIFF);
#else
		HP_UNUSED(LDIFF);
		HP_UNUSED(RDIFF);
#endif

		// Early Echo (Comb Filter, with input from buffer)
		//   Lout=vCOMB1*[mLCOMB1]+vCOMB2*[mLCOMB2]+vCOMB3*[mLCOMB3]+vCOMB4*[mLCOMB4]
		//   Rout=vCOMB1*[mRCOMB1]+vCOMB2*[mRCOMB2]+vCOMB3*[mRCOMB3]+vCOMB4*[mRCOMB4]
		s32 Lout = applyEarlyEcho(m_rev.mLCOMB1, m_rev.mLCOMB2, m_rev.mLCOMB3, m_rev.mLCOMB4, m_rev.vCOMB1, m_rev.vCOMB2, m_rev.vCOMB3, m_rev.vCOMB4);
		s32 Rout = applyEarlyEcho(m_rev.mRCOMB1, m_rev.mRCOMB2, m_rev.mRCOMB3, m_rev.mRCOMB4, m_rev.vCOMB1, m_rev.vCOMB2, m_rev.vCOMB3, m_rev.vCOMB4);
#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		HP_DEBUG_ASSERT(earlyEchoBufferLenSamples + 2 <= earlyEchoBufferCapacitySamples); // should never fail by construction
		earlyEchoBuffer[earlyEchoBufferLenSamples++] = saturateS32toS16(Lout);
		earlyEchoBuffer[earlyEchoBufferLenSamples++] = saturateS32toS16(Rout);
#else
		HP_UNUSED(Lout);
		HP_UNUSED(Rout);
#endif
		// Late Reverb APF1 (All Pass Filter 1, with input from COMB)
		//   Lout=Lout-vAPF1*[mLAPF1-dAPF1], [mLAPF1]=Lout, Lout=Lout*vAPF1+[mLAPF1-dAPF1]
		//   Rout=Rout-vAPF1*[mRAPF1-dAPF1], [mRAPF1]=Rout, Rout=Rout*vAPF1+[mRAPF1-dAPF1]
		Lout = applyLateReverb(Lout, m_rev.mLAPF1, m_rev.dAPF1, m_rev.vAPF1);
		Rout = applyLateReverb(Rout, m_rev.mRAPF1, m_rev.dAPF1, m_rev.vAPF1);
#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		HP_DEBUG_ASSERT(lateReverb1BufferLenSamples + 2 <= lateReverbBufferCapacitySamples); // should never fail by construction
		lateReverb1Buffer[lateReverb1BufferLenSamples++] = saturateS32toS16(Lout);
		lateReverb1Buffer[lateReverb1BufferLenSamples++] = saturateS32toS16(Rout);
#endif

		// Late Reverb APF2 (All Pass Filter 2, with input from APF1)
		//   Lout=Lout-vAPF2*[mLAPF2-dAPF2], [mLAPF2]=Lout, Lout=Lout*vAPF2+[mLAPF2-dAPF2]
		//   Rout=Rout-vAPF2*[mRAPF2-dAPF2], [mRAPF2]=Rout, Rout=Rout*vAPF2+[mRAPF2-dAPF2]
		Lout = applyLateReverb(Lout, m_rev.mLAPF2, m_rev.dAPF2, m_rev.vAPF2);
		Rout = applyLateReverb(Rout, m_rev.mRAPF2, m_rev.dAPF2, m_rev.vAPF2);
#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		HP_DEBUG_ASSERT(lateReverb2BufferLenSamples + 2 <= lateReverbBufferCapacitySamples); // should never fail by construction
		lateReverb2Buffer[lateReverb2BufferLenSamples++] = saturateS32toS16(Lout);
		lateReverb2Buffer[lateReverb2BufferLenSamples++] = saturateS32toS16(Rout);
#endif

		// Apply output volume vLOUT, vROUT
		s32 LeftOutput = (Lout * (s32)m_reverbOutputVolumeLeft) >> 15; // / 0x8000;
		s32 RightOutput = (Rout * (s32)m_reverbOutputVolumeRight) >> 15; // / 0x8000;

		// Write the new reverb output values into the upsampler ring buffers
		m_upsamplerRingbufferL[m_upsamplerRingbufferIndex] = saturateS32toS16(LeftOutput);
		m_upsamplerRingbufferR[m_upsamplerRingbufferIndex] = saturateS32toS16(RightOutput);

#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
		// Write to debug reverb output buffer at 22050 Hz
		HP_DEBUG_ASSERT(reverbOutputBufferLenSamples + 2 <= reverbOutputBufferCapacitySamples); // should never fail by construction
		reverbOutputBuffer[reverbOutputBufferLenSamples++] = upsamplerRingbufferL[upsamplerRingbufferIndex];
		reverbOutputBuffer[reverbOutputBufferLenSamples++] = upsamplerRingbufferR[upsamplerRingbufferIndex];
#endif
		// Increment and wrap reverb buffer current head position.
		// Note that each reverb stage has separate buffers for L and R so only need to advance by 1 sample (2 bytes) per cycle, not 2 samples for stereo.
		m_currentReverbBufferHead += 2;
		if (m_currentReverbBufferHead >= kRamSizeBytes)
			m_currentReverbBufferHead -= m_reverbBufferSizeBytes;
	}
	else
	{
		// Zero stuffing for upsampling: every other sample is zero, so write zeros to the ring buffer for the odd cycles where reverb is not processed.
		m_upsamplerRingbufferL[m_upsamplerRingbufferIndex] = 0;
		m_upsamplerRingbufferR[m_upsamplerRingbufferIndex] = 0;
	}

	// Upsample from 22050 Hz back to 44100 Hz
	// In the PSX SPU reverb unit, this is achieved with "zero stuffing" every other element and convolving with the same FIR filter.
	s32 upsampledL = 0;
	s32 upsampledR = 0;
	for (size_t j = 0; j < COUNTOF_ARRAY(kFiniteImpulseResponse); j++)
	{
		int inputIndex = (int)m_upsamplerRingbufferIndex - (int)j;
		if (inputIndex < 0)
			inputIndex += kFIRFilterSize; // Wrap around; circular buffer

		s32 h = (s32)kFiniteImpulseResponse[j]; // impulse response
		upsampledL += (s32)m_upsamplerRingbufferL[inputIndex] * h;
		upsampledR += (s32)m_upsamplerRingbufferR[inputIndex] * h;
	}

	// The coefficients in the FIR table represent volume multipler N/0x8000 so should rescale by dividing by 0x8000 (shifting right by 15) to compensate for this.
	// However, because of the zero stuffing, every other sample in the input is zero, the volume will be halved compared to the downsampled signal,
	// so instead of rescaling by 0x8000, we can rescale by 0x4000 (shifting right by 14)
	upsampledL >>= 14;
	upsampledR >>= 14;

	output[0] = saturateS32toS16(upsampledL);
	output[1] = saturateS32toS16(upsampledR);

#if SPU_REVERB_DEBUG_BUFFERS_ENABLED
	HP_DEBUG_ASSERT(upsampledBufferLenSamples + 2 <= upsampledBufferCapacitySamples); // should never fail by construction
	upsampledBuffer[upsampledBufferLenSamples++] = output[0];
	upsampledBuffer[upsampledBufferLenSamples++] = output[1];
#endif

	if (s_pReverbFile)
	{
		fwrite((const void*)&output[0], 2, 1, s_pReverbFile); // left
		fwrite((const void*)&output[1], 2, 1, s_pReverbFile); // right
	}

	// Increment this after calculating the output sample for the current input, so that the current input is included in the FIR filter calculation for the next output sample.
	m_downsamplerRingbufferIndex++;
	if (m_downsamplerRingbufferIndex == COUNTOF_ARRAY(m_downsamplerRingbufferL))
		m_downsamplerRingbufferIndex = 0;

	// Increment this after calculating the output sample for the current input, so that the current input is included in the FIR filter calculation for the next output sample.
	m_upsamplerRingbufferIndex++;
	if (m_upsamplerRingbufferIndex == COUNTOF_ARRAY(m_upsamplerRingbufferL))
		m_upsamplerRingbufferIndex = 0;

	m_reverbCounter = !m_reverbCounter;
}


// Delay and filter. And mix the two together.
// 
// A delayed version of the input is added to the input and then blended with previous sample.
//
//     buf[m_addr] = (input + buf[d_addr]*vWALL - buf[m_addr-2])*vIIR + buf[m_addr-2]
//
// In DSP notation this is:
// 
//     y[n] = (x[n] + vWALL*y[n-D] - y[n-1])*vIIR + y[n-1]
//
// Where [m_addr-2] is the previous sample in the delay line.
//
// This can be rewritten to show that this is a weighted average of the new sample an the previous sample:
//
// buf[m_addr] = vIIR*(input + buf[d_addr]*vWALL) - vIIR*buf[m_addr-2] + buf[m_addr-2]
// buf[m_addr] = vIIR*(input + buf[d_addr]*vWALL) + 1*buf[m_addr-2] - vIIR*buf[m_addr-2]
// buf[m_addr] = vIIR*(input + buf[d_addr]*vWALL) + (1-vIIR)*buf[m_addr-2]
//
// In DSP notation this is:
// 
//     y[n] = vIIR*(x[n] + y[n-D]*vWALL) - (1-vIIR)*y[n-1]
//
// In jsgroth's blog post, 1-vIIR is written as 0x8000 - vIIR because volume is represented as a signed 16-bit value.
//
// High vIIR makes the reverb responsive to new inputs.
// Low vIIR makes the reverb less responsive to new inputs, which creates a longer decay time.  This is a "Leaky Integrator"
// which acts as a low pass filter by averaging inputs over time.
//
// It is an *Infinite* Impulse Response filter because the output depends on all previous inputs, as well as previous outputs (feedback).
//
// Note that the vIIR coefficient also applies to the tapped echo. This prevents the energy in the system from growing continually via feedback.
//
s32 SPU::applyReflection(
	s32 input,
	u16 m_addr,
	u16 d_addr, // delayed tap taken from buffer
	s16 vIIR, // Feedback coefficient. Controls decay time
	s16 vWALL) // Reflection coefficient. The "hardness" of the room's walls
{
	u32 outputAddr = reverbAddrToRamAddr(m_addr);

	// previous output sample y[n-1]
	u32 prevAddr = outputAddr > m_reverbBufferStartAddress ? outputAddr - 2 : SPU::kRamSizeBytes - 2; // the reverb buffer always extends to the end of RAM, so wrap around to there
	s32 prev = (s32)readReverbSampleFromRAM(prevAddr);

	// tap sample for echo
	u32 tapAddr = reverbAddrToRamAddr(d_addr);
	s32 tap = (s32)readReverbSampleFromRAM(tapAddr); // tap a previous sample to generate delay

	s32 reflection = (tap * vWALL) >> 15; // Apply volume, and rescale back down to signed 16-bit range

	// Lerp between new input+delay and previous output
	s32 output = ((input + reflection - prev) * vIIR) >> 15; // Rescale back down after multiplying by volume
	output += prev;

	writeReverbSampleToRAM(outputAddr, saturateS32toS16(output));

	return output;
}

// Comb filter to simulate hearing the same sound arriving at different times from different paths through the room.
// It is called a comb filter because of the characterisic shape of the frequency response, which has regularly spaced peaks and troughs like a comb.
// The delay time determines the spacing of the peaks and troughs, and the feedback volume determines how pronounced they are.
// The peaks and troughs are generated due to constructive and destructive interference between the direct sound and the delayed sound.
//
//   out=vCOMB1*[mCOMB1] + vCOMB2*[mCOMB2] + vCOMB3*[mCOMB3] + vCOMB4*[mCOMB4]
//
s32 SPU::applyEarlyEcho(
	u16 mComb1, u16 mComb2, u16 mComb3, u16 mComb4,
	s16 vComb1, s16 vComb2, s16 vComb3, s16 vComb4)
{
	s32 output = 0;
	output += (s32)vComb1 * (s32)readSampleFromReverbBuffer(mComb1);
	output += (s32)vComb2 * (s32)readSampleFromReverbBuffer(mComb2);
	output += (s32)vComb3 * (s32)readSampleFromReverbBuffer(mComb3);
	output += (s32)vComb4 * (s32)readSampleFromReverbBuffer(mComb4);
	output >>= 15; // Rescale back down after multiplying by volume

	return output;
}

// Late Reverb All Pass Filter, with input from COMB
//
// Passes all frequencies with equal magnitude (hence "all-pass")
// Introduces frequency-dependent phase shifts
// Creates diffusion - smearing out echoes into a dense reverb tail
// Makes discrete echoes from comb filters sound more natural
// 
//   out = in - (vAPF * buf[m_addr-d_addr])
//   buf[m_addr]=out
//   out = (out * vAPF) + buf[m_addr-d_addr]
//
// Duckstation:
//      // Late Reverb APF1 (All Pass Filter 1, with input from COMB).
//      const s32 FB_A = ReverbRead(s_state.reverb_registers.MIX_DEST_A[channel] - s_state.reverb_registers.FB_SRC_A);
//      const s32 FB_B = ReverbRead(s_state.reverb_registers.MIX_DEST_B[channel] - s_state.reverb_registers.FB_SRC_B);
//      const s32 MDA = Clamp16((ACC + ((FB_A * neg(s_state.reverb_registers.FB_ALPHA)) >> 14)) >> 1);
//
s32 SPU::applyLateReverb(
	s32 input,
	u16 m_addr,
	u16 d_addr, // displacement relative to m_addr. It represents the buffer size i.e. delay time of the late reverb
	s16 vAPF)
{
	u32 addr = reverbAddrToRamAddr(m_addr);

	// Calculate the address of the delayed sample that we will read and write for the APF
	u32 disp = d_addr * 8u; // Convert from address in sound ram (divided by 8) to byte offset.
	s64 delayedSampleAddrSigned = (s64)addr - (s64)disp; // handle potential wrap-around by using signed arithmetic first
	while (delayedSampleAddrSigned < m_reverbBufferStartAddress)
		delayedSampleAddrSigned += m_reverbBufferSizeBytes; // the reverb buffer always extends to the end of RAM, so wrap around to there
	u32 delayedSampleAddr = (u32)delayedSampleAddrSigned;
	HP_ASSERT(delayedSampleAddr >= m_reverbBufferStartAddress && delayedSampleAddr + 2 <= SPU::kRamSizeBytes);

	// out = in - (vAPF * buf[m_addr-d_addr])
	s16 delayedSample = readReverbSampleFromRAM(delayedSampleAddr);
	s32 output = input - ((vAPF * delayedSample) >> 15); // Apply volume, and rescale back down to signed 16-bit range

	// buf[m_addr] = out
	writeReverbSampleToRAM(addr, saturateS32toS16(output));

	// out = (out * vAPF) + buf[m_addr-d_addr]
	output = (output * vAPF) >> 15; // Rescale back down after multiplying by volume
	output += (s32)delayedSample;

	return output;
}
