//
// Implementation note:
//
// This is high level emulation. We schedule delayed responses and IRQs to emulate slow CDROM subsystem
// internal CPU, which takes time to generate the IRQ. The BIOS expects these delays between issuing CDROM
// commands and receiving the CDROM IRQs in response. Without this delay it only sends two commands to the
// CDROM and then stops, which prevents CDs from being loaded.
//

#include "CDROM.h"

#include "Scheduler.h"
#include "INTC.h"
#include "Timing.h"

#include "core/RingBuffer.h"
#include "core/MathsHelpers.h" // IsPowerOfTwo
#include "core/Log.h"
#include "core/StringHelpers.h" // SafeSnprintf
#include "core/hp_assert.h"
#include "core/ArrayHelpers.h"
#include "core/Helpers.h" // HP_UNUSED

#include <string.h> // memset #TODO: Remove if possible

//----------------------------------------------------------------------------------------------------------------------------

static constexpr u8 kSectorSyncBytes[] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
static_assert(COUNTOF_ARRAY(kSectorSyncBytes) == CD::kSectorSyncSizeBytes);

// Each XA-ADPCM sector contains 4032 samples at 37800 Hz.
// This is resampled up to 44100 Hz, which is 7/6, so 7 samples for every 6 samples.
// 4032 * 7/6 = 4704. At 2 bytes per sample (16 bit signed), this is 9408 (24C0h) bytes per sector after resampling.
// Need to double though, because mono is only a single channel of input that is duplicated to both L and R channels of output.
// Need to double again, because 18900 Hz (half sample rate) audio needs to be doubled up in the output buffer.
static constexpr unsigned int kResampledXABufferSizeBytes = 4 * 0x4000;

/*
   Zigzag interpolation tables
   Used for XA-ADPCM resampling.

   Table1, Table2, Table3, Table4, Table5, Table6, Table7  ;Index
   0     , 0     , 0     , 0     , -0001h, +0002h, -0005h  ;1
   0     , 0     , 0     , -0001h, +0003h, -0008h, +0011h  ;2
   0     , 0     , -0001h, +0003h, -0008h, +0010h, -0023h  ;3
   0     , -0002h, +0003h, -0008h, +0011h, -0023h, +0046h  ;4
   0     , 0     , -0002h, +0006h, -0010h, +002Bh, -0017h  ;5
   -0002h, +0003h, -0005h, +0005h, +000Ah, +001Ah, -0044h  ;6
   +000Ah, -0013h, +001Fh, -001Bh, +006Bh, -00EBh, +015Bh  ;7
   -0022h, +003Ch, -004Ah, +00A6h, -016Dh, +027Bh, -0347h  ;8
   +0041h, -004Bh, +00B3h, -01A8h, +0350h, -0548h, +080Eh  ;9
   -0054h, +00A2h, -0192h, +0372h, -0623h, +0AFAh, -1249h  ;10
   +0034h, -00E3h, +02B1h, -05BFh, +0BCDh, -16FAh, +3C07h  ;11
   +0009h, +0132h, -039Eh, +09B8h, -1780h, +53E0h, +53E0h  ;12
   -010Ah, -0043h, +04F8h, -11B4h, +6794h, +3C07h, -16FAh  ;13
   +0400h, -0267h, -05A6h, +74BBh, +234Ch, -1249h, +0AFAh  ;14
   -0A78h, +0C9Dh, +7939h, +0C9Dh, -0A78h, +080Eh, -0548h  ;15
   +234Ch, +74BBh, -05A6h, -0267h, +0400h, -0347h, +027Bh  ;16
   +6794h, -11B4h, +04F8h, -0043h, -010Ah, +015Bh, -00EBh  ;17
   -1780h, +09B8h, -039Eh, +0132h, +0009h, -0044h, +001Ah  ;18
   +0BCDh, -05BFh, +02B1h, -00E3h, +0034h, -0017h, +002Bh  ;19
   -0623h, +0372h, -0192h, +00A2h, -0054h, +0046h, -0023h  ;20
   +0350h, -01A8h, +00B3h, -004Bh, +0041h, -0023h, +0010h  ;21
   -016Dh, +00A6h, -004Ah, +003Ch, -0022h, +0011h, -0008h  ;22
   +006Bh, -001Bh, +001Fh, -0013h, +000Ah, -0005h, +0002h  ;23
   +000Ah, +0005h, -0005h, +0003h, -0001h, 0     , 0       ;24
   -0010h, +0006h, -0002h, 0     , 0     , 0     , 0       ;25
   +0011h, -0008h, +0003h, -0002h, +0001h, 0     , 0       ;26
   -0008h, +0003h, -0001h, 0     , 0     , 0     , 0       ;27
   +0003h, -0001h, 0     , 0     , 0     , 0     , 0       ;28
   -0001h, 0     , 0     , 0     , 0     , 0     , 0       ;29
*/
static const unsigned int kZigZagInterpolationTableCount = 7;
static const unsigned int kZigZagInterpolationTableElementCount = 29;
static const s16 kZigZagInterpolationTables[kZigZagInterpolationTableCount][kZigZagInterpolationTableElementCount] =
{
	{ // Table[0]
		0,
		0,
		0,
		0,
		0,
		-0x0002,
		+0x000A,
		-0x0022,
		+0x0041,
		-0x0054,
		+0x0034,
		+0x0009,
		-0x010A,
		+0x0400,
		-0x0A78,
		+0x234C,
		+0x6794,
		-0x1780,
		+0x0BCD,
		-0x0623,
		+0x0350,
		-0x016D,
		+0x006B,
		+0x000A,
		-0x0010,
		+0x0011,
		-0x0008,
		+0x0003,
		-0x0001
	},
	{ // Table[1]
		0,
		0,
		0,
		-0x0002,
		0,
		+0x0003,
		-0x0013,
		+0x003C,
		-0x004B,
		+0x00A2,
		-0x00E3,
		+0x0132,
		-0x0043,
		-0x0267,
		+0x0C9D,
		+0x74BB,
		-0x11B4,
		+0x09B8,
		-0x05BF,
		+0x0372,
		-0x01A8,
		+0x00A6,
		-0x001B,
		+0x0005,
		+0x0006,
		-0x0008,
		+0x0003,
		-0x0001,
		0,
	},
	{ // Table[2]
		0,
		0,
		-0x0001,
		+0x0003,
		-0x0002,
		-0x0005,
		+0x001F,
		-0x004A,
		+0x00B3,
		-0x0192,
		+0x02B1,
		-0x039E,
		+0x04F8,
		-0x05A6,
		+0x7939,
		-0x05A6,
		+0x04F8,
		-0x039E,
		+0x02B1,
		-0x0192,
		+0x00B3,
		-0x004A,
		+0x001F,
		-0x0005,
		-0x0002,
		+0x0003,
		-0x0001,
		0,
		0,
	},
	{ // Table[3]
		0,
		-0x0001,
		+0x0003,
		-0x0008,
		+0x0006,
		+0x0005,
		-0x001B,
		+0x00A6,
		-0x01A8,
		+0x0372,
		-0x05BF,
		+0x09B8,
		-0x11B4,
		+0x74BB,
		+0x0C9D,
		-0x0267,
		-0x0043,
		+0x0132,
		-0x00E3,
		+0x00A2,
		-0x004B,
		+0x003C,
		-0x0013,
		+0x0003,
		0     ,
		-0x0002,
		0     ,
		0     ,
		0     ,
	},
	{ // Table[4]
		-0x0001,
		+0x0003,
		-0x0008,
		+0x0011,
		-0x0010,
		+0x000A,
		+0x006B,
		-0x016D,
		+0x0350,
		-0x0623,
		+0x0BCD,
		-0x1780,
		+0x6794,
		+0x234C,
		-0x0A78,
		+0x0400,
		-0x010A,
		+0x0009,
		+0x0034,
		-0x0054,
		+0x0041,
		-0x0022,
		+0x000A,
		-0x0001,
		0     ,
		+0x0001,
		0     ,
		0     ,
		0     ,
	},
	{ // Table[5]
		+0x0002,
		-0x0008,
		+0x0010,
		-0x0023,
		+0x002B,
		+0x001A,
		-0x00EB,
		+0x027B,
		-0x0548,
		+0x0AFA,
		-0x16FA,
		+0x53E0,
		+0x3C07,
		-0x1249,
		+0x080E,
		-0x0347,
		+0x015B,
		-0x0044,
		-0x0017,
		+0x0046,
		-0x0023,
		+0x0011,
		-0x0005,
		0     ,
		0     ,
		0     ,
		0     ,
		0     ,
		0     ,
	},
	{ // Table[6]
		-0x0005,
		+0x0011,
		-0x0023,
		+0x0046,
		-0x0017,
		-0x0044,
		+0x015B,
		-0x0347,
		+0x080E,
		-0x1249,
		+0x3C07,
		+0x53E0,
		-0x16FA,
		+0x0AFA,
		-0x0548,
		+0x027B,
		-0x00EB,
		+0x001A,
		+0x002B,
		-0x0023,
		+0x0010,
		-0x0008,
		+0x0002,
		0,
		0,
		0,
		0,
		0,
		0,
	},
};

//----------------------------------------------------------------------------------------------------------------------------
// Command constants
//----------------------------------------------------------------------------------------------------------------------------

// Assume most/all commands have the same first response time, meaning the delay between
// receiving the command and acknowledging it by generating an IRQ.
// See https://psx-spx.consoledev.net/cdromdrive/#cdrom-response-timings
static const u32 kDefaultFirstResponseDurationCycles = 0x000c4e1;

// Currently use fixed seek time of 1/60 of a second, as recommended on EmuDev Discord playstation channel.
// "In some cases (like seek or spin-up), it may take more than a second until the 2nd response is sent." - psx-spx
// #TODO: Calculate seek time more accurately based on drive physical characteristics and remove this.
//static constexpr float kSeekTimeSeconds = 1.0f / 60.0f;
//static constexpr float kSeekTimeSeconds = 1.0f / 75.0f; // Reportedly, Doom requires fast seek time (Chicho)
//static constexpr float kSeekTimeSeconds = 0.1f; // More realistic seek time
static constexpr float kSeekTimeSeconds = 0.382f; // Much larger seek time, as seen in Duckstation logs for Earthworm Jim 2
static constexpr u32 kSeekTimeCycles = (u32)(kCpuClock * kSeekTimeSeconds); // (cycles/sec) * sec

// Init takes longer than most commands to send the the first response:
// https://psx-spx.consoledev.net/cdromdrive/#first-response
static const u32 kInitFirstResponseTimeCycles = 0x13cce;

static constexpr float kInitSecondResponseTimeSeconds = 0.001f; // 1 ms (see EmuDev)  #TODO: This feels too short!
static constexpr u32 kInitSecondResponseTimeCycles = (u32)(kCpuClock * kInitSecondResponseTimeSeconds); // (cycles/sec) * sec

// Calculate the exact timings for sector reads.
// This is important for accurate CDDA and CDXA audio streaming.
// The docs say ReadN is 0x36cd2 cycles average between reads at double speed.
// In Megaman X4 XA-ADPCM every 8th sector is an audio sector which decodes to 4704 samples.
// 2 samples are read each 768 cycles.
// This gives a delay of: (4704 / 2 / 8) * 768 = 225792 = 0x37200 exactly.
// n.b. This is exactly the same as CPU clock / 75 (single speed) or CPU clock / 150 (double speed)
// "The INT1 rate needs to be precise for CD-DA and CD-XA Audio streaming,
//  exact clock cycle values should be: SystemClock*930h/4/44100Hz for Single Speed (and half as much for Double
//  Speed) (the "Average" values are AVERAGE values, not exact values)."
// https://psx-spx.consoledev.net/cdromdrive/#int1-rate

static constexpr u32 kReadNTimeCycles_SingleSpeed = 0x6e400;
static constexpr u32 kReadNTimeCycles_DoubleSpeed = 0x37200;
static constexpr u32 kReadSTimeCycles_SingleSpeed = 0x6e400;
static constexpr u32 kReadSTimeCycles_DoubleSpeed = 0x37200; // Calculated for XA-ADPCM
static_assert(kCpuClock % 75 == 0);
static_assert(kCpuClock / 75 == kReadSTimeCycles_SingleSpeed);
static_assert(kCpuClock % 150 == 0);
static_assert(kCpuClock / 150 == kReadSTimeCycles_DoubleSpeed);

static constexpr u32 kPlaySectorTimeCycles = 0x6e400; // Average time

//  ___These values appear in the FIRST response; with stat.bit0 set___
//  10h - Invalid Sub_function (for command 19h), or invalid parameter value
//  20h - Wrong number of parameters
//  40h - Invalid command
//  80h - Cannot respond yet (eg. required info was not yet read from disk yet)
//           (namely, TOC not-yet-read or so)
//           (also appears if no disk inserted at all)
//  ___These values appear in the SECOND response; with stat.bit2 set___
//  04h - Seek failed (when trying to use SeekL on Audio CDs)
//  ___These values appear even if no command was sent; with stat.bit2 set___
//  08h - Drive door became opened
// https://psx-spx.consoledev.net/cdromdrive/#status-code-stat
static const u8 kCDROM_Error_WrongNumberOfParameters = 0x20;
// #TODO: Add other constants

//----------------------------------------------------------------------------------------------------------------------------

#define CDROM_XA_ADPCM_CAPTURE_ENABLED 0

#if CDROM_XA_ADPCM_CAPTURE_ENABLED
static FILE* s_pXAADPCMFile;
#endif

//----------------------------------------------------------------------------------------------------------------------------

// Forward declarations
static void decode_28_nibbles(const u8* pSrc, unsigned int blk, unsigned int nibble, s16* pDst, s16& old, s16& older);
static s16 zigZagInterpolate(const s16 resamplingRingBuffer[32], unsigned int p, const s16 table[kZigZagInterpolationTableElementCount]);

// Decodes 8-bit binary coded decimal value
// Assumes each nibble <= 9
static constexpr u8 BCDtoDecimal(u8 bcd)
{
	return (bcd >> 4) * 10 + (bcd & 0xf);
}

// Assumes decimal <= 99
static constexpr u8 DecimalToBCD(u8 decimal)
{
	return ((decimal / 10) << 4) | decimal % 10;
}

static void logMode(CDROM::Mode mode)
{
	LOG_INFO(
		"[CDROM] Setmode command 1Eh Mode: %02X\n"
		"[CDROM]   Speed: %s\n"
		"[CDROM]   XA-ADPCM: %u\n"
		"[CDROM]   Sector Size: %u = %s\n"
		"[CDROM]   Ignore: %u %s\n"
		"[CDROM]   XA-Filter: %u\n"
		"[CDROM]   Report: %u\n"
		"[CDROM]   AutoPause: %u\n"
		"[CDROM]   CDDA: %u\n",
		mode.val,
		mode.doubleSpeed ? "Double" : "Normal",
		mode.XA_ADPCM,
		mode.SectorSize, mode.SectorSize == 0 ? "800h (2048) bytes, DataOnly" : "924h (2340) bytes, Whole Sector Except Sync Bytes",
		mode.IgnoreBit, mode.IgnoreBit == 0 ? "" : "Ignore Sector Size and Setloc position",
		mode.XA_Filter,
		mode.Report,
		mode.AutoPause,
		mode.CDDA);
}

//----------------------------------------------------------------------------------------------------------------------------

CDROM::CDROM(INTC& intc, Scheduler& scheduler)
: m_intc(intc)
, m_scheduler(scheduler)
{
	m_stat.ShellOpen = m_shellOpen;

	m_sectorDataBufferStorage = new u8[kSectorDataBufferSizeBytes];
	memset(m_sectorDataBufferStorage, 0, kSectorDataBufferSizeBytes);
	m_pSectorDataBuffer = new RingBuffer(m_sectorDataBufferStorage, kSectorDataBufferSizeBytes);

	m_cddaBufferStorage = new u8[kCDDADataBufferSizeBytes];
	memset(m_cddaBufferStorage, 0, kCDDADataBufferSizeBytes);
	m_pCDDABuffer = new RingBuffer(m_cddaBufferStorage, kCDDADataBufferSizeBytes);

	m_pXABufferStorage = new u8[kResampledXABufferSizeBytes];
	memset(m_pXABufferStorage, 0, kResampledXABufferSizeBytes);
	m_pXABuffer = new RingBuffer(m_pXABufferStorage, kResampledXABufferSizeBytes);

#if CDROM_XA_ADPCM_CAPTURE_ENABLED
	s_pXAADPCMFile = fopen("hpsx-xaadpcm-og.raw", "wb");
#endif
}

CDROM::~CDROM()
{
	delete m_pSectorDataBuffer;
	delete[] m_sectorDataBufferStorage;

	delete m_pCDDABuffer;
	delete[] m_cddaBufferStorage;

	delete m_pXABuffer;
	delete[] m_pXABufferStorage;

#if CDROM_XA_ADPCM_CAPTURE_ENABLED
	if (s_pXAADPCMFile)
	{
		fclose(s_pXAADPCMFile);
		s_pXAADPCMFile = nullptr;
	}
#endif
}

// PARAMETER register
//
// Parameter bytes are written to this register *before* sending a command.
// The FIFO can hold 16 bytes. Once full, the decoder will clear the PRMWRDY flag.
// 
// Note: the CXD1199 datasheet incorrectly states the parameter FIFO is 8 bytes deep,
// however the longest CD-ROM command has a 13-byte parameter.
// 
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-write-bank-0-parameter
//
void CDROM::Reset()
{
	m_bank = 0;

	m_hintmsk = 0xe0; // HINTMSK bits 7:5 are always 1 when read, so set them here.

	// HINTSTS bits 7:5 are always 1 https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-read-all-banks-rddata
	m_hintsts = 0xe0;

	m_int1pending = false;
	m_int2pending = false;

	m_mode.val = 0; // #TODO: Is this the correct initial state?
	m_hchpctl.val = 0;

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_params); i++)
	{
		m_params[i] = 0;
	}
	m_paramCount = 0;

	resetResponseFIFO();

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Reset command synchronization state
	m_hasPendingCommand = false;
	m_pendingCommand = 0;
	memset(m_pendingParams, 0, sizeof(m_pendingParams));
	m_pendingParamCount = 0;

	// Reset physical state
	m_shellOpen = false; // CD door closed
	m_pCD = nullptr;
	m_spinningUp = false;
	m_stopped = true;

	m_stat.val = 0;
	m_stat.ShellOpen = m_shellOpen;
	m_stat.SpindleMotor = false;

	// Reset seek params (target head/laser location)
	m_targetLocMinutes = 0;
	m_targetLocSeconds = 0;
	m_targetLocFrames = 0;
	m_targetLBA = 0;
	m_setLocPending = false;

	// Reset current head/laser location
	m_headLBA = 0;

	m_GetTD_trackNum = 0;

	memset(m_sectorDataBufferStorage, 0, kSectorDataBufferSizeBytes);
	m_pSectorDataBuffer->Reset(); // reset ring buffer
	m_sectorDataByteValue = 0;
#if CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
	SafeStrcpy(m_debugSectorDataBufferDescription, sizeof(m_debugSectorDataBufferDescription), "Empty");
#endif

	memset(m_cddaBufferStorage, 0, kCDDADataBufferSizeBytes);
	m_pCDDABuffer->Reset(); // reset ring buffer

	// #TODO: Are these the correct initial volume values?
	m_cdLeftLeftVolume_pending = 0;
	m_cdLeftRightVolume_pending = 0;
	m_cdRightRightVolume_pending = 0;
	m_cdRightLeftVolume_pending = 0;
	m_cdLeftLeftVolume = 0;
	m_cdLeftRightVolume = 0;
	m_cdRightRightVolume = 0;
	m_cdRightLeftVolume = 0;

	resetXABuffer();
	m_xaMuted = false;
	m_xaFilterFileNumber = 0;
	m_xaFilterChannelNumber = 0;

	// Clear scheduled events.
	// Implementation note: Scheduler should have been independently reset, so no need to cancel these events.
	m_commandFirstResponseEvent = 0;
	m_commandSecondResponseEvent = 0;
	m_readSectorEvent = 0;
}

//
// Bank  0x1f801800  0x1f801801  0x1f801802  0x1f801803
// 0     ADDRESS     COMMAND     PARAMETER   HCHPCTL
// 1     ADDRESS     WRDATA      HINTMSK     HCLRCTL
// 2     ADDRESS     CI          ATV0        ATV1
// 3     ADDRESS     ATV2        ATV3        ADPCTL
//
void CDROM::WriteReg(unsigned int regIndex, u8 val)
{
	HP_DEBUG_ASSERT(regIndex < 4);

	// https://psx-spx.consoledev.net/cdromdrive/#playstation-cdrom-io-ports

	// Write to register 0 is the ADDRESS (Index/Status register in all 4 banks)

	switch ((m_bank << 2 | regIndex))
	{
		case 0:
			writeADDRESS(val);
			break;
		case 1:
			writeCOMMAND(val);
			break;
		case 2:
			writePARAMETER(val);
			break;
		case 3:
			writeHCHPCTL(val);
			break;
		case 4:
			writeADDRESS(val);
			break;
		case 5:
			writeWRDATA(val);
			break;
		case 6:
			writeHINTMASK(val);
			break;
		case 7:
			writeHCLRCTL(val);
			break;
		case 8:
			writeADDRESS(val);
			break;
		case 9:
			writeCI(val);
			break;
		case 10:
			writeATV0(val);
			break;
		case 11:
			writeATV1(val);
			break;
		case 12:
			writeADDRESS(val);
			break;
		case 13:
			writeATV2(val);
			break;
		case 14:
			writeATV3(val);
			break;
		case 15:
			writeADPCTL(val);
			break;
		default:
			HP_DEBUG_FATAL_ERROR("Invalid register");
	}
}

void CDROM::writeADDRESS(u8 val)
{
	// bits 1:0 are the bank index
	m_bank = val & 3;

	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ADDRESS reg value %02X: Select bank %u\n", val, m_bank);
}

void CDROM::writeCOMMAND(u8 command)
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Write COMMAND reg value %02X\n", command);

	if (m_firstResponseInProgress) // Street Fighter Alpha 3 issues Pause while Pause in progress.
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Warning: Ignoring command %02X because first response in progress for %s\n", command, m_scheduler.GetEventDebugName(m_commandFirstResponseEvent));
		return;
	}

	// Check if an interrupt is pending (IRQ acts as a lock)
	// HINTSTS bits 2:0 contain the interrupt level (0 = no interrupt pending)
	bool irqPending = (m_hintsts & 0x07) != 0;

	if (irqPending)
	{
		// Command must wait until IRQ is acknowledged
		if (m_hasPendingCommand)
		{
			// Stomp the previous pending command
			if (s_logCDROM || s_logCDROMCommands)
			{
				LOG_INFO("[CDROM] Warning: Command %02X received while IRQ pending and another command %02X already pending. Stomping previous pending command.\n",
					command, m_pendingCommand);

				// #TODO: Perhaps try ignoring new command?
			}
		}

		// Store command and parameters as pending
		m_hasPendingCommand = true;
		m_pendingCommand = command;
		m_pendingParamCount = m_paramCount;
		memcpy(m_pendingParams, m_params, m_paramCount);

		if (s_logCDROM || s_logCDROMCommands)
		{
			LOG_INFO("[CDROM] Command %02X deferred (IRQ%u pending, will execute after acknowledgment)\n",
				command, m_hintsts & 0x07);
		}

		return;
	}

	// Execute command immediately
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Executing command %02X immediately\n", command);
	executeCommand(command);
}

void CDROM::executeCommand(u8 command)
{
	switch (command)
	{
		case 0x01:
			executeNopCommand01();
			break;

		case 0x02:
			executeSetLocCommand02();
			break;

		case 0x03:
			executePlayCommand03();
			break;

		case 0x06:
			executeReadNCommand06();
			break;

		case 0x07:
			executeMotorOnCommand07();
			break;

		case 0x08:
			executeStopCommand08();
			break;

		case 0x09:
			executePauseCommand09();
			break;

		case 0x0A:
			executeInitCommand0A();
			break;

		case 0x0B:
			executeMuteCommand0B();
			break;

		case 0x0C:
			executeDemuteCommand0C();
			break;

		case 0x0D:
			executeSetfilterCommand0D();
			break;

		case 0x0E:
			executeSetmodeCommand0E();
			break;

		case 0x10:
			executeGetlocLCommand10();
			break;

		case 0x11:
			executeGetlocPCommand11();
			break;

		case 0x13:
			executeGetTNCommand13();
			break;

		case 0x14:
			executeGetTDCommand14();
			break;

		case 0x15:
			executeSeekLCommand15();
			break;

		case 0x16:
			executeSeekPCommand16();
			break;

		case 0x19:
			executeTestCommand19();
			break;

		case 0x1A:
			executeGetIDCommand1A();
			break;

		case 0x1B:
			executeReadSCommand1B();
			break;

		default:
			HP_FATAL_ERROR("[CDROM] Unimplemented command %02X\n", command);
			break;
	}
}

void CDROM::cancelCommandSecondResponse()
{
	if (m_commandSecondResponseEvent)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Cancelling command second response for %s\n", m_scheduler.GetEventDebugName(m_commandSecondResponseEvent));

		m_scheduler.Cancel(m_commandSecondResponseEvent);
		m_commandSecondResponseEvent = 0;
	}
}

void CDROM::errorINT5(u8 errorByte)
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Error INT5\n");

	resetParameterFIFO();

	resetResponseFIFO();

	// Not sure what to do with stat here...
	Stat statPrev = m_stat;
	m_stat.val = 0;
	m_stat.Error = true;
	m_stat.SpindleMotor = statPrev.SpindleMotor;
	writeResponseFIFO(m_stat.val);

	writeResponseFIFO(errorByte);

	m_hasPendingCommand = false;

	HP_ASSERT((m_hintsts & 7) == 0, "INT%u is already pending. Should it be overwritten, or should INT2 flag be set to be applied when INT acknowledged with CLRCTL write?");
	generateINT(5);
}

//
// PARAMETER FIFO (not implemented as such)
//
void CDROM::writePARAMETER(u8 val)
{
	if (m_paramCount == COUNTOF_ARRAY(m_params))
	{
		LOG_INFO("[CDROM] Failed to write PARAMETER value %02X - FIFO full\n", val);
		return;
	}

	// Buffer parameter in FIFO
	m_params[m_paramCount++] = val;

	if (s_logCDROM || s_logCDROMParams)
		LOG_INFO("[CDROM] Write PARAMETER value %02X. Param count = %u\n", val, m_paramCount);
}

// HCLRCTL (host clear control) register
//
// Set bits high to clear various status bits.
//
// Writing to HCLRCTL, can clear HINTSTS Interrupt Flag Register bits 4:0 to acknowledge interrupts
//
// Allows write CD Interrupt Flag Register (R/W) HINTSTS bits 4:0
// 
// Mapped to 1F801803h bank 1 for write
// 
//  0-2 CLRINT     Acknowledge HC05 interrupt "flags" (0=no change, 1=clear)
//  3   CLRBFEMPT  Acknowledge BFEMPT                 (0=no change, 1=clear)
//  4   CLRBFWRDY  Acknowledge BFBFWRDY               (0=no change, 1=clear)
//  5   SMADPCLR   Clear sound map XA-ADPCM buffer    (0=no change, 1=clear/stop playback)
//  6   CLRPRM     Clear parameter FIFO               (0=no change, 1=clear)
//  7   CHPRST     Reset decoder chip                 (0=no change, 1=reset)
// 
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801803-write-bank-1-hclrctl
//
void CDROM::writeHCLRCTL(u8 val)
{
	// Setting bits 4:0 resets the corresponding flags in HINTSTS.
	// Code commonly writes 7 to reset the interrupt flags or 1F to acknowledge all IRQs.
	u8 hintsts_bits = val & (u8)0x1f;

	if (s_logCDROM || s_logCDROMInterrupts)
	{
		LOG_INFO("[CDROM] Write HCLRCTL (interrupt flag) reg value %02X%s\n", val,
			hintsts_bits == 0x1f ? ": acknowledge all IRQs" : (hintsts_bits == 7 ? ": acknowledge INT" : ""));
		if ((hintsts_bits & 7) == 7 && (m_hintsts & 7))
			LOG_INFO("[CDROM]   INT%u acknowledged\n", m_hintsts & 7);
	}

	m_hintsts &= ~hintsts_bits;

	if ((m_hintsts & 7) == 0) // no interrupts pending
	{
		// The HC05 sub-CPU won't check for new sectors while processing a command.
		// This means that if a sector read has completed and data is available, then INT1 will not occur until m_firstResponseInProgress is false (BUSYSTS is not set)
		// and the INT3 has been acknowledged.
		// #TODO: Should INT1 take priority over INT2?
		if (m_int1pending && !m_firstResponseInProgress)
		{
			m_hintsts |= 1;
			m_int1pending = false;

			if (s_logCDROM || s_logCDROMInterrupts)
				LOG_INFO("[CDROM] Queued INT1 set pending\n");

			// Deferred response
			writeResponseFIFO(m_stat.val); // INT1(stat)
		}
		else if (m_int2pending)
		{
			m_hintsts |= 2;
			m_int2pending = false;

			if (s_logCDROM || s_logCDROMInterrupts)
				LOG_INFO("[CDROM] Queued INT2 set pending\n");
		}
	}

	// Execute pending command if IRQ was just acknowledged and a command is waiting
	if ((m_hintsts & 7) == 0 && m_hasPendingCommand)
	{
		// IRQ was acknowledged, execute the pending command
		if (s_logCDROM || s_logCDROMCommands)
		{
			LOG_INFO("[CDROM] IRQ acknowledged, executing pending command %02X\n", m_pendingCommand);
		}

		// Restore pending parameters to active parameter buffer
		m_paramCount = m_pendingParamCount;
		memcpy(m_params, m_pendingParams, m_pendingParamCount);

		u8 pendingCmd = m_pendingCommand;
		m_hasPendingCommand = false;
		m_pendingCommand = 0;
		m_pendingParamCount = 0;

		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Executing pending command %02X\n", pendingCmd);
		executeCommand(pendingCmd);
	}

	HP_DEBUG_ASSERT((val & (1 << 5)) == 0, "CDROM sound map XA-ADPCM buffer not implemented"); // 0x20

	if (val & (1 << 6)) // 0x40
		resetParameterFIFO();

	HP_ASSERT((val & (1 << 7)) == 0, "CDROM decoder chip reset not implemented"); // 0x80 #TODO: Is this just XA-ADPCM decoder state?
}

// 0x1f801801 (write, bank 1): WRDATA
// Used to upload sectors to the decoder for sound map XA-ADPCM playback.
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801801-write-bank-1-wrdata
void CDROM::writeWRDATA(u8 /*val*/)
{
	HP_DEBUG_FATAL_ERROR("Unexpected PSX write to CDROM WRDATA register");
}

// HINTMASK (host interrupt mask) register
//
// aka Interrupt Enable Register
//
// Mapped to 1F801802 bank 1 for write
// 
//  0-2 ENINT    Enable IRQ on respective INTSTS bits
//  3   ENBFEMPT Enable IRQ on BFEMPT
//  4   ENBFWRDY Enable IRQ on BFWRDY
//  5-7 -        Reserved (should be 0 when written, always 1 when read)
// 
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-write-bank-1-hintmsk
//
void CDROM::writeHINTMASK(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write to HINTMASK (interrupt enable) reg value %02X\n", val);

	m_hintmsk = val;
}

u8 CDROM::readHINTMASK() const
{
	u8 ret = m_hintmsk | 0xe0; // bits 7:5 are always 1 when read

	if (s_logCDROM)
		LOG_INFO("[CDROM] Read HINTMASK (interrupt enable) reg value %02X\n", ret);

	return ret;
}

// HCHPCTL (host chip control) register
// aka Request register
//
// Mapped to 1F801803 bank 0 for write
//
// Bit(s)
//  0-4 0    Not used (should be zero)
//  5   SMEN Want Command Start Interrupt on Next Command (0=No change, 1=Yes)
//  6   BFWR ...                                      1=prepare for writes to WRDATA
//  7   BFRD Request sector buffer read   Want Data   0=No/Reset Data Fifo, 1=Yes/Load Data Fifo i.e. prepare for reads from RDDATA
//
// n.b. All the docs are incorrect for BFRD. BFRD does NOT clear or reset the data FIFO, it just allows/disallows reads from it.
// Writing to BFRD does not reset the reading position (See EmuDev Discord playstation channel).

void CDROM::writeHCHPCTL(u8 val)
{
	m_hchpctl.val = val;
	m_hchpctl.unusedBits4_0 = 0; // Bits 4:0 are unused and should be zero

	if (s_logCDROM)
		LOG_INFO("[CDROM] Write HCHPCTL reg value %02X BFRD: %u BFWR: %u SMEN: %u\n", val, m_hchpctl.BFRD, m_hchpctl.BFWR, m_hchpctl.SMEN);

	// #TODO: psx-spx and CXD1199AQ datasheet seem to be in conflict about the meaning of this bit.
	HP_DEBUG_ASSERT(!m_hchpctl.SMEN, "CDROM sound map ADPCM playback not implemented");
}

// Used to configure the decoder for sound map XA-ADPCM playback.
//
// Sound Map mode allows to output XA-ADPCM from Main RAM (rather than from CDROM).
// 
//   0    S/M      Channel count   (0=mono, 1=stereo)
//   1    -        Reserved        (should be 0)
//   2    FS       Sample rate     (0=37800Hz, 1=18900Hz)
//   3    -        Reserved        (should be 0)
//   4    BITLNGTH Bits per sample (0=4bit, 1=8bit)
//   5    -        Reserved        (should be 0)
//   6    EMPHASIS Emphasis filter (0=off, 1=on)
//   7    -        Reserved        (should be 0)
//
// Does not affect playback of XA-ADPCM sectors from the disc). Uses the same format as the "codinginfo" field in XA sector headers.
// 
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801801-write-bank-2-ci
//
void CDROM::writeCI(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write CI reg value %02X  NOT IMPLEMENTED\n", val);

	HP_DEBUG_FATAL_ERROR("CDROM CI register not implemented");
}

// 0x1f801802 (write, bank 2): ATV0 (L->L volume)
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-write-bank-2-atv0-l-l-volume
void CDROM::writeATV0(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ATV0 reg value %02X (CD left -> left volume)\n", val);

	// After changing this registers, the CHNGATV flag in ADPCTL must be set.
	m_cdLeftLeftVolume_pending = val;
}

// 0x1f801803 (write, bank 2): ATV1 (L->R volume)
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801803-write-bank-2-atv1-l-r-volume
void CDROM::writeATV1(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ATV1 reg value %02X (CD left -> right volume)\n", val);

	// After changing this registers, the CHNGATV flag in ADPCTL must be set.
	m_cdLeftRightVolume_pending = val;
}

// 0x1f801801 (write, bank 3): ATV2 (R->R volume)
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801801-write-bank-3-atv2-r-r-volume
void CDROM::writeATV2(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ATV2 reg value %02X (CD right -> right volume)\n", val);

	// After changing this registers, the CHNGATV flag in ADPCTL must be set.
	m_cdRightRightVolume_pending = val;
}

// 0x1f801802 (write, bank 3): ATV3 (R->L volume)
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-write-bank-3-atv3-r-l-volume
void CDROM::writeATV3(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ATV3 reg value %02X (CD right -> left volume)\n", val);

	// After changing this registers, the CHNGATV flag in ADPCTL must be set.
	m_cdRightLeftVolume_pending = val;
}

// 0x1f801803 (write, bank 3): ADPCTL
// 
//   0    ADPMUTE Mute XA-ADPCM           (1=mute)
//   1-4  -       Reserved                (should be 0)
//   5    CHNGATV Apply ATV0-ATV3 changes (0=no change, 1=apply)
//   6-7  -       Reserved                (should be 0)
// 
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801803-write-bank-3-adpctl
//
void CDROM::writeADPCTL(u8 val)
{
	if (s_logCDROM)
		LOG_INFO("[CDROM] Write ADPCTL reg value %02X\n", val);

	m_xaMuted = val & 1;

	if (val & (1 << 5)) // CHNGATV
	{
		m_cdLeftLeftVolume = m_cdLeftLeftVolume_pending;
		m_cdLeftRightVolume = m_cdLeftRightVolume_pending;
		m_cdRightRightVolume = m_cdRightRightVolume_pending;
		m_cdRightLeftVolume = m_cdRightLeftVolume_pending;

		if (s_logCDROM)
			LOG_INFO("[CDROM]   Applied ATV (CD volume) changes: L->L %02X, L->R %02X, R->R %02X, R->L %02X\n",
				m_cdLeftLeftVolume, m_cdLeftRightVolume, m_cdRightRightVolume, m_cdRightLeftVolume);
	}
}

//
// Bank  0x1f801800  0x1f801801  0x1f801802  0x1f801803
// 0,2   HSTS        RESULT      RDDATA      HINTMSK
// 1,3   HSTS        RESULT      RDDATA      HINTSTS
//
u8 CDROM::ReadReg(unsigned int regIndex)
{
	// Registers 0, 1 and 2 are the same for all banks. Only register 3 differs.
	switch (regIndex)
	{
		case 0:
			return readHSTS();
		case 1:
			return readRESULT();
		case 2:
			return readRDDATA();
		case 3:
		{
			if (m_bank == 0 || m_bank == 2)
				return readHINTMASK();
			else // if (m_bank == 1 || m_bank == 3)
				return readHINTSTS();
		}
		default:
			HP_DEBUG_FATAL_ERROR("Read invalid CDROM register index %u", regIndex);
			return 0;
	}
}

u32 CDROM::ReadRDDATA()
{
	if (m_hchpctl.BFRD)
		return readSectorDataFIFOWord();
	else
		return 0;
}

// HSTS (host status) register
//
// CD Index (bank) / Status Register (Bit0-1 R/W, Bit2-7 Read Only)
//
// Mapped to address 1F801800h in all banks.
// 
// See:
// - https://problemkaputt.de/psxspx-cdrom-controller-i-o-ports.htm "Index/Status Register"
// - CXD1199AQ datasheet "HSTS"
//
u8 CDROM::readHSTS() const
{
	u8 val = 0;

	// Bit 7 BUSYSTS (busy status)
	// This is high when the host writes a command into the command register and low when the sub
	// CPU sets the CLRBUSY bit (bit 6) of the CLRCTL register.
	// EmuDev Discord playstation channel seems to indicate that BUSYSTS is cleared when the first response is complete.
	if (m_firstResponseInProgress)
		val |= (1 << 7);

	// Bit 6 DRQSTS (data request status)
	// Indicates to the host that the buffer memory data transfer request status is established. When
	// transferring data in the I/O mode, the host should confirm that this bit is high before accessing the
	// WRDATA or RDDATA register.
	// Data fifo empty      (0=Empty, 1=not empty) ;triggered after reading LAST byte
	// #TEMP: Return true (empty) unless reading is in progress.
	// #TODO: Fix this up to respect data FIFO state
	if (!m_pSectorDataBuffer->IsEmpty())
		val |= (1 << 6);

	// Bit 5 RSLRRDY (result read ready)
	// The result register is not empty when this bit is high. At this time, the host can read the result register.
	// This bit should go to zero as soon as the last byte has been read from the results/response FIFO
	if (m_responseCount > 0)
		val |= (1 << 5);
	
	// Bit 4 PRMWRDY (parameter write ready)
	// Zero when the parameter FIFO is full.
	if (m_paramCount < COUNTOF_ARRAY(m_params))
		val |= (1 << 4);

	// Bit 3 PRMEMPT (parameter empty)
	// The PARAMETER register is empty when this bit is high.
	if (m_paramCount == 0)
		val |= (1 << 3);

	// Bit 2 ADPBUSY (ADPCM busy)
	// Set when playing XA-ADPCM sound. Clear when XA-ADPCM fifo empty
	if (!m_pXABuffer->IsEmpty())
		val |= (1 << 2);

	// Bits 1:0 contain the bank index.
	val |= m_bank & 3;

	if (s_logCDROM)
		LOG_INFO("[CDROM] Read from HSTS register value %02X:%s%s%s%s%s%s bank %u\n",
			val,
			val & (1 << 7) ? " BUSYSTS" : "",
			val & (1 << 6) ? " DRQSTS" : "",
			val & (1 << 5) ? " RSLRRDY" : "",
			val & (1 << 4) ? " PRMWRDY" : "",
			val & (1 << 3) ? " PRMEMPT" : "",
			val & (1 << 2) ? " ADPBUSY" : "",
			val & 3);

	return val;
}

// HINTSTS (host interrupt status) register
//
// aka Interrupt Flag Register
//
// Mapped to 1F801803h banks 1 and 3 for read
//
// Bits 2:0 are the current interrupt level: 0b001 = INT1, 0b010 = INT2, 0b011 = INT3
// 
// Bit 3 BFEMPT (buffer empty)
//   The BFEMPT status is established when there is no more sector data in the buffer memory upon
//   completion of the sound map ADPCM decoding of one sector for sound map playback.
//
// Bit 4 BFWRDY (buffer write ready)
//   - See CXD1199AQ datasheet for info
//
// Bits 7:5 Reserved (always 1)
//
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-read-all-banks-rddata
// 
// #TODO: psx-spx seems to conflict itself here:
// - https://psx-spx.consoledev.net/iomap/#cdrom-registers-addressreadwriteindex says 1F801803h banks 1 and 3 is CD Interrupt Flag Register (R/W)
// - https://psx-spx.consoledev.net/cdromdrive/#cdrom-controller-io-ports table says 0x1f801803 banks 1 and 3 read is HINTSTS,
//   but bank 1 write is HCLRCTL and bank 3 write is ADPCTL
// 
u8 CDROM::readHINTSTS() const
{
	HP_DEBUG_ASSERT((m_hintsts & 0xe0) == 0xe0, "[CDROM] HINTSTS bits 7:5 should always be 1");

	if (s_logCDROM)
		LOG_INFO("[CDROM] Read HINTSTS value %08X (INT%u)\n", m_hintsts, m_hintsts & 0x7);

	return m_hintsts;
}

// RESULT
// 
// Read command response/result FIFO
//
u8 CDROM::readRESULT()
{
	const unsigned int slotIndex = m_responseReadIndex;
	u8 result = readResponseFIFO();

	if (s_logCDROM || s_logCDROMResponse)
		LOG_INFO("[CDROM] Read RESULT (response FIFO) register slot %u value %02X\n", slotIndex, result);

	return result;
}

// RDDATA
//
// After ReadS/ReadN commands have generated INT1, software must set the BFRD flag, then wait until DRQSTS is set.
// The datablock (disc sector) can be then read from this register
//
// RDDATA can be accessed with 8 bit or 16 bit reads.
// To read a 2048-byte sector, one can use: 2048 load-byte opcodes, or 1024 load halfword opcodes, or, more conventionally,
// a 512 word DMA transfer; the actual CDROM databus is only 8 bits wide, so the CPU's bus interface handles splitting the reads).
// #TODO: How are 16-bit reads managed from this register?
//
// https://psx-spx.consoledev.net/cdromdrive/#0x1f801802-read-all-banks-rddata
//
u8 CDROM::readRDDATA()
{
	u8 val;

	// Undocumented: Data cannot be read from the FIFO when the HCHPCTL BFRD bit is not set
	if (m_hchpctl.BFRD)
	{
		// Return value from sector data buffer FIFO
		val = readSectorDataFIFOByte();

		if (s_logCDROM) // #TODO: May want to use s_logCDROMDMA here to avoid lots of spam
			LOG_INFO("[CDROM] RDDATA (data FIFO) read value %02X\n", val);
	}
	else
	{
		val = 0; // #TODO: Could possibly return last value read?
	}

	return val;
}

void CDROM::InsertDisc(const CD& cd)
{
	HP_DEBUG_ASSERT(!m_pCD, "Should logic require disc is ejected before inserting a new one?");

	m_pCD = &cd;

	// Close the CD "shell" (lid/door)
	m_shellOpen = false;

	// By *not* turning on the motor automatically here when a disc is inserted, I would expect
	// the Init command to be issued earlier, because it enables the motor. i.e. before SetLoc, SeekL, ReadN
	// However this doesn't seem to happen and the disc is read even when stat.SpindleMotor bit is false!
	// #TODO: How does read from disc occur even when stat.SpindleMotor is not set?
	m_stat.ShellOpen = m_shellOpen;
	m_stat.SpindleMotor = true; // #TODO: I think this should be false, but this bit needs to get set somehow!

	// Spin up the motor to full speed immediately.
	// #TODO: Does motor spin up time need to be simulated?
	m_spinningUp = false;

	m_stopped = true;

	if (s_logCDROM)
		LOG_INFO("[CDROM] Disc inserted. Shell (door/lid) closed.\n");
}

void CDROM::EjectDisc()
{
	HP_DEBUG_ASSERT(m_pCD, "No CD to eject");
	m_pCD = nullptr;

	// #TODO: Fire INT5 when lid opened i.e. disc ejected
	// "All interrupts are always fired in response to a command with the exception of INT5, which may also be triggered at any time by opening the lid."
	// Currntly the application asserts if disc is ejected while loading!

	if (s_logCDROM)
		LOG_INFO("[CDROM] Disc ejected\n");

	// #TODO: Stop motor and reset head when eject disk?
	m_stat.SpindleMotor = false;
	m_stopped = true;
	m_headLBA = 0;
}

//
// Nop - Command 01h -> INT3(stat)
// https://psx-spx.consoledev.net/cdromdrive/#nop-command-01h-int3stat
//
void CDROM::executeNopCommand01()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Nop\n");

	// Nop first response time is shorter if motor is stopped.
	// See https://psx-spx.consoledev.net/cdromdrive/#first-response
	// #TODO: !m_spindleMotorOn may not mean stopped if still spinning.
	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Nop_FirstResponseCallback, this, m_stat.SpindleMotor ? 0xc4e1 : 0x5cf4, "Nop1");
	m_firstResponseInProgress = true;
}

void CDROM::Nop_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeNop_FirstResponse();
}

void CDROM::executeNop_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	if (m_paramCount != 0)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Nop/GetStat first response. Wrong number of parameters: expected 0 got %u\n", m_paramCount);

		errorINT5(kCDROM_Error_WrongNumberOfParameters);
		return;
	}

	// #TODO: If stat bits 0 (Error) or 2 (Seek Error) are set then INT5 instead
	// https://psx-spx.consoledev.net/cdromdrive/#status-code-stat
	HP_DEBUG_ASSERT(!m_stat.Error && !m_stat.SeekError, "CDROM Nop command 01h INT5 error not implemented");

	// If the shell (physical CD cover, *not* OS shell) is closed, then bit 4 is automatically reset to zero after reading stat with the Nop command.
	if (m_shellOpen && m_stat.ShellOpen)
		m_stat.ShellOpen = false;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Nop1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	writeResponseFIFO(m_stat.val);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);
}

// Setloc - Command 02h,amm,ass,asect --> INT3(stat)
//
// Sets the seek target - but without yet starting the seek operation.
// The actual seek is invoked by certain commands:
// - SeekL (Data) and SeekP (Audio) are doing plain seeks (and do Pause after completion).
// - ReadN/ReadS are similar to SeekL (and do start reading data after the seek operation).
// - Play is similar to SeekP (and does start playing audio after the seek operation).
// 
// The amm,ass,asect parameters refer to the entire disc (not to the current track).
// Note that each of these parameters is encoded as BCD values, not binary.
// To seek to a specific location within a specific track, use GetTD to get the start address
// of the track, and add the desired time offset to it.
// 
// https://psx-spx.consoledev.net/cdromdrive/#setloc-command-02hammassasect-int3stat
void CDROM::executeSetLocCommand02()
{
	HP_ASSERT(m_paramCount == 3, "[CDROM] Unexpected number of parameters for SetLoc Command 02h");
	u8 ammBCD = m_params[0]; // BCD minutes
	u8 assBCD = m_params[1]; // BCD seconds
	u8 asectBCD = m_params[2]; // BCD sector/frame/fragment index
	m_paramCount = 0; // Remove params from FIFO

	// Don't store the target location MSF values ins BCD format because would just lead to bugs!
	// Convert to decimal immediately.
	m_targetLocMinutes = BCDtoDecimal(ammBCD);
	m_targetLocSeconds = BCDtoDecimal(assBCD);
	m_targetLocFrames = BCDtoDecimal(asectBCD);
	m_targetLBA = CD::MSFtoLBA(m_targetLocMinutes, m_targetLocSeconds, m_targetLocFrames);

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SetLoc MSF: %02u:%02u:%02u LBA: 0x%08X (%u)\n", m_targetLocMinutes, m_targetLocSeconds, m_targetLocFrames, m_targetLBA, m_targetLBA); // MSF conventionally printed in decimal

	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(SetLoc_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "SetLoc1");
	m_firstResponseInProgress = true;
}

void CDROM::SetLoc_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeSetLoc_FirstResponse();
}

void CDROM::executeSetLoc_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SetLoc1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	m_setLocPending = true;

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);
}

// Play - Command 03h (,track) --> INT3(stat) --> optional INT1(report bytes)
//
// Starts CD Audio Playback.
//
// The parameter is optional, if there's no parameter given (or if it is 00h), then play either starts at
// Setloc position (if there was a pending unprocessed Setloc), or otherwise starts at the current location
// e.g. the last point seeked, or the current location of the current song; if it was already playing).
//
// For a disk with N songs, Parameters 1..N are starting the selected track. Parameters N+1..99h are
// restarting the begin of current track.
//
// The motor is switched off automatically when Play reaches the end of the disk, and INT4(stat) is generated (with stat.bit7 cleared).
// 
// The track parameter seems to be ignored when sending Play shortly after power-up (ie. when the drive hasn't yet read the TOC).
// 
// Play is almost identical to CdlReadS, believe it or not. The main difference is that this does not trigger a completed read IRQ.
// CdlPlay may be used on data sectors. However, all sectors from data tracks are treated as 00, so no sound is played.
// As CdlPlay is reading, the audio data appears in the sector buffer, but is not reliable.
// 
// https://psx-spx.consoledev.net/cdromdrive/#play-command-03h-track-int3stat-optional-int1report-bytes
//
void CDROM::executePlayCommand03()
{
	// One optional parameter!
	HP_ASSERT(m_paramCount <= 1);
	if (m_paramCount == 0)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Play\n");
	}
	else // if (m_paramCount == 1)
	{
		u8 trackNum = m_params[0]; // #TODO: Is this BCD?
		m_paramCount = 0; // Remove params from FIFO

		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Play %02u\n", trackNum);

		HP_FATAL_ERROR("Not implemented");
	}

	HP_ASSERT(m_readSectorEvent == 0, "A read or play is already in progress. Should it be cancelled?");

	// #TODO: Does Play first response have specific timing?
	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Play_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Play1");
	m_firstResponseInProgress = true;
}

void CDROM::Play_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executePlay_FirstResponse();
}

void CDROM::executePlay_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Play1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// I don't think it makes sense for stat Play bit to be set in the first response, but I may be wrong.
	// I also don't think Seek should be set.
	writeResponseFIFO(m_stat.val);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);

	// Schedule the second response, which will respond when the first data is ready.

	// Calling Play after SetLoc without a SeekL causes the seek to happen automatically (undocumented)
	unsigned int secondResponseTime;
	bool seek = m_setLocPending && (m_headLBA != m_targetLBA);
	if (seek)
	{
		// #TODO: Is Play is sensitive to INT1 timings?
		secondResponseTime = kSeekTimeCycles;

		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Play will seek to pending SetLoc\n");

		HP_DEBUG_ASSERT(!m_stat.Play && !m_stat.Read, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
		m_stat.Seek = true;  // *** This has to be set true to prevent the lock up! **
	}
	else
		secondResponseTime = kPlaySectorTimeCycles;

	// Clear any unused data in the XA buffer
	resetXABuffer();

	HP_DEBUG_ASSERT(m_readSectorEvent == 0);
	m_readSectorEvent = m_scheduler.Schedule(Play_ReadSectorCallback, this, secondResponseTime, "PlayReadSector");
}

void CDROM::Play_ReadSectorCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_readSectorEvent = 0;
	pCDROM->Play_ReadSector();
}

void CDROM::Play_ReadSector()
{
	// Finish implicit seek if active
	if (m_setLocPending)
	{
		m_stat.Seek = false;  // To get EWJ demo mode not to issue a Pause and freeze up, Seek must be left set!
		m_headLBA = m_targetLBA;
		m_setLocPending = false;
	}

	if (!m_stat.Play)
	{
		HP_DEBUG_ASSERT(!m_stat.Read && !m_stat.Seek, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
		m_stat.Play = true;
	}

	// The drive reads a whole sector at once. Not a byte at a time like the Amiga floppy disk controller!
	const u8* pDiscImage = m_pCD->GetData();
	unsigned int discOffsetBytes = m_headLBA * CD::kSectorSizeBytes;
	const u8* pSector = pDiscImage + discOffsetBytes;

	// During Play, only bit 7,2,1 of Setmode are used, all other Setmode bits are ignored, including SectorSize
	// so full 2352 bytes of sector data are read as CDDA data.
	//
	//  7   Speed       (0=Normal speed, 1=Double speed)
	//  2   Report      (0=Off, 1=Enable Report-Interrupts for Audio Play)
	//  1   AutoPause   (0=Off, 1=Auto Pause upon End of Track) ;for Audio Play
	//  0   CDDA        (0=Off, 1=Allow to Read CD-DA Sectors; ignore missing EDC)
	// 
	// The drive is reading in CDDA mode, not CDROM mode, so the sectors do not have headers - they are
	// pure CDDA data.

	if (s_logCDROM || s_logCDROMCommands)
	{
		unsigned int m, s, f;
		CD::LBAtoMSF(m_headLBA, m, s, f);
		LOG_INFO("[CDROM] Play read sector MSF: %02u:%02u:%02u LBA: 0x%08X (%u) %08X bytes at offset %08X\n", // MSF conventionally printed in decimal
			m, s, f, m_headLBA, m_headLBA, CD::kSectorSizeBytes, (u32)(ptrdiff_t)(pSector - pDiscImage));
	}

	// n.b. We don't test the HCHPCTL BFWR flag before writing to the FIFO here. That is only for writes to the data FIFO via the WRDATA register, which I think is unused on PSX.
	HP_ASSERT(pSector + CD::kSectorSizeBytes <= pDiscImage + m_pCD->GetSizeBytes(), "Reading past end of disc image");

	// Copy data to CDDA buffer

	// #TEST: Clear the buffer before every read
	// This means the buffer is not being treated as a true ring-buffer
	m_pCDDABuffer->Reset();

	// n.b. We don't test the HCHPCTL BFWR flag before writing to the FIFO here. That is only for writes to the data FIFO via the WRDATA register, which I think is unused on PSX.
	HP_ASSERT(pSector + CD::kSectorSizeBytes <= pDiscImage + m_pCD->GetSizeBytes(), "Reading past end of disc image");
	HP_DEBUG_ASSERT(m_pCDDABuffer->GetFreeSpaceBytes() >= CD::kSectorSizeBytes, "Insufficient room in sector buffer"); // #TODO: Should this wrap?
	m_pCDDABuffer->Write(pSector, CD::kSectorSizeBytes);

	unsigned int trackIndex = findCurrentTrackIndex(); // #TODO: Cache this

	// Respect m_mode.AutoPause https://psx-spx.consoledev.net/cdromdrive/#autopause-int4stat
	if (m_mode.AutoPause) // Issue INT4(stat) and PAUSE at end of TRACK
	{
		if (m_headLBA == m_pCD->GetTrackFinalLBA(trackIndex))
		{
			// INT4(stat) and PAUSE
			generateINT(4); // DataEnd
			m_stat.Play = false;

			// Return early so don't advance head or reschedule another sector read
			return;
		}
	}
	else // Issue INT4(stat) and STOP at end of DISC
	{
		if (m_headLBA == m_pCD->GetSizeSectors() - 1)
		{
			// INT4(stat) and PAUSE
			generateINT(4); // DataEnd
			m_stat.Play = false;

			// Return early so don't advance head or reschedule another sector read
			return;
		}
	}

	if (m_mode.Report)
	{
		// Play data response INT1: status, track, index, (r)min, (r)sec, (r)frame, peakl, peakh
		// #TODO: The interrupt isn't generated on ALL sectors. See https://psx-spx.consoledev.net/cdromdrive/#report-int1stattrackindexmmammss80hasssectasectpeaklopeakhi

		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] ReadSector Report INT1(status, track, index, (r)min, (r)sec, (r)frame, peakl, peakh)\n");

		unsigned int m, s, f;
		CD::LBAtoMSF(m_headLBA, m, s, f);
		writeResponseFIFO(m_stat.val);
		writeResponseFIFO((u8)trackIndex);
		writeResponseFIFO(1); // #TODO: Get this from CDROM
		writeResponseFIFO((u8)m);
		writeResponseFIFO((u8)s);
		writeResponseFIFO((u8)f);
		writeResponseFIFO(/*peakl*/0); // #TODO: Calculate this
		writeResponseFIFO(/*peakh*/0); // #TODO: Calculate this
		HP_VERIFY(generateINT(1), "INT1 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
	}

	// One full sector is always read at a time
	// Increment head position one sector. One LBA is one sector.
	HP_ASSERT(m_headLBA + 1 < m_pCD->GetSizeSectors(), "#TODO: Switch off motor when reach end of disk");
	m_headLBA++;

	// Keep playing
	// The INT1 rate needs to be precise for CD-DA and CD-XA Audio streaming, exact clock cycle
	// values should be: SystemClock*930h/4/44100Hz for Single Speed (and half as much for Double Speed)
	HP_DEBUG_ASSERT(m_readSectorEvent == 0);
	m_readSectorEvent = m_scheduler.Schedule(Play_ReadSectorCallback, this, kPlaySectorTimeCycles, "PlayReadSector");
}

// ReadN - Command 06h --> INT3(stat) --> INT1(stat) --> datablock
//
// Read with retry.
//
// The command responds once with "stat,INT3", and repeatedly sends "stat,INT1 --> datablock".
// This continues even after a successful read has occured; use the Pause command to terminate the repeated INT1 responses.
//
// A normal CDROM access (such like reading a file) consists of three commands:
//
//     Setloc -> Read -> Pause
// 
// n.b. Read performs a seek if a SeekL was not performed expicitely.
// 
// https://psx-spx.consoledev.net/cdromdrive/#readn-command-06h-int3stat-int1stat-datablock
//
void CDROM::executeReadNCommand06()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for ReadN");

	if (s_logCDROM || s_logCDROMCommands)
	{
		unsigned int m, s, f;
		CD::LBAtoMSF(m_headLBA, m, s, f);
		LOG_INFO("[CDROM] ReadN. Current location MSF: %02u:%02u:%02u LBA: 0x%08X (%u)\n", m, s, f, m_headLBA, m_headLBA); // MSF conventionally printed in decimal
	}

	cancelReadOrPlay(); // Required by Ape Escape
	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(ReadN_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "ReadN1");
	m_firstResponseInProgress = true;
}

// static
void CDROM::ReadN_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeReadN_FirstResponse();
}

void CDROM::executeReadN_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] ReadN1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// I don't think it makes sense for stat Read bit to be set in the first response, but I may be wrong
	writeResponseFIFO(m_stat.val);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);

	// Clear any unused data in the XA buffer
	resetXABuffer();

	// Schedule the second response, which will respond when the first data is ready.
	// n.b. ReadN is sensitive to INT1 timings

	// Calling ReadN after SetLoc without a SeekL causes the seek to happen automatically (undocumented)
	unsigned int secondResponseTime;
	bool seek = m_setLocPending && (m_headLBA != m_targetLBA);
	if (seek)
	{
		if (s_logCDROM)
			LOG_INFO("[CDROM]   ReadN will seek because SetLoc pending\n");

		secondResponseTime = kSeekTimeCycles;

		HP_DEBUG_ASSERT(!m_stat.Play && !m_stat.Read, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
		m_stat.Seek = true;
	}
	else
		secondResponseTime = m_mode.doubleSpeed ? kReadNTimeCycles_DoubleSpeed : kReadNTimeCycles_SingleSpeed;

	HP_DEBUG_ASSERT(m_readSectorEvent == 0);
	m_readSectorEvent = m_scheduler.Schedule(ReadSectorCallback, this, secondResponseTime, "ReadN2");
}

void CDROM::ReadSectorCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_readSectorEvent = 0;
	pCDROM->readSector();
}

void CDROM::readSector()
{
	// Finish implicit seek if active
	if (m_setLocPending)
	{
		m_stat.Seek = false;
		m_headLBA = m_targetLBA;
		m_setLocPending = false;
	}

	if (!m_stat.Read)
	{
		HP_DEBUG_ASSERT(!m_stat.Play && !m_stat.Seek, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
		m_stat.Read = true;
	}

	HP_ASSERT(m_pCD);

	// The drive reads a whole sector at once. Not a byte at a time like the Amiga floppy disk controller!
	const unsigned int lba = m_headLBA;
	const u8* pDiscImage = m_pCD->GetData();
	unsigned int discOffsetBytes = lba * CD::kSectorSizeBytes;
	const u8* pSector = pDiscImage + discOffsetBytes;

	// #TODO: Assert correct sector is being read by decoding header. Need to skip sync bytes first. See "encode_sector" pseudocode at https://psx-spx.consoledev.net/cdromformat/#cdrom-sector-encoding 
	// sector[000h] = 00h,FFh,FFh,FFh,FFh,FFh,FFh,FFh,FFh,FFh,FFh,00h
	// sector[00ch] = BCD minutes  [0,7x]
	// sector[00dh] = BCD seconds  [0,59]
	// sector[00eh] = BCD frames   [0,74]
#if HP_DEBUG_ASSERTS_ENABLED
	unsigned int off;
	for (off = 0; off < CD::kSectorSyncSizeBytes; off++)
	{
		HP_DEBUG_ASSERT(pSector[off] == kSectorSyncBytes[off]);
	}
	u8 minutes = BCDtoDecimal(pSector[off++]);
	u8 seconds = BCDtoDecimal(pSector[off++]);
	u8 frames = BCDtoDecimal(pSector[off++]);
	unsigned int sectorLBA = CD::MSFtoLBA(minutes, seconds, frames);
	HP_DEBUG_ASSERT(sectorLBA == lba, "Incorrect sector read from disc.");

	u8 mode = BCDtoDecimal(pSector[off++]);
	HP_DEBUG_ASSERT(mode == 2, "Expect Mode 2 sector");

	// #TODO: Is there any useful data in the 8 byte subheader?
#endif

	unsigned int dataSizeBytes;
	const u8* pData;
	if (m_mode.SectorSize == 0)
	{
		// Read data only: 800h bytes
		dataSizeBytes = CD::kXA_Mode2Form1SectorDataSizeBytes; // 800h = 2048

		// Only copy the data. Skip the sync bytes, address, mode and subheader bytes.
		pData = pSector + CD::kMode2SectorDataOffset; // skip 24 bytes
	}
	else // if (m_mode.SectorSize == 1)
	{
		// Read whole sector, excluding the sync bytes
		dataSizeBytes = CD::kSectorSizeExcludingSyncBytes;
		pData = pSector + CD::kSectorSyncSizeBytes; // skip 12 bytes
	}

	// One full sector is always read at a time
	// Increment head position one sector. One LBA is one sector.
	m_headLBA++;

	bool generateINT1 = true; // INT1 are only generated for data tracks the CPU needs to be notified of

	if (m_mode.XA_ADPCM) // this flag controls whether XA-ADPCM sectors are sent to SPU Audio Input, which in this implementation means they are buffered.
	{
		// https://psx-spx.consoledev.net/cdromformat/#cdrom-xa-subheader-file-channel-interleave

		const u8* pSubheader = pSector + CD::kMode2SectorSubHeaderOffset;

		// File and channel number are used for audio/video interleave.
		unsigned int fileNumber = pSubheader[0]; // [0,FF]
		unsigned int channelNumber = pSubheader[1] & 0xf; // [00,1F]

		if (!m_mode.XA_Filter || (fileNumber == m_xaFilterFileNumber && channelNumber == m_xaFilterChannelNumber))
		{
			//  0   End of Record (EOR) (all Volume Descriptors, and all sectors with EOF)
			//  1   Video     ;\Sector Type (usually ONE of these bits should be set)
			//  2   Audio     ; Note: PSX .STR files are declared as Data (not as Video)
			//  3   Data      ;/
			//  4   Trigger           (for application use)
			//  5   Form2             (0=Form1/800h-byte data, 1=Form2, 914h-byte data)
			//  6   Real Time (RT)
			//  7   End of File (EOF) (or end of Directory/PathTable/VolumeTerminator)
			// 
			// https://psx-spx.consoledev.net/cdromformat/#3rd-subheader-byte-submode-sm
			u8 submode = pSubheader[2];

			// For XA-ADPCM audio sectors:
			//  0-1 Mono/Stereo     (0=Mono, 1=Stereo, 2-3=Reserved)
			//  2-3 Sample Rate     (0=37800Hz, 1=18900Hz, 2-3=Reserved)
			//  4-5 Bits per Sample (0=Normal/4bit, 1=8bit, 2-3=Reserved)
			//  6   Emphasis        (0=Normal/Off, 1=Emphasis)  #TODO: What is this?
			//  7   Reserved        (0)
			// 
			// https://psx-spx.consoledev.net/cdromformat/#4th-subheader-byte-codinginfo-ci
			u8 codingInfo = pSubheader[3];

			if (submode & 0x4) // Audio  #TODO: Chicho says only decode if submode Real Time bit 6 (0x40) is also set
			{
				HP_DEBUG_ASSERT(submode & 0x40, "Real Time bit is not set");
				if (s_logCDROM || s_logCDROMCommands)
				{
					unsigned int m, s, f;
					CD::LBAtoMSF(lba, m, s, f);
					LOG_INFO("[CDROM] ReadSector XA-ADPCM MSF: %02u:%02u:%02u LBA: 0x%08X (%u) %08X bytes at offset %08X File: %02X Channel: %02X Submode: %02X (audio) CI: %02X %s %s %s\n",
						m, s, f, lba, lba, dataSizeBytes, (u32)(ptrdiff_t)(pData - pDiscImage),
						fileNumber, channelNumber, submode,
						codingInfo,
						(codingInfo & 0x01) ? "Stereo" : "Mono",
						(codingInfo & 0x10) ? "8-bit" : "4-bit",
						(codingInfo & 0x04) ? "18900 Hz" : "37800 Hz");
				}

				HP_ASSERT((codingInfo & 0x10) == 0, "Only 4-bits per sample currently supported"); // See https://psx-spx.consoledev.net/cdromformat/#xa-adpcm-data-words-32bit-little-endian

				decodeXAADPCMSector(pSector, codingInfo);

				// "If XA-ADPCM (and/or XA-Filter) is enabled via Setmode, then INT1 is generated only for non-ADPCM sectors."
				// IRQs shouldn't fire for for XA sectors because they have been processed internally, so no need for CPU to do anything with the data.
				// XA sectors should not be added to the to data buffer.
				generateINT1 = false;
			}
		}
	}

	if (generateINT1)
	{
		if (s_logCDROM || s_logCDROMCommands)
		{
			unsigned int m, s, f;
			CD::LBAtoMSF(lba, m, s, f);
			LOG_INFO("[CDROM] ReadSector INT1(stat) = INT1(%02X) sector MSF: %02u:%02u:%02u LBA: 0x%08X (%u) %08X bytes at offset %08X\n", // MSF conventionally printed in decimal
				m_stat.val, m, s, f, lba, lba, dataSizeBytes, (u32)(ptrdiff_t)(pData - pDiscImage));
		}

		// #TEST: Clear the buffer before every read
		// I think that in the real hardware, when a Pause command cancels Read in the second response, the previous read overruns by the delay
		// of about 5 sectors, which ends up in the sector buffer. When a new read starts, this data would needs to be cleared.
#if 1
	// This means the buffer is not being treated as a true ring-buffer
		m_pSectorDataBuffer->Reset();
#endif
		// n.b. We don't test the HCHPCTL BFWR flag before writing to the FIFO here. That is only for writes to the data FIFO via the WRDATA register, which I think is unused on PSX.
		HP_ASSERT(pData + dataSizeBytes <= pDiscImage + m_pCD->GetSizeBytes(), "Reading past end of disc image");
		HP_DEBUG_ASSERT(m_pSectorDataBuffer->GetFreeSpaceBytes() >= dataSizeBytes, "Insufficient room in sector buffer"); // #TODO: Should this wrap?
		m_pSectorDataBuffer->Write(pData, dataSizeBytes);

#if CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
		unsigned int m, s, f;
		CD::LBAtoMSF(lba, m, s, f);
		SafeSnprintf(m_debugSectorDataBufferDescription, sizeof(m_debugSectorDataBufferDescription), "MSF: %02u:%02u:%02u LBA: 0x%08X (%u) %08X bytes from disc offset %08X",
			m, s, f, lba, lba, dataSizeBytes, (u32)(ptrdiff_t)(pData - pDiscImage));
#endif
		// Generate INT1 when response in place to communicate to host machine that data is ready for read.
		// Only write to response FIFO if INT was generated. If queued, the response is deferred.
		if (generateINT(1))
			writeResponseFIFO(m_stat.val); // INT1(stat)
	}

	// ReadN keeps reading until Paused
	// #TODO: What would happen if mode is changed while reading in progress?
	HP_DEBUG_ASSERT(m_readSectorEvent == 0);
	m_readSectorEvent = m_scheduler.Schedule(ReadSectorCallback, this, m_mode.doubleSpeed ? kReadNTimeCycles_DoubleSpeed : kReadNTimeCycles_SingleSpeed, "ReadSector");
}

void CDROM::executeReadSCommand1B()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for ReadS");

	if (s_logCDROM || s_logCDROMCommands)
	{
		unsigned int m, s, f;
		CD::LBAtoMSF(m_headLBA, m, s, f);
		LOG_INFO("[CDROM] ReadS. Current location MSF: %02u:%02u:%02u LBA: 0x%08X (%u)\n", m, s, f, m_headLBA, m_headLBA); // MSF conventionally printed in decimal
	}

	cancelReadOrPlay();
	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(ReadS_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "ReadS1");
	m_firstResponseInProgress = true;
}

// static
void CDROM::ReadS_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeReadS_FirstResponse();
}

void CDROM::executeReadS_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] ReadS1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// I don't think it makes sense for stat Read bit to be set in the first response, but I may be wrong
	writeResponseFIFO(m_stat.val);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);

	// Clear any unused data in the XA buffer
	resetXABuffer();

	// Schedule the second response, which will respond when the first data is ready.
	// n.b. ReadS is sensitive to INT1 timings

	// Calling ReadS after SetLoc without a SeekL causes the seek to happen automatically (undocumented)
	unsigned int secondResponseTime;
	bool seek = m_setLocPending && (m_headLBA != m_targetLBA);
	if (seek)
	{
		if (s_logCDROM)
			LOG_INFO("[CDROM]   ReadS will seek because SetLoc pending\n");

		secondResponseTime = kSeekTimeCycles;

		HP_DEBUG_ASSERT(!m_stat.Play && !m_stat.Read, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
		m_stat.Seek = true;
	}
	else
		secondResponseTime = m_mode.doubleSpeed ? kReadSTimeCycles_DoubleSpeed : kReadSTimeCycles_SingleSpeed;

	HP_DEBUG_ASSERT(m_readSectorEvent == 0);
	m_readSectorEvent = m_scheduler.Schedule(ReadSectorCallback, this, secondResponseTime, "ReadS2");
}

// MotorOn - Command 07h --> INT3(stat) --> INT2(stat)
// Activates the drive motor, works ONLY if the motor was off (otherwise fails with INT5(stat,20h); that error code would normally indicate "wrong number of parameters", but means "motor already on" in this case).
// Commands like Read, Seek, and Play are automatically starting the Motor when needed (which makes the MotorOn command rather useless, and it's rarely used by any games).
// Myth: Older homebrew docs are referring to MotorOn as "Standby", claiming that it would work similar as "Pause", that is wrong: the command does NOT pause anything (if the motor is on, then it does simply trigger INT5, but without pausing reading or playing).
// Note: The game "Nightmare Creatures 2" does actually attempt to use MotorOn to "pause" after reading files, but the hardware does simply ignore that attempt (aside from doing the INT5 thing).
void CDROM::executeMotorOnCommand07()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for MotorOn");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] MotorOn\n");

	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(MotorOn_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "MotorOn1");
	m_firstResponseInProgress = true;
}

void CDROM::MotorOn_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeMotorOn_FirstResponse();
}

void CDROM::executeMotorOn_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] MotorOn1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(MotorOn_SecondResponseCallback, this, kDefaultFirstResponseDurationCycles, "MotorOn2");
}

void CDROM::MotorOn_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executeMotorOn_SecondResponse();
}

void CDROM::executeMotorOn_SecondResponse()
{
	m_stopped = false;
	m_spinningUp = false;
	m_stat.SpindleMotor = true;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] MotorOn2 INT2(stat) = INT2(%02X)\n", m_stat.val);

	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}

// Stop - Command 08h --> INT3(stat) --> INT2(stat)
//
// Stops motor with magnetic brakes. Stops within a second or so, unlike power-off where it'd keep spinning.
// for about 10 seconds)
// 
// The drive head is moved to the begin of the first track.
// 
// Official way to restart is command 0Ah, but almost any command will restart it.
// 
// The first response returns the current status, already with bit 5 [sic] cleared. HP: Assume this means bit 7 Play, rather than bit 5 Read
// 
// The second response returns the new status with bit 1 SpindleMotor cleared.
// 
// https://psx-spx.consoledev.net/cdromdrive/#stop-command-08h-int3stat-int2stat
void CDROM::executeStopCommand08()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for Stop");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Stop\n");

	cancelReadOrPlay();

	// n.b. We don't set Read to false yet. That is done later.

	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Stop_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Stop1");
	m_firstResponseInProgress = true;
}

void CDROM::Stop_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeStop_FirstResponse();
}

void CDROM::executeStop_FirstResponse()
{
	// The first response returns the current status, already with bit 5 [sic] cleared. HP: Assume this means both bit 5 Read and bit 7 Play
	if (m_stat.Read)
		m_stat.Read = false;
	if (m_stat.Play)
		m_stat.Play = false;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Stop1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	// Pause second response timings from https://psx-spx.consoledev.net/cdromdrive/#second-response
	//                         Average   Min       Max
	//  Stop (single speed)    0d38acah  0c3bc41h..0da554dh
	//  Stop (double speed)    18a6076h  184476bh..192b306h
	//  Stop (when stopped)    0001d7bh  0001ce8h..0001eefh
	unsigned int responseTime;
	if (m_stopped)
		responseTime = 0x1d7b;
	else if (m_mode.doubleSpeed)
		responseTime = 0x18a6076;
	else // single speed
		responseTime = 0xd38aca;

	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(Stop_SecondResponseCallback, this, responseTime, "Stop2");
}

void CDROM::Stop_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executeStop_SecondResponse();
}

void CDROM::executeStop_SecondResponse()
{
	// The second response returns the new status with bit 1 SpindleMotor cleared.
	m_stopped = true;
	m_stat.SpindleMotor = false;

	// The drive head is moved to the begin of the first track.
	m_headLBA = 0;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Stop2 INT2(stat) = INT2(%02X)\n", m_stat.val);

	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}

// Pause - Command 09h --> INT3(stat) --> INT2(stat)
//
// Aborts Reading and Playing, the motor is kept spinning, and the drive head maintains the current
// location within reasonable error.
// 
// The first response returns the current status (still with bit 5 set if a Read command was active),
// the second response returns the new status (with bit5 cleared).
// 
// https://psx-spx.consoledev.net/cdromdrive/#pause-command-09h-int3stat-int2stat
//
void CDROM::executePauseCommand09()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for Pause");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Pause\n");

	// n.b. We don't set Read or Play status bits to false yet. That is done later.

	// Pause aborts both CDROM reading and CDDA playing.
	cancelReadOrPlay();

	// Street Fighter Alpha 3 requests a Pause while a Pause is already in progress. The pre-existing Pause needs to be cancelled.
	// In Duckstation this seems to be the second response, but here it seems to be the first response.
	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Pause_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Pause1");
	m_firstResponseInProgress = true;
}

void CDROM::Pause_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executePause_FirstResponse();
}

void CDROM::executePause_FirstResponse()
{
	// The first response returns the current status (still with bit 5 set if a Read command was active),

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Pause1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	// Pause aborts both CDROM reading and CDDA playing, so don't rResume any suspended Read/Play operations

	// Pause second response timings from https://psx-spx.consoledev.net/cdromdrive/#second-response
	//                         Average   Min       Max
	//  Pause (single speed)   021181ch  020eaefh..0216e3ch ;\time equal to
	//  Pause (double speed)   010bd93h  010477Ah..011B302h ;/about 5 sectors
	//  Pause (when paused)    0001df2h  0001d25h..0001f22h
	unsigned int responseTime;
	bool paused = !m_stat.Read && !m_stat.Play; // #TODO: Is this the correct logic to determine if "paused"
	if (paused)
		responseTime = 0x1df2;
	else if (m_mode.doubleSpeed)
		responseTime = 0x10bd93;
	else // single speed
		responseTime = 0x21181c;

	// Duckstation seems to show that a GetStat (Nop) between Pause1 and Pause2 does not have the Read bit set. Just SpindleMotorOn
	if (m_stat.Read)
		m_stat.Read = false;
	if (m_stat.Play)
		m_stat.Play = false;

	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(Pause_SecondResponseCallback, this, responseTime, "Pause2");
}

void CDROM::Pause_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executePause_SecondResponse();
}

void CDROM::executePause_SecondResponse()
{
	// the second response returns the new status with bit 5 cleared
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Pause2 INT2(stat) = INT2(%02X)\n", m_stat.val);

	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}

// Stop Play and Read if active by cancelling any outstanding callbacks.
void CDROM::cancelReadOrPlay()
{
	if (m_readSectorEvent != 0)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Cancelling read/play sector event %s\n", m_scheduler.GetEventDebugName(m_readSectorEvent));
		m_scheduler.Cancel(m_readSectorEvent);
		m_readSectorEvent = 0;
	}
}

// Init - Command 0Ah --> INT3(stat) --> INT2(stat)
// Multiple effects at once. Sets mode=20h, activates drive motor, Standby, abort all commands.
// https://psx-spx.consoledev.net/cdromdrive/#init-command-0ah-int3stat-int2stat
void CDROM::executeInitCommand0A()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for Init");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Init\n");

	// Abort all commands
	// This includes any Reads or Plays
	if (m_commandFirstResponseEvent)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("Cancelling command first response for %s\n", m_scheduler.GetEventDebugName(m_commandFirstResponseEvent));
		m_scheduler.Cancel(m_commandFirstResponseEvent);
		m_commandFirstResponseEvent = 0;
	}

	cancelCommandSecondResponse();
	cancelReadOrPlay();

	m_commandFirstResponseEvent = m_scheduler.Schedule(Init_FirstResponseCallback, this, kInitFirstResponseTimeCycles, "Init1");
	m_firstResponseInProgress = true;
}

void CDROM::Init_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeInit_FirstResponse();
}

void CDROM::executeInit_FirstResponse()
{
	// The first response returns the current status (still with bit 5 set if a Read command was active),

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Init1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	// n.b. Init aborts all commands, so don't Resume any Read/Play operations that were suspended; clear them instead

	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(Init_SecondResponseCallback, this, kInitSecondResponseTimeCycles, "Init2");
}

void CDROM::Init_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executeInit_SecondResponse();
}

void CDROM::executeInit_SecondResponse()
{
	// Mode 0x20 is to be equivalent to setting only bit 5 SectorSize 1 = 924h=WholeSectorExceptSyncBytes
	m_mode.val = 0x20;
	if (s_logCDROM)
		logMode(m_mode);

	// Clear FIFOs
	resetParameterFIFO();
	resetResponseFIFO();
	m_pSectorDataBuffer->Reset(); // reset ring buffer
#if CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
	SafeStrcpy(m_debugSectorDataBufferDescription, sizeof(m_debugSectorDataBufferDescription), "Empty");
#endif

	// #TODO: Clear any pending commands

	m_headLBA = 0; // seems sensible to do this, but not 100% sure this is correct
	m_setLocPending = false;

	// Clear stat to aborts all read/seek/play states
	m_stat.val = 0;

	// Set motor on (only if disc present)
	if (m_pCD)
	{
		if (!m_stat.SpindleMotor && s_logCDROM)
			LOG_INFO("[CDROM]   Motor enabled\n");

		m_stat.SpindleMotor = true;
		m_spinningUp = false; // spin up immediately
		m_stopped = false;
	}

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Init2 INT2(stat) = INT2(%02X)\n", m_stat.val);

	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}


// Setfilter - Command 0Dh,file,channel --> INT3(stat)
// #TODO: Implement Setfilter
// https://psx-spx.consoledev.net/cdromdrive/#setfilter-command-0dhfilechannel-int3stat
//
void CDROM::executeSetfilterCommand0D()
{
	HP_ASSERT(m_paramCount == 2, "[CDROM] Unexpected number of parameters for Setfilter");
	m_xaFilterFileNumber = m_params[0];
	m_xaFilterChannelNumber = m_params[1];
	m_paramCount = 0; // Remove params from FIFO

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Setfilter(fileNumber=%02X, channelNumber=%02X)\n", m_xaFilterFileNumber, m_xaFilterChannelNumber);

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Setfilter_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Setfilter1");
	m_firstResponseInProgress = true;
}

void CDROM::Setfilter_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeSetfilter_FirstResponse();
}

void CDROM::executeSetfilter_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Setfilter1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);
}

void CDROM::executeMuteCommand0B()
{
	HP_FATAL_ERROR("[CDROM] Mute command is not implemented. This command is required by Ape Escape");
}

// Demute - Command 0Ch --> INT3(stat)
//
// Turn on audio streaming to SPU (affects both CD-DA and XA-ADPCM).
// The Demute command is needed only if one has formerly used the Mute command
// By default, the PSX is demuted after power-up (...and/or after Init command?), and is demuted after cdrom-booting).
//
// https://psx-spx.consoledev.net/cdromdrive/#demute-command-0ch-int3stat
void CDROM::executeDemuteCommand0C()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for Demute");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Demute\n");

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Demute_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Demute1");
	m_firstResponseInProgress = true;
}

void CDROM::Demute_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeDemute_FirstResponse();
}

void CDROM::executeDemute_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Demute1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);
}

// Setmode - Command 0Eh,mode --> INT3(stat)
//
//   7   Speed       (0=Normal speed, 1=Double speed)
//   6   XA-ADPCM    (0=Off, 1=Send XA-ADPCM sectors to SPU Audio Input)
//   5   Sector Size (0=800h=DataOnly, 1=924h=WholeSectorExceptSyncBytes)
//   4   Ignore Bit  (0=Normal, 1=Ignore Sector Size and Setloc position)
//   3   XA-Filter   (0=Off, 1=Process only XA-ADPCM sectors that match Setfilter)
//   2   Report      (0=Off, 1=Enable Report-Interrupts for Audio Play)
//   1   AutoPause   (0=Off, 1=Auto Pause upon End of Track) ;for Audio Play
//   0   CDDA        (0=Off, 1=Allow to Read CD-DA Sectors; ignore missing EDC)
// 
// https://psx-spx.consoledev.net/cdromdrive/#setmode-command-0ehmode-int3stat
//
// #TODO: Respect all mode bits
//
void CDROM::executeSetmodeCommand0E()
{
	HP_ASSERT(m_paramCount == 1, "[CDROM] Unexpected number of parameters for Setmode");
	m_mode.val = m_params[0];
	m_paramCount = 0; // Remove params from FIFO

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Setmode %02X\n", m_mode.val);

	if (s_logCDROM)
		logMode(m_mode);

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(Setmode_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "Setmode1");
	m_firstResponseInProgress = true;
}

// static
void CDROM::Setmode_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeSetmode_FirstResponse();
}

void CDROM::executeSetmode_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] Setmode1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);
}

//--------------------------------------------------------------------------------------------------

// GetlocL - Command 10h --> INT3(amm,ass,asect,mode,file,channel,sm,ci)
// 
// https://psx-spx.consoledev.net/cdromdrive/#getlocl-command-10h-int3ammassasectmodefilechannelsmci
//
void CDROM::executeGetlocLCommand10()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for GetlocP");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetlocL\n");

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(GetlocL_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "GetlocL");
	m_firstResponseInProgress = true;
}

void CDROM::GetlocL_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeGetlocL_FirstResponse();
}

void CDROM::executeGetlocL_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	// #TODO: Respond with correct values for GetlocL
	// #TEMP: Just return all zeros to get Parodius running
	u8 amm = 0;
	u8 ass = 0;
	u8 asect = 0;
	u8 mode = 0;
	u8 file = 0;
	u8 channel = 0;
	u8 sm = 0;
	u8 ci = 0;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetLocL1 INT3(amm,ass,asect,mode,file,channel,sm,ci) = INT3(%02X,%02X,%02X,%02X,%02u,%02u,%02X,%02X)\n",
			amm, ass, asect, mode, file, channel, sm, ci);

	writeResponseFIFO(amm);
	writeResponseFIFO(ass);
	writeResponseFIFO(asect);
	writeResponseFIFO(mode);
	writeResponseFIFO(file);
	writeResponseFIFO(channel);
	writeResponseFIFO(sm);
	writeResponseFIFO(ci);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);
}

//--------------------------------------------------------------------------------------------------

// GetlocP - Command 11h - INT3(track,index,mm,ss,sect,amm,ass,asect)
//
// Retrieves 8 bytes of position information from Subchannel Q with ADR=1.
// Mainly intended for displaying the current audio position during Play.
// All results are in BCD.
//
// 
//
// https://psx-spx.consoledev.net/cdromdrive/#getlocp-command-11h-int3trackindexmmsssectammassasect
void CDROM::executeGetlocPCommand11()
{
	HP_ASSERT(m_paramCount == 0, "[CDROM] Unexpected number of parameters for GetlocP");

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetlocP\n");

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(GetlocP_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "GetlocP");
	m_firstResponseInProgress = true;
}

void CDROM::GetlocP_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeGetlocP_FirstResponse();
}

void CDROM::executeGetlocP_FirstResponse()
{
	// INT3(track,index,mm,ss,sect,amm,ass,asect)
	// 
	// n.b. All results are in BCD.
	// 
	//   track:  track number (AAh=Lead-out area) (FFh=unknown, toc, none?)
	//   index:  index number (Usually 01h)
	//   mm:     minute number within track (00h and up)
	//   ss:     second number within track (00h to 59h)
	//   sect:   sector number within track (00h to 74h)
	//   amm:    minute number on entire disc (00h and up)
	//   ass:    second number on entire disc (00h to 59h)
	//   asect:  sector number on entire disc (00h to 74h)

	unsigned int trackIndex = findCurrentTrackIndex(); // #TODO: This value should be cached.
	u8 track = DecimalToBCD((u8)trackIndex + 1); // 1-indexed

	u8 index = 0x01; // Session index is always 1

	unsigned int m, s, f;
	CD::LBAtoMSF(m_headLBA, m, s, f);
	u8 amm = DecimalToBCD((u8)m);
	u8 ass = DecimalToBCD((u8)s);
	u8 asect = DecimalToBCD((u8)f);

	// To calculate minutes, seconds and sector *within* the track for GetlocP, just subtract track start LBA from headLBA and convert.
	unsigned int trackStartLBA = m_pCD->GetTrackStartLBA(trackIndex); // I think this is where
	HP_DEBUG_ASSERT(m_headLBA >= trackStartLBA); // Don't think this is valid. if 
	unsigned int lbaWithinTrack = m_headLBA - trackStartLBA;
	CD::LBAtoMSF(lbaWithinTrack, m, s, f);
	u8 mm = DecimalToBCD((u8)m);
	u8 ss = DecimalToBCD((u8)s);
	u8 sect = DecimalToBCD((u8)f);

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetLocP1 INT3(track,index,mm,ss,sect,amm,ass,asect) = INT3(%02X,%02u,%02u,%02X,%02u,%02u,%02X,%02X)\n",
			track, index, mm, ss, sect, amm, ass, asect);

	m_firstResponseInProgress = false; // clear BUSYSTS

	writeResponseFIFO(track);
	writeResponseFIFO(index);
	writeResponseFIFO(mm);
	writeResponseFIFO(ss);
	writeResponseFIFO(sect);
	writeResponseFIFO(amm);
	writeResponseFIFO(ass);
	writeResponseFIFO(asect);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);
}

//--------------------------------------------------------------------------------------------------

// GetTN - Command 13h --> INT3(stat,first,last) ;BCD
// 
// This assumes the CD has a single session
// 
// https://psx-spx.consoledev.net/cdromdrive/#gettn-command-13h-int3statfirstlast-bcd
void CDROM::executeGetTNCommand13()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetTN\n");

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(GetTN_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "GetTN1");
	m_firstResponseInProgress = true;
}

void CDROM::GetTN_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeGetTN_FirstResponse();
}

void CDROM::executeGetTN_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	if (m_paramCount != 0)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_ERROR("[CDROM] GetTN first response. Wrong number of parameters: expected 0, got %u\n", m_paramCount);

		errorINT5(kCDROM_Error_WrongNumberOfParameters);
		return;
	}

	u8 firstTrackNum = 1;
	u8 lastTrackNum = (u8)m_pCD->GetNumTracks();
	HP_DEBUG_ASSERT(lastTrackNum >= firstTrackNum && lastTrackNum <= 99);

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetTN1 INT3(stat,first,last) = INT3(%02X,%02u,%02u)\n", m_stat.val, firstTrackNum, lastTrackNum);

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	writeResponseFIFO(DecimalToBCD(firstTrackNum)); // n.b. BCD
	writeResponseFIFO(DecimalToBCD(lastTrackNum)); // n.b. BCD
	generateINT(3);
}

// GetTD - Command 14h,track --> INT3(stat,mm,ss) ;BCD
//
// For a disk with NN tracks, parameter values 01h..NNh return the start of the specified track
// 
// Parameter value 00h returns the end of the last track, and parameter values bigger than NNh return error code 10h.
// 
// The GetTD values are relative to Index=1 and are rounded down to second boundaries.
// 
// e.g. If track=N Index=0 starts at 12:34:56, and Track=N Index=1 starts at 12:36:56, then GetTD(N)
// will return 12:36, ie. the sector number is truncated, and the Index=0 region is skipped).
//
// https://psx-spx.consoledev.net/cdromdrive/#gettd-command-14htrack-int3statmmss-bcd
void CDROM::executeGetTDCommand14()
{
	HP_ASSERT(m_paramCount == 1, "CDROM GetTD command requires a single parameter");
	u8 trackNumBCD = m_params[0];
	m_paramCount = 0; // remove param from FIFO

	m_GetTD_trackNum = BCDtoDecimal(trackNumBCD);

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetTD %02u\n", m_GetTD_trackNum);

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(GetTD_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "GetTD1");
	m_firstResponseInProgress = true;
}

void CDROM::GetTD_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeGetTD_FirstResponse();
}

void CDROM::executeGetTD_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	HP_ASSERT(m_GetTD_trackNum <= m_pCD->GetNumTracks(), "#TODO: Return error code 10h");

	unsigned int trackLBA;
	if (m_GetTD_trackNum == 0)
		trackLBA = m_pCD->GetSizeBytes() / CD::kSectorSizeBytes; // end of final track
	else
		trackLBA = m_pCD->GetTrackStartLBA(m_GetTD_trackNum - 1);

	unsigned int minutes, seconds, fragments;
	CD::LBAtoMSF(trackLBA, minutes, seconds, fragments);

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetTD1 INT3(stat,mm,ss) = INT3(%02X,%02u,%02u)\n", m_stat.val, minutes, seconds);

	writeResponseFIFO(m_stat.val);
	writeResponseFIFO(DecimalToBCD((u8)minutes)); // n.b. BCD
	writeResponseFIFO(DecimalToBCD((u8)seconds)); // n.b. BCD

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	generateINT(3);
}

// SeekL - Command 15h --> INT3(stat) --> INT2(stat)
//
// Seek to Setloc's location in data mode (using data sector header position data, which works/exists
// only on Data tracks, not on CD-DA Audio tracks).
// 
// After the seek, the read head stays on the seeked location forever (namely: when seeking sector N, it
// does stay at around N-8..N-0 in single speed mode, or at around N-5..N+2 in double speed mode).
// This command will stop any current or pending ReadN or ReadS.
//
// Trying to use SeekL on Audio CDs passes okay on the first response, but (after two seconds or so)
// the second response will return an error (stat+4,04h), and stop the drive motor... that error
// doesn't appear ALWAYS though... works in some situations... such like when previously reading data
// sectors or so...?
//
// https://psx-spx.consoledev.net/cdromdrive/#seekl-command-15h-int3stat-int2stat
void CDROM::executeSeekLCommand15()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekL. Current target location (amm,ass,asect) = (%02X,%02X,%02X)\n", m_targetLocMinutes, m_targetLocSeconds, m_targetLocFrames);

	HP_ASSERT(m_setLocPending, "CDROM SeekL with no pending SetLoc"); // It is probably okay to ignore this.
	if (m_setLocPending)
		m_setLocPending = false;

	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(SeekL_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "SeekL1");
	m_firstResponseInProgress = true;
}

// static
void CDROM::SeekL_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeSeekL_FirstResponse();
}

void CDROM::executeSeekL_FirstResponse()
{
	// Don't set Seek bit be set in first response

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekL1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	// Set seek status now that Seek is underway internally
	HP_DEBUG_ASSERT(!m_stat.Read && !m_stat.Play, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
	m_stat.Seek = true;

	// Schedule the second response, which will respond when the seek is complete
	// Currently using a fixed seek time, regardless of current state
	// #TODO: Is it required to calculate the time required to seek from current position to target position?
	// #TODO: May need to account for spin up time if motor is stopped.
	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(SeekL_SecondResponseCallback, this, kSeekTimeCycles, "SeekL2");
}

void CDROM::SeekL_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executeSeekL_SecondResponse();
}

void CDROM::executeSeekL_SecondResponse()
{
	// Set laser head pos to target pos
	m_headLBA = m_targetLBA;

	// #TODO: Will stat Seek bit be clear in second response?
	m_stat.Seek = false;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekL2 INT2(stat) = INT2(%02X)  Head LBA: %08X (%u)\n", m_stat.val, m_headLBA, m_headLBA);

	// Generates SeekL command second response: INT2(stat)
	// Generate INT2 when response in place to communicate to host machine that command is complete.
	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}

// SeekP - Command 16h --> INT3(stat) --> INT2(stat)
// 
// Seek to Setloc's location in audio mode (using the Subchannel Q position data, which works on both Audio on Data disks).
// 
// After the seek, the disk stays on the seeked location forever (namely: when seeking sector N, it
// does stay at around N-9..N-1 in single speed mode, or at around N-2..N in double speed mode).
// This command will stop any current or pending ReadN or ReadS.
// 
// Note: Some older docs claim that SeekP would recurse only "MM:SS" of the "MM:SS:FF" position from
// Setloc - that is wrong, it does seek to MM:SS:FF (verified on a PSone).
// 
// After the seek, status is stat.bit7=0 (ie. audio playback off), until sending a new Play command
// (without parameters) to start playback at the seeked location.
// 
// https://psx-spx.consoledev.net/cdromdrive/#seekp-command-16h-int3stat-int2stat
//
void CDROM::executeSeekPCommand16()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekP. Current target location (amm,ass,asect) = (%02X,%02X,%02X)\n", m_targetLocMinutes, m_targetLocSeconds, m_targetLocFrames);

	HP_ASSERT(m_setLocPending, "CDROM SeekP with no pending SetLoc"); // It is probably okay to ignore this.
	if (m_setLocPending)
		m_setLocPending = false;

	cancelReadOrPlay();

	cancelCommandSecondResponse();
	HP_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(SeekP_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "SeekP1");
	m_firstResponseInProgress = true;
}

// static
void CDROM::SeekP_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeSeekP_FirstResponse();
}

void CDROM::executeSeekP_FirstResponse()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekP1 INT3(stat) = INT3(%02X)\n", m_stat.val);

	m_firstResponseInProgress = false; // clear BUSYSTS

	// Generate INT3 - meaning "response received" - when command response bytes are in place.
	// Don't set Seek bit be set in first response
	writeResponseFIFO(m_stat.val);
	generateINT(3);

	// Set seek status now that Seek is underway internally
	HP_DEBUG_ASSERT(!m_stat.Read && !m_stat.Play, "Only one of the status code Play, Seek and Read bits can be set at any one time.");
	m_stat.Seek = true;

	// Schedule the second response, which will respond when the seek is complete
	// Currently using a fixed seek time, regardless of current state
	// #TODO: Is it required to calculate the time required to seek from current position to target position?
	// #TODO: May need to account for spin up time if motor is stopped.
	HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
	m_commandSecondResponseEvent = m_scheduler.Schedule(SeekP_SecondResponseCallback, this, kSeekTimeCycles, "SeekP2");
}

void CDROM::SeekP_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->executeSeekP_SecondResponse();
}

void CDROM::executeSeekP_SecondResponse()
{
	// Set laser head pos to target pos
	m_headLBA = m_targetLBA;

	// #TODO: Will stat Seek bit be clear in second response?
	m_stat.Play = false;
	m_stat.Seek = false;

	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] SeekP2 INT2(stat) = INT2(%02X)  Head LBA: %08X (%u)\n", m_stat.val, m_headLBA, m_headLBA);

	// Generates SeekP command second response: INT2(stat)
	// Generate INT2 when response in place to communicate to host machine that command is complete.
	writeResponseFIFO(m_stat.val);
	HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
}

// Test commands are invoked with command number 19h
//
// See big table at: https://psx-spx.consoledev.net/cdromdrive/#sub_function-numbers-for-command-19h
//
void CDROM::executeTestCommand19()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] TestCommand 19h\n");

	cancelCommandSecondResponse();

	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(TestCommand_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "TestCommand1");
	m_firstResponseInProgress = true;
}

void CDROM::TestCommand_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->executeTestCommand_FirstResponse();
}

void CDROM::executeTestCommand_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	// psx-spx docs state that this command is *followed* by a parameter, but I think params are sent first.
	if (m_paramCount != 1) // CDROM Test commands require a single parameter containing sub-function number");
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] Test command 19h first response. Wrong number of parameters: expected 1 got %u\n", m_paramCount);

		errorINT5(kCDROM_Error_WrongNumberOfParameters);
		return;
	}

	u8 subFuncIndex = m_params[0];
	m_paramCount = 0; // remove param from FIFO

	switch (subFuncIndex)
	{
		// 20h GetVersion INT3(yy,mm,dd,ver)
		// Indicates the date (Year-month-day, in BCD format) and version of the HC05 CDROM controller BIOS.
		case 0x20:
		{
			if (s_logCDROM || s_logCDROMCommands)
				LOG_INFO("[CDROM] Test/GetVersion (yy,mm,dd,ver)\n");

			// Respond with INT3(yy,mm,dd,ver)
			// See table at https://psx-spx.consoledev.net/cdromdrive/#19h20h-int3yymmddver
			// It is important to return the correct result:
			// Choose very first version: PSX (PU-7)  19 Sep 1994, version vC0 (a)
			// (yy,mm,dd,ver) = (0x94,0x09,0x19,0xc0) n.b. BCD
			// This should result in TTY: System Controller ROM Version 94/09/19 c0
			static const u8 kVersion[] = { 0x94, 0x09, 0x19, 0xc0 };

			if (s_logCDROM || s_logCDROMCommands)
				LOG_INFO("[CDROM] Test/GetVersion1 INT3(yy,mm,dd,ver) = INT3(%02X,%02X,%02X,%02X)\n", kVersion[0], kVersion[1], kVersion[2], kVersion[3]);

			// Generate INT3 - meaning "response received" - when command response bytes are in place.
			writeResponseFIFO(kVersion[0]); // n.b BCD
			writeResponseFIFO(kVersion[1]);
			writeResponseFIFO(kVersion[2]);
			writeResponseFIFO(kVersion[3]);
			generateINT(3);

			break;
		}

		default:
		{
			if (s_logCDROM)
				LOG_INFO("[CDROM] Unimplemented Command 19h (test) sub-function %02X\n", subFuncIndex);
		}
	}
}

// GetID - Command 1Ah --> INT3(stat) --> INT2/5 (stat,flags,type,atip,"SCEx")
//
//  Drive Status           1st Response   2nd Response
//  Door (shell) Open      INT5(11h,80h)  N/A
//  Spin-up                INT5(01h,80h)  N/A
//  Detect busy            INT5(03h,80h)  N/A
//  No Disc                INT3(stat)     INT5(08h,40h, 00h,00h, 00h,00h,00h,00h)
//  Audio Disc             INT3(stat)     INT5(0Ah,90h, 00h,00h, 00h,00h,00h,00h)
//  Unlicensed:Mode1       INT3(stat)     INT5(0Ah,80h, 00h,00h, 00h,00h,00h,00h)
//  Unlicensed:Mode2       INT3(stat)     INT5(0Ah,80h, 20h,00h, 00h,00h,00h,00h)
//  Unlicensed:Mode2+Audio INT3(stat)     INT5(0Ah,90h, 20h,00h, 00h,00h,00h,00h)
//  Debug/Yaroze:Mode2     INT3(stat)     INT2(02h,00h, 20h,00h, 20h,20h,20h,20h)
//  Licensed:Mode2         INT3(stat)     INT2(02h,00h, 20h,00h, 53h,43h,45h,4xh)
//  Modchip:Audio/Mode1    INT3(stat)     INT2(02h,00h, 00h,00h, 53h,43h,45h,4xh)
// 
// https://psx-spx.consoledev.net/cdromdrive/#getid-command-1ah-int3stat-int25-statflagstypeatipscex
// 
void CDROM::executeGetIDCommand1A()
{
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("[CDROM] GetID\n");

	cancelCommandSecondResponse();
	HP_DEBUG_ASSERT(m_commandFirstResponseEvent == 0);
	m_commandFirstResponseEvent = m_scheduler.Schedule(GetID_FirstResponseCallback, this, kDefaultFirstResponseDurationCycles, "GetID1");
	m_firstResponseInProgress = true;
}

void CDROM::GetID_FirstResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandFirstResponseEvent = 0;
	pCDROM->GetID_FirstResponse();
}

void CDROM::GetID_FirstResponse()
{
	m_firstResponseInProgress = false; // clear BUSYSTS

	if (m_paramCount != 0)
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] GetID first response. Wrong number of parameters: expected 0 got %u\n", m_paramCount);

		errorINT5(kCDROM_Error_WrongNumberOfParameters);
		return;
	}

	bool detectBusy = false; // #TODO: What does "detect busy" mean?

	if (m_shellOpen)
	{
		// #TODO: First response: INT5(11h,80h)
		// No second response
		HP_FATAL_ERROR("Not implemented");
	}
	else if (m_spinningUp)
	{
		// #TODO: First response: INT5(01h,80h)
		// No second response
		HP_FATAL_ERROR("Not implemented");
	}
	else if (detectBusy)
	{
		// #TODO: First response: INT5(03h,80h)
		// No second response
		HP_FATAL_ERROR("Not implemented");
		HP_FATAL_ERROR("Not implemented");
	}
	else // no disc, or disc present
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] GetID1 INT3(stat) = %02X\n", m_stat.val);

		// First response: INT3(stat)
		// Generate INT3 - meaning "response received" - when command response bytes are in place.
		writeResponseFIFO(m_stat.val);
		generateINT(3);

		// See GetID second response timing at https://psx-spx.consoledev.net/cdromdrive/#second-response
		HP_DEBUG_ASSERT(m_commandSecondResponseEvent == 0);
		m_commandSecondResponseEvent = m_scheduler.Schedule(GetID_SecondResponseCallback, this, 0x0004a00, "GetID2");
	}
}

void CDROM::GetID_SecondResponseCallback(void* userdata)
{
	CDROM* pCDROM = (CDROM*)userdata;
	pCDROM->m_commandSecondResponseEvent = 0;
	pCDROM->GetID_SecondResponse();
}

//
// Generate GetID command second response: INT2/5 (stat,flags,type,atip,"SCEx")
//
void CDROM::GetID_SecondResponse()
{
	// Logic assumes a Licensed:Mode2 (Japan/NTSC) disc is present
	if (m_pCD)
	{
		// Second response: INT2(02h,00h, 20h,00h, 53h,43h,45h,4xh)
		// Where x is ASCII code for SCEx region code:
		// - 'I' for SCEI (Japan/NTSC)
		// - 'A' for SCEA (America/NTSC)
		// - 'E' for SCEE (Europe/PAL)
		// The "SCEx" string is displayed in the intro, and the PSX refuses to boot if it doesn't match up for the local region.
		enum class Region { Japan, America, Europe };
		const u8 kRegionCodeSCEI[] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'I' };
		const u8 kRegionCodeSCEA[] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };
		const u8 kRegionCodeSCEE[] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E' };
		const u8* kRegionCodes[] = { kRegionCodeSCEI, kRegionCodeSCEA, kRegionCodeSCEE };

		// #TODO: Should the region come from the disc data itself?
		Region region = Region::America;
		const u8* regionCode = kRegionCodes[(int)region];

		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] GetID2 INT2(02h,00h, 20h,00h, '%c','%c','%c','%c') = Licensed:Mode2 SCE%c\n",
				regionCode[4], regionCode[5], regionCode[6], regionCode[7], regionCode[7]);

		for (unsigned int i = 0; i < 8; i++)
		{
			writeResponseFIFO(regionCode[i]);
		}

		// Generate INT2 when response in place to communicate to host machine that command is complete.
		HP_VERIFY(generateINT(2), "INT2 was queued rather than being generated, so the response queue may be read out of order. See INT(1) for solution");
	}
	else // no disc
	{
		if (s_logCDROM || s_logCDROMCommands)
			LOG_INFO("[CDROM] GetID2 INT5(08h,40h,00h,00h,00h,00h,00h,00h) = No disc\n");

		// Second response: INT5(08h,40h, 00h,00h, 00h,00h,00h,00h)
		writeResponseFIFO(0x08);
		writeResponseFIFO(0x40);
		writeResponseFIFO(0x00);
		writeResponseFIFO(0x00);
		writeResponseFIFO(0x00);
		writeResponseFIFO(0x00);
		writeResponseFIFO(0x00);
		writeResponseFIFO(0x00);

		// Generate INT5 when response in place to communicate to host machine that command is complete.
		HP_ASSERT((m_hintsts & 7) == 0, "INT%u is already pending. Should it be overwritten, or should INT2 flag be set to be applied when INT acknowledged with CLRCTL write?");
		generateINT(5);
	}
}

void CDROM::resetParameterFIFO()
{
	if (s_logCDROM || s_logCDROMParams)
		LOG_INFO("[CDROM] Parameter FIFO reset: %u params cleared\n", m_paramCount);
	m_paramCount = 0;
}

void CDROM::resetResponseFIFO()
{
	if (s_logCDROM || s_logCDROMResponse)
		LOG_INFO("[CDROM] Response FIFO reset: %u results cleared\n", m_responseCount);

	for (unsigned int i = 0; i < COUNTOF_ARRAY(m_responseFIFO); i++)
	{
		m_responseFIFO[i] = 0;
	}
	m_responseWriteIndex = 0;
	m_responseReadIndex = 0;
	m_responseCount = 0;
}

void CDROM::writeResponseFIFO(u8 val)
{
	// Responses just keep getting written to the circular FIFO, regardless of what has been read.

	HP_DEBUG_ASSERT(m_responseWriteIndex < COUNTOF_ARRAY(m_responseFIFO));
	m_responseFIFO[m_responseWriteIndex] = val;

	if (s_logCDROM || s_logCDROMResponse)
		LOG_INFO("[CDROM] Response queued value %02X into slot %u (%u results available)\n", val, m_responseWriteIndex, m_responseCount);

	static_assert(COUNTOF_ARRAY(m_responseFIFO) == 16);
	m_responseWriteIndex = (m_responseWriteIndex + 1) & 0xf;

	m_responseCount++;
}

u8 CDROM::readResponseFIFO()
{
	// "It is possible/legal to read past last written response value and wrap." - https://psx-spx.consoledev.net/cdromdrive/#0x1f801801-read-all-banks-result
	// Responses are just read from the FIFO regardless of what has been written.
	// This is observed when booting Mortal Kombat 2. Either the BIOS or the game are to blame.

	HP_DEBUG_ASSERT(m_responseReadIndex < COUNTOF_ARRAY(m_responseFIFO));
	u8 result = m_responseFIFO[m_responseReadIndex];

	// Increment and wrap FIFO read pointer.
	static_assert(COUNTOF_ARRAY(m_responseFIFO) == 16, "FIFO read pointer wrapping depends on this.");
	m_responseReadIndex = (m_responseReadIndex + 1) & 0xf;

	if (m_responseCount > 0)
		m_responseCount--;

#if 0 // redundant - calling code logs
	if (s_logCDROM || s_logCDROMResponse)
		LOG_INFO("[CDROM] Response FIFO read. Value %02X. %u results available\n", result, m_responseCount);
#endif
	return result;
}

//-----------------------------------------------------------------------------------------------------------

unsigned int CDROM::GetSectorDataSizeBytes() const
{
	return m_pSectorDataBuffer->GetUsedSpaceBytes();
}

u8 CDROM::readSectorDataFIFOByte()
{
	if (m_pSectorDataBuffer->IsEmpty())
	{
		// Return final byte value if data FIFO empty when read:
		//    The PSX hardware allows to read 800h-byte or 924h-byte sectors, indexed as [000h..7FFh] or [000h..923h], 
		//    when trying to read further bytes, then the PSX will repeat the byte at index [800h-8] or [924h-4] as padding value.
		HP_FATAL_ERROR("Not tested");
		return m_sectorDataByteValue;
	}
	else // not empty
	{
		m_pSectorDataBuffer->Read(&m_sectorDataByteValue, 1);
		return m_sectorDataByteValue;
	}
}

u32 CDROM::readSectorDataFIFOWord()
{
	const unsigned int bytesAvailable = m_pSectorDataBuffer->GetUsedSpaceBytes();
	if (bytesAvailable >= 4)
	{
		u32 val;
		m_pSectorDataBuffer->Read((u8*)&val, 4); // #TODO: Check endianness

		// Update last byte value
		m_sectorDataByteValue = (u8)val;

		return val;
	}
	else if (bytesAvailable == 0) // empty
	{
		// Return final byte value if data FIFO empty when read:
		//    The PSX hardware allows to read 800h-byte or 924h-byte sectors, indexed as [000h..7FFh] or [000h..923h], 
		//    when trying to read further bytes, then the PSX will repeat the byte at index [800h-8] or [924h-4] as padding value.
//		HP_FATAL_ERROR("Not tested"); // Disabled: Ape Escape hits this.
		return (m_sectorDataByteValue << 24) | (m_sectorDataByteValue << 16) | (m_sectorDataByteValue << 8) | m_sectorDataByteValue;
	}
	else // if (m_pSectorDataBuffer->GetUsedSpaceBytes() < 4)
	{
		// Less than one word available
		// #TODO: Read as many bytes as possible, then repeat final one.
		HP_FATAL_ERROR("Not implemented");
		return 0;
	}
}

void CDROM::ReadDataBlock(u8* dst, unsigned int numBytes)
{
	HP_DEBUG_ASSERT(numBytes <= m_pSectorDataBuffer->GetUsedSpaceBytes(), "Don't call unless enough bytes are available");
	m_pSectorDataBuffer->Read(dst, numBytes);
}

//-----------------------------------------------------------------------------------------------------------

//  INT0 NoIntr      No interrupt pending
//  INT1 DataReady   New sector (ReadN/ReadS) or report packet (Play) available
//  INT2 Complete    Command finished processing (some commands, after INT3 is fired)
//  INT3 Acknowledge Command received and acknowledged (all commands). Generated immediately after executing a command.
//  INT4 DataEnd     Reached end of disc (or end of track if auto-pause enabled)
//  INT5 DiskError   Command error, read error, license string error or lid opened
//  INT6 -
//  INT7 -
//
// The IRQ acts as a synchronization lock:
// - When a command response sets an interrupt, the lock is acquired
// - When the CPU acknowledges the interrupt (via writeHCLRCTL), the lock is released
// - New commands cannot execute while the lock is held (they are queued as pending)
//
// The PSX can deliver one INT after another. Instead of using a real queue, it's merely using some flags
// that do indicate which INT(s) need to be delivered. Basically, there seem to be two flags:
// One for Second Response (INT2), and one for Data/Report Response (INT1).
// There is no flag for First Response (INT3); because that INT is generated immediately after executing a command.
// 
// https://psx-spx.consoledev.net/cdromdrive/#responses
//
// Returns true if the INT could be generated, false if had to be queued.
//
bool CDROM::generateINT(unsigned int index)
{
	HP_DEBUG_ASSERT(index <= 7);

	const unsigned int pendingINT = m_hintsts & 7;

	// If a read finishes, the sub-CPU won't check for the new sector and set the IRQ if there is an unacknowledged interrupt or command in progress.
	if (index == 1)
	{
		if (pendingINT)
		{
			// Queue INT1
			HP_DEBUG_ASSERT(!m_int1pending);
			m_int1pending = true;

			if (s_logCDROM || s_logCDROMInterrupts)
				LOG_INFO("[CDROM] INT1 queued because INT%u pending\n", pendingINT);
			return false; // INT1 not generated
		}

		// When a command is being processed (until INT3 is posted), the HC05 won't check for
		// new sectors. This ensures INT3 always comes before any INT1 from Read/Play operations.
		if (m_firstResponseInProgress)
		{
			// Queue INT1
			HP_DEBUG_ASSERT(!m_int1pending);
			m_int1pending = true;

			if (s_logCDROM || s_logCDROMInterrupts)
				LOG_INFO("[CDROM] INT1 queued because command first response in progress.\n");
			return false; // INT1 not generated
		}
	}

	if (index == 2 && (pendingINT || m_firstResponseInProgress))
	{
		// Queue INT2
		HP_DEBUG_ASSERT(!m_int2pending);
		m_int2pending = true;

		if (s_logCDROM || s_logCDROMInterrupts)
			LOG_INFO("[CDROM] INT2 queued because INT%u pending or first response in progress.\n", pendingINT);
		return false; // INT2 not generated
	}

	HP_ASSERT(pendingINT == 0, "Previous interrupt has not be acknowledged.");

	m_hintsts &= ~7; // clear bits 2:0
	m_hintsts |= index;

	if (s_logCDROM || s_logCDROMInterrupts)
		LOG_INFO("[CDROM] INT%u set (prev %u)\n", index, pendingINT);

	// The CD-ROM drive fires an interrupt whenever (HINTMSK & HINTSTS) is non-zero.
	if ((m_hintmsk & m_hintsts) != 0)
	{
		m_intc.SetIRQ(IRQ::IRQ2_CDROM);
	}
	else
	{
		HP_FATAL_ERROR("Not generating interrupt because masked. Not tested");
	}

	return true; // INT generated
}

//-----------------------------------------------------------------------------------------------------------

unsigned int CDROM::findCurrentTrackIndex() const
{
	unsigned int trackIndex;
	for (trackIndex = 0; trackIndex + 1 < m_pCD->GetNumTracks(); trackIndex++)
	{
		if (m_pCD->GetTrackStartLBA(trackIndex + 1) > m_headLBA) // The track actually starts at INDEX 1. Index 0 is pregap.
			break;
	}
	HP_ASSERT(trackIndex < m_pCD->GetNumTracks());
	return trackIndex;
}

void CDROM::resetXABuffer()
{
	memset(m_xaResamplingRingBufferL, 0, sizeof(m_xaResamplingRingBufferL));
	memset(m_xaResamplingRingBufferR, 0, sizeof(m_xaResamplingRingBufferR));

	m_xa_adcpm_old_left = 0;
	m_xa_adcpm_older_left = 0;
	m_xa_adcpm_old_right = 0;
	m_xa_adcpm_older_right = 0;

	m_pXABuffer->Reset();
	if (s_logCDROM)
		LOG_INFO("[CDROM] XA buffer reset\n");

	// I think in real hardware these are never reset, but I would prefer to.
	m_xaResamplingRingBufferPos = 0;
	m_xaSixstepCounter = 6;
}

//-----------------------------------------------------------------------------------------------------------

//
// Each sector consists of 12h 128-byte portions (=900h bytes) (the remaining 14h bytes of the sectors 914h-byte data region are 00h filled).
// 
// The separate 128-byte portions start with a 16-byte header:
// 
//     00h..03h  Copy of below 4 bytes (at 04h..07h)
//     04h       Header for 1st Block/Mono, or 1st Block/Left
//     05h       Header for 2nd Block/Mono, or 1st Block/Right
//     06h       Header for 3rd Block/Mono, or 2nd Block/Left
//     07h       Header for 4th Block/Mono, or 2nd Block/Right
//     08h       Header for 5th Block/Mono, or 3rd Block/Left  ;\unknown/unused
//     09h       Header for 6th Block/Mono, or 3rd Block/Right ; for 8bit ADPCM
//     0Ah       Header for 7th Block/Mono, or 4th Block/Left  ; (maybe 0, or maybe
//     0Bh       Header for 8th Block/Mono, or 4th Block/Right ;/copy of above)
//     0Ch..0Fh  Copy of above 4 bytes (at 08h..0Bh)
//
// The header is followed by twenty-eight data words (4x28-bytes):
//
//   10h..13h  1st Data Word (packed 1st samples for 2-8 blocks)
//   14h..17h  2nd Data Word (packed 2nd samples for 2-8 blocks)
//   18h..1Bh  3rd Data Word (packed 3rd samples for 2-8 blocks)
//   ...       Nth Data Word (packed Nth samples for 2-8 blocks)
//   7Ch..7Fh  28th Data Word (packed 28th samples for 2-8 blocks)
//
// and then followed by the next 128-byte portion.
//
// XA-ADPCM Header Bytes
//   0-3   Shift  (0..12) (0=Loudest) (13..15=Reserved/Same as 9)
//   4-5   Filter (0..3) (only four filters, unlike SPU-ADPCM which has five)
//   6-7   Unused (should be 0)
//
// Note: The 4-bit (or 8-bit) samples are expanded to 16-bit by left-shifting them by 12 (or 8).
// That 16-bit value is then right-shifted by the selected 'shift' amount.
// For 8-bit ADPCM shift should be 0..8 (values 9..12 will cut-off the LSB(s) of the 8-bit value, this works, but isn't useful).
// For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
//
// XA-ADPCM Data Words (32bit, little endian)
// 
//     0-3   Nibble for 1st Block/Mono, or 1st Block/Left  (-8h..+7h)
//     4-7   Nibble for 2nd Block/Mono, or 1st Block/Right (-8h..+7h)
//     8-11  Nibble for 3rd Block/Mono, or 2nd Block/Left  (-8h..+7h)
//     12-15 Nibble for 4th Block/Mono, or 2nd Block/Right (-8h..+7h)
//     16-19 Nibble for 5th Block/Mono, or 3rd Block/Left  (-8h..+7h)
//     20-23 Nibble for 6th Block/Mono, or 3rd Block/Right (-8h..+7h)
//     24-27 Nibble for 7th Block/Mono, or 4th Block/Left  (-8h..+7h)
//     28-31 Nibble for 8th Block/Mono, or 4th Block/Right (-8h..+7h)
// 
// or, for 8bit ADPCM format:
// 
//     0-7   Byte for 1st Block/Mono, or 1st Block/Left    (-80h..+7Fh)
//     8-15  Byte for 2nd Block/Mono, or 1st Block/Right   (-80h..+7Fh)
//     16-23 Byte for 3rd Block/Mono, or 2nd Block/Left    (-80h..+7Fh)
//     24-31 Byte for 4th Block/Mono, or 2nd Block/Right   (-80h..+7Fh)
// 
// decode_sector routine based on pseudo-code from https://psx-spx.consoledev.net/cdromformat/#decode_sectorsrc
//
//	  src=src+12+4+8   ;skip sync,header,subheader
//	  for i=0 to 11h
//	   for blk=0 to 3
//		IF stereo ;left-samples (LO-nibbles), plus right-samples (HI-nibbles)
//		  decode_28_nibbles(src,blk,0,dst_left,old_left,older_left)
//		  decode_28_nibbles(src,blk,1,dst_right,old_right,older_right)
//		ELSE      ;first 28 samples (LO-nibbles), plus next 28 samples (HI-nibbles)
//		  decode_28_nibbles(src,blk,0,dst_mono,old_mono,older_mono)
//		  decode_28_nibbles(src,blk,1,dst_mono,old_mono,older_mono)
//		ENDIF
//	   next blk
//	   src=src+128
//	  next i
//	  src=src+14h+4    ;skip padding,edc
//
//
void CDROM::decodeXAADPCMSector(const u8* pSector, u8 codingInfo)
{
	const u8* pSrc = pSector + CD::kMode2SectorDataOffset;

	// Tony Hawk's (original game) uses 18900 Hz XA-ADPCM
	const bool is18900Hz = codingInfo & 0x04;

	const bool stereo = codingInfo & 0x01;

	// 0x12 chunks of 8 blocks each containing 28 samples = 4032
	static constexpr unsigned int kSamplesPerWord = 8; // For 4-bit audio, each 32-bit word contains 8 4-bit nibbles, each is decoded to one 16-bit sample
	static constexpr unsigned int kSamplesPerSector = kSamplesPerWord * XAADPCM::kWordsPerChunk * XAADPCM::kChunksPerSector; // 4032 (decimal)

	// The decoded data is resampled up to 44100 Hz on the fly.
	// 44100 / 37800 = 7/6, so need to generate 7 samples for every 6 samples
	static constexpr unsigned int k44100SamplesPerSector = (kSamplesPerSector * 7) / 6; // 4704 exactly
	static constexpr unsigned int kBytesPerSample = 2; // signed 16-bit samples
	static constexpr unsigned int k44100BytesPerSector = kBytesPerSample * k44100SamplesPerSector; // 9408 exactly
	static_assert(kResampledXABufferSizeBytes >= k44100BytesPerSector, "Buffer needs to be large enough to hold at least one sector worth of resampled data");
	HP_ASSERT(m_pXABuffer->GetFreeSpaceBytes() >= k44100BytesPerSector);

	// Important: Do not clear the XA buffer on each sector.
	// Slightly more data is generated each sector than is consumed, so the ring buffer must be used for its intended purpose.
	//	m_pXABuffer->Reset(); // Deliberately commented out
	if (s_logCDROM || s_logCDROMCommands)
		LOG_INFO("XA buffer size: %08X\n", m_pXABuffer->GetUsedSpaceBytes()); // debug

	if (stereo)
	{
		// Small intermediate buffers for decoded samples, usually at a sample rate of 37800 Hz
		s16 samplesL[XAADPCM::kWordsPerChunk];
		s16 samplesR[XAADPCM::kWordsPerChunk];

		for (unsigned int i = 0; i < XAADPCM::kChunksPerSector; i++)
		{
			for (unsigned int blk = 0; blk < 4; blk++)
			{
				decode_28_nibbles(pSrc, blk, /*nibble*/0, samplesL, m_xa_adcpm_old_left, m_xa_adcpm_older_left);
				decode_28_nibbles(pSrc, blk, /*nibble*/1, samplesR, m_xa_adcpm_old_right, m_xa_adcpm_older_right);

#if CDROM_XA_ADPCM_CAPTURE_ENABLED
				if (s_pXAADPCMFile)
				{
					for (unsigned int sampleIndex = 0; sampleIndex < XAADPCM::kWordsPerChunk; sampleIndex++)
					{
						fwrite((const void*)(samplesL + sampleIndex), 2, 1, s_pXAADPCMFile);
						fwrite((const void*)(samplesR + sampleIndex), 2, 1, s_pXAADPCMFile);
					}
				}
#endif
				resampleStereo(samplesL, samplesR, is18900Hz);
			}
			pSrc += 128;
		}
	}
	else // mono
	{
		// Resident Evil uses mono XA-ADPCM almost as soon as gameplay starts
		// Crash Team Racing uses mono XA-ADPCM as soon as audio starts.

		// Small intermediate buffer for decoded samples, usually at a sample rate of 37800 Hz
		s16 samples[2 * XAADPCM::kWordsPerChunk]; // first 28 samples (LO-nibbles), plus next 28 samples (HI-nibbles)

		for (unsigned int i = 0; i < XAADPCM::kChunksPerSector; i++)
		{
			for (unsigned int blk = 0; blk < 4; blk++)
			{
				// Decode first 28 samples from LO-nibbles
				decode_28_nibbles(pSrc, blk, 0, samples, m_xa_adcpm_old_left, m_xa_adcpm_older_left);

				// Decode next next 28 samples from HI-nibbles
				decode_28_nibbles(pSrc, blk, 1, samples + XAADPCM::kWordsPerChunk, m_xa_adcpm_old_left, m_xa_adcpm_older_left);

#if CDROM_XA_ADPCM_CAPTURE_ENABLED
					if (s_pXAADPCMFile)
					{
						for (unsigned int sampleIndex = 0; sampleIndex < COUNTOF_ARRAY(samples); sampleIndex++)
						{
							fwrite((const void*)(samples + sampleIndex), 2, 1, s_pXAADPCMFile);
						}
					}
#endif
				resampleMono(samples, is18900Hz);
			}
			pSrc += 128;
		}
	}

	// Can't see anypoint in skipping the padding and EDC at the end of the sector because this is all the data we have, but here it is if needed:
//	pSrc += 0x14 + 4;
}

// Resample to 44100 Hz and interleave L and R channels into XA buffer for consumption by host audio device.
//
// 25-point Zigzag Interpolation
// 
// https://psx-spx.consoledev.net/cdromformat/#25-point-zigzag-interpolation
// 
//	Output37800Hz(sample):
//    ringbuf[p AND 1Fh]=sample, p=p+1, sixstep=sixstep-1
//    if sixstep=0
//      sixstep=6
//      Ouput44100Hz(ZigZagInterpolate(p,Table1))
//      Ouput44100Hz(ZigZagInterpolate(p,Table2))
//      Ouput44100Hz(ZigZagInterpolate(p,Table3))
//      Ouput44100Hz(ZigZagInterpolate(p,Table4))
//      Ouput44100Hz(ZigZagInterpolate(p,Table5))
//      Ouput44100Hz(ZigZagInterpolate(p,Table6))
//      Ouput44100Hz(ZigZagInterpolate(p,Table7))
//    endif
// 
// ZigZagInterpolate(p,TableX):
//    sum=0
//    for i=1 to 29, sum=sum+(ringbuf[(p-i) AND 1Fh]*TableX[i])/8000h, next i
//    return MinMax(sum,-8000h,+7FFFh)
//
void CDROM::resampleStereo(s16 samplesL[XAADPCM::kWordsPerChunk], s16 samplesR[XAADPCM::kWordsPerChunk], bool is18900Hz)
{
	for (unsigned int sampleIndex = 0; sampleIndex < XAADPCM::kWordsPerChunk; sampleIndex++)
	{
		m_xaResamplingRingBufferL[m_xaResamplingRingBufferPos] = samplesL[sampleIndex];
		m_xaResamplingRingBufferR[m_xaResamplingRingBufferPos] = samplesR[sampleIndex];

		if (++m_xaResamplingRingBufferPos == COUNTOF_ARRAY(m_xaResamplingRingBufferL))
			m_xaResamplingRingBufferPos = 0;

		m_xaSixstepCounter--;
		if (m_xaSixstepCounter == 0)
		{
			m_xaSixstepCounter = 6;

			// Interleave L and R channels in output
			for (unsigned int tableIndex = 0; tableIndex < kZigZagInterpolationTableCount; tableIndex++)
			{
				s16 l = zigZagInterpolate(m_xaResamplingRingBufferL, m_xaResamplingRingBufferPos, kZigZagInterpolationTables[tableIndex]);
				s16 r = zigZagInterpolate(m_xaResamplingRingBufferR, m_xaResamplingRingBufferPos, kZigZagInterpolationTables[tableIndex]);
				m_pXABuffer->Write((u8*)&l, 2);
				m_pXABuffer->Write((u8*)&r, 2);
				if (is18900Hz)
				{
					// Write again.
					// #TODO: Could consider zero stuffing to upsample 2x (like reverb)
					m_pXABuffer->Write((u8*)&l, 2);
					m_pXABuffer->Write((u8*)&r, 2);
				}
			}
		}
	}
}

void CDROM::resampleMono(s16 samples[2 * XAADPCM::kWordsPerChunk], bool is18900Hz)
{
	for (unsigned int sampleIndex = 0; sampleIndex < 2 * XAADPCM::kWordsPerChunk; sampleIndex++)
	{
		// Use the left stereo buffer for mono
		m_xaResamplingRingBufferL[m_xaResamplingRingBufferPos] = samples[sampleIndex];

		if (++m_xaResamplingRingBufferPos == COUNTOF_ARRAY(m_xaResamplingRingBufferL))
			m_xaResamplingRingBufferPos = 0;

		m_xaSixstepCounter--;
		if (m_xaSixstepCounter == 0)
		{
			m_xaSixstepCounter = 6;

			// Interleave L and R channels in output, but this is mono, so just write same sample to both channels.
			for (unsigned int tableIndex = 0; tableIndex < kZigZagInterpolationTableCount; tableIndex++)
			{
				s16 s;
				s = zigZagInterpolate(m_xaResamplingRingBufferL, m_xaResamplingRingBufferPos, kZigZagInterpolationTables[tableIndex]);

				// Write mono signal to both channels
				m_pXABuffer->Write((u8*)&s, 2);
				m_pXABuffer->Write((u8*)&s, 2);
				if (is18900Hz)
				{
					// Write again.
					// #TODO: Could consider zero stuffing to upsample 2x (like reverb)
					m_pXABuffer->Write((u8*)&s, 2);
					m_pXABuffer->Write((u8*)&s, 2);
				}
			}
		}
	}
}

// sign-extend 4-bit value
static inline s8 signed4bit(u8 val)
{
	// n.b. The cast before shifting back down is crucial because val << 4 is of type int due to integer promotion rules,
	// so we need to cast it back to s16 before shifting back down to get the correct sign extension.
	return (s8)(val << 4) >> 4;
}

// XA-ADPCM decoding helper function based on pseudo-code from
// https://psx-spx.consoledev.net/cdromformat/#decode_28_nibblessrcblknibbledstoldolder
//
//    shift  = 12 - (src[4+blk*2+nibble] AND 0Fh)
//    filter =      (src[4+blk*2+nibble] AND 30h) SHR 4
//    f0 = pos_xa_adpcm_table[filter]
//    f1 = neg_xa_adpcm_table[filter]
//    for j=0 to 27
//      t = signed4bit((src[16+blk+j*4] SHR (nibble*4)) AND 0Fh)
//      s = (t SHL shift) + ((old*f0 + older*f1+32)/64);
//      s = MinMax(s,-8000h,+7FFFh)
//      halfword[dst]=s, dst=dst+2, older=old, old=s
//    next j
//
static void decode_28_nibbles(const u8* pSrc, unsigned int blk, unsigned int nibble, s16* pDst, s16& old, s16& older)
{
	u8 headerByte = pSrc[4 + (blk * 2) + nibble];

	unsigned int shift = headerByte & 0xf; // bits 3:0
	if (shift >= 13) // 13,14,15 same as 9
		shift = 9;
	shift = 12 - shift;

	// n.b. Only 4 of these values are used for XA-ADPCM
	static const int pos_xa_adpcm_table[5] = { 0, 60, 115, 98, 122 };
	static const int neg_xa_adpcm_table[5] = { 0, 0, -52, -55, -60 };
	unsigned int filter = (headerByte >> 4) & 3; // bits 5:4
	const int f0 = pos_xa_adpcm_table[filter];
	const int f1 = neg_xa_adpcm_table[filter];

	for (unsigned int j = 0; j < 28; j++)
	{
		u8 val = pSrc[16 + blk + (j * 4)]; // 16 is size of header (block of 16 header bytes)
		val >>= (nibble * 4);
		s8 t = signed4bit(val & 0xf);
		int s = ((int)t << shift) + ((old * f0 + older * f1 + 32) / 64); // interpolate  #TODO[#opt]: Replace /64 with shift right
		s = Clamp(s, (int)INT16_MIN, (int)INT16_MAX);
		*pDst++ = (s16)s;
		older = old;
		old = (s16)s;
	}
}

static s16 zigZagInterpolate(const s16 resamplingRingBuffer[32], unsigned int p, const s16 table[kZigZagInterpolationTableElementCount])
{
	s32 sum = 0;
	for (unsigned int i = 1; i <= kZigZagInterpolationTableElementCount; i++)
	{
		sum += ((s32)resamplingRingBuffer[(p - i) & 0x1f] * (s32)table[i - 1]) >> 15; // / 0x8000 replaced with >> 15 to avoid division in hot loop
	}

	return (s16)Clamp(sum, (int)INT16_MIN, (int)INT16_MAX);
}
