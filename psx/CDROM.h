// PSX CDROM subsystem emulation
// 
// This is high level emulation (HLE) of the CDROM subsystem.
//
// 
// CDROM command flow:
// - CPU writes parameters
// - CPU sends command
// - CDROM starts filling response fifo 
// - CDROM sets internal interrupt level (usually INT3)
// - X cycles pass...
// - CDROM generates IRQ2 (set bit 2 in I_STAT)
// - CPU Reads CDROM status and response fifo
//
// References:
// - https://psx-spx.consoledev.net/cdromdrive/ (many comments copied directly from here)
// - https://problemkaputt.de/psxspx-cdrom-controller-i-o-ports.htm
// - Sony CXD1199AQ datasheet
//
// Terminology:
// - psx-spx uses "index" to mean "register bank"
// - Reponse FIFO and Results FIFO seem to be used interchangeably.
//

#pragma once

#include "CD.h"

#include "core/Types.h"
#include "core/Helpers.h" // ENUM_COUNT

#ifndef CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
#ifdef DEBUG
#define CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED 1
#else
#define CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED 0
#endif
#endif

inline bool s_logCDROM = false; // log everything - very verbose and detrimental to performance
inline bool s_logCDROMParams = false;
inline bool s_logCDROMResponse = false;
inline bool s_logCDROMCommands = false;
inline bool s_logCDROMInterrupts = false;

class CD;
class INTC;
class RingBuffer;
class Scheduler;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

struct XAADPCM
{
	static constexpr unsigned int kWordsPerChunk = 28;
	static constexpr unsigned int kChunksPerSector = 0x12; // 18
};

class CDROM
{
public:

	CDROM(INTC& intc, Scheduler& scheduler);
	~CDROM();

	void Reset();

	// 4 banks of 4 registers
	void WriteReg(unsigned int index, u8 val);
	u8 ReadReg(unsigned int index);

	void InsertDisc(const CD& cd);
	void EjectDisc();
	bool IsDiscInserted() const { return m_pCD != nullptr; }
	const CD* GetCD() const { return m_pCD; }

	// For use by DMA, which transfers 32-bits at a time
	u32 ReadRDDATA();

	unsigned int GetSectorDataSizeBytes() const; // returns number of used bytes (not capacity)

	// Optimisation for immediate DMA to copy directly from sector data buffer into RAM.
	// Caller should check that required number of bytes are available and ensure that dst buffer is large enough.
	void ReadDataBlock(u8* dst, unsigned int numBytes);

#if CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
	// Debug API to return human-readable description of the current contents of the sector data buffer.
	const char* DebugGetSectorDataBufferDescription() const { return m_debugSectorDataBufferDescription; }
#else
	const char* DebugGetSectorDataBufferDescription() const { return "Only available in DEBUG"; }
#endif

	u32 GetHeadLBA() const { return m_headLBA; }

	RingBuffer& GetCDDABuffer() { return *m_pCDDABuffer; }
	const RingBuffer& GetCDDABuffer() const { return *m_pCDDABuffer; }

	RingBuffer& GetCDXABuffer() { return *m_pXABuffer; }
	const RingBuffer& GetCDXABuffer() const { return *m_pXABuffer; }
	bool IsXAMuted() const { return m_xaMuted; }

	// Volumes are used for both CDDA and XA-ADPCM audio
	u8 GetCDLeftLeftVolume() const { return m_cdLeftLeftVolume; }
	u8 GetCDLeftRightVolume() const { return m_cdLeftRightVolume; }
	u8 GetCDRightRightVolume() const { return m_cdRightRightVolume; }
	u8 GetCDRightLeftVolume() const { return m_cdRightLeftVolume; }

	//----------------------------------------------------------------------------------------------------------------------------

	// Status code
	//
	//  Bit  Name          Meaning
	//  7    Play          Playing CD-DA         ;\only ONE of these bits can be set
	//  6    Seek          Seeking               ; at a time (ie. Read/Play won't get
	//  5    Read          Reading data sectors  ;/set until after Seek completion)
	//  4    ShellOpen     Once shell open (0=Closed, 1=Is/was Open)  "shell" refers to the physical door on the case covering the CD.
	//  3    IdError       (0=Okay, 1=GetID denied) (also set when Setmode.Bit4=1)
	//  2    SeekError     (0=Okay, 1=Seek error)     (followed by Error Byte)
	//  1    Spindle Motor (0=Motor off, or in spin-up phase, 1=Motor on)
	//  0    Error         Invalid Command/parameters (followed by Error Byte)
	// 
	// Returned by Nop command 01h and many others in "first response" e.g. GetID
	// 
	// https://psx-spx.consoledev.net/cdromdrive/#status-code-stat
	//
	struct Stat
	{
		union {
			u8 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u8 Error : 1;        // 0 0x01
				u8 SpindleMotor : 1; // 1 0x02
				u8 SeekError : 1;    // 2 0x04
				u8 IdError : 1;      // 3 0x08
				u8 ShellOpen : 1;    // 4 0x10   "shell" refers to the physical door on the grey PlayStation case that covers the CD, not the software OS shell.
				u8 Read : 1;         // 5 0x20
				u8 Seek : 1;         // 6 0x40
				u8 Play : 1;         // 7 0x80    Playing CDDA
			};
		};
	};

	// Mode
	//
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
	struct Mode
	{
		union {
			u8 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u8 CDDA : 1;        // 0 0x01
				u8 AutoPause : 1;   // 1 0x02
				u8 Report : 1;      // 2 0x04
				u8 XA_Filter : 1;   // 3 0x08
				u8 IgnoreBit : 1;   // 4 0x10
				u8 SectorSize : 1;  // 5 0x20
				u8 XA_ADPCM : 1;    // 6 0x40
				u8 doubleSpeed : 1; // 7 0x80  0 = single speed (x1), 1 = double speed (x2)
			};
		};
	};

	// HCHPCTL (host chip control) register
	// aka Request register
	// Mapped to 1F801803 bank 0 for write
	//
	// Bit(s)
	//  0-4 0    Not used (should be zero)
	//  5   SMEN Want Command Start Interrupt on Next Command (0=No change, 1=Yes)
	//  6   BFWR ...                                      1=prepare for writes to WRDATA
	//  7   BFRD Request sector buffer read   Want Data   0=No/Reset Data Fifo, 1=Yes/Load Data Fifo i.e. prepare for reads from RDDATA
	//
	// n.b. All the docs are incorrect on this. BFRD does NOT clear or reset the data FIFO, it just allows/disallows reads from it.
	//
	struct HCHPCTL
	{
		union {
			u8 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u8 unusedBits4_0 : 5; // 0:4
				u8 SMEN : 1;
				u8 BFWR : 1; // allow writes to data FIFO via WRDATA register (not used on PSX I don't think)
				u8 BFRD : 1; // allow reads to data FIFO. n.b. All the docs are incorrect on this. BFRD does NOT clear or reset the data FIFO, it just allows/disallows reads from it.
			};
		};
	};

	const Stat& GetStat() const { return m_stat; }
	const Mode& GetMode() const { return m_mode; }

	unsigned int GetXAFilterFileNumber() const { return m_xaFilterFileNumber; };
	unsigned int GetXAFilterChannelNumber() const { return m_xaFilterChannelNumber; };

private:

	enum class Command
	{
		Nop,
		SetLoc,
		Play,
		ReadN,
		MotorOn, // Command 07h
		Stop,    // Command 08h
		Pause,
		Init,
		Setfilter,
		Demute,
		Setmode,
		GetlocP,
		GetTN,
		GetTD,
		SeekL,
		SeekP,
		Test,
		GetID,
		ReadS,

		Max = ReadS
	};

	unsigned int m_bank = 0;

	// HINTMASK host interrupt mask) register
	// aka Interrupt Enable Register
	u8 m_hintmsk = 0;

	// HINTSTS (host interrupt status) register
	// aka Interrupt Flag Register
	// Bits 2:0 are the current interrupt level: 0b001 = INT1, 0b010= INT2, 0b011 = INT3
	u8 m_hintsts = 0xe0; // HINTSTS bits 7:5 should always be 1

	// Command acknowledgments (INT3) must always posted before data interrupts (INT1) from Read/Play operations.
	// These flags allow this to be implemented
	// https://psx-spx.consoledev.net/cdromdrive/#responses
	bool m_int1pending{}; // Read sector available
	bool m_int2pending{}; // Non read/play second response

	Mode m_mode{};
	HCHPCTL m_hchpctl{};

	// Command parameters.
	// #TODO: Implement FIFO if required i.e. command does not consume all params.
	u8 m_params[16]{};
	unsigned int m_paramCount = 0;

	// Results FIFO aka Response FIFO
	// 16 byte buffer.
	u8 m_responseFIFO[16]{};
	unsigned int m_responseWriteIndex = 0;
	unsigned int m_responseReadIndex = 0;
	unsigned int m_responseCount = 0;

	// The SUB-CPU can hold up to eight sectors in 32K SRAM.
	// However, the SUB-CPU BIOS merely sets a sector-delivery-needed flag. Instead of memorizing which/how many sectors need to be
	// delivered, and, accordingly, the PSX can use only three of the available eight SRAM slots:
	// - One for currently pending INT1,
	// - one for undelivered INT1, and
	// - one for currently/incompletely received sector).
	// https://psx-spx.consoledev.net/cdromdrive/#responses
	static constexpr unsigned int kSectorDataBufferSizeBytes = 3 * CD::kSectorSizeBytes;
	static_assert(kSectorDataBufferSizeBytes < 32 * 1024);

	static constexpr unsigned int kCDDADataBufferSizeBytes = CD::kSectorSizeBytes; // #TODO: How large does this need to be?

	// Data FIFO aka sector buffer
	// Buffers data read from CD until read via RDDATA (0x1f801802) port.
	u8* m_sectorDataBufferStorage = nullptr;
	RingBuffer* m_pSectorDataBuffer = nullptr;
	u8 m_sectorDataByteValue = 0; // last value read. Returned when no more data in buffer to read.

	// CDDA FIFO
	u8* m_cddaBufferStorage = nullptr;
	RingBuffer* m_pCDDABuffer = nullptr;

	// XA-ADPCM data resampled to 44100 Hz
	u8* m_pXABufferStorage = nullptr;
	RingBuffer* m_pXABuffer = nullptr;

	unsigned int m_xaFilterFileNumber = 0;
	unsigned int m_xaFilterChannelNumber = 0;

	// Previous XA-ADPCM samples for interpolation
	// https://psx-spx.consoledev.net/cdromformat/#oldolder-values
	s16 m_xa_adcpm_old_left = 0;
	s16 m_xa_adcpm_older_left = 0;
	s16 m_xa_adcpm_old_right = 0;
	s16 m_xa_adcpm_older_right = 0;

	// It is important that the XA resampling ring buffer persists between sectors to ensure a smooth waveform.
	s16 m_xaResamplingRingBufferL[32]{};
	s16 m_xaResamplingRingBufferR[32]{};
	int m_xaResamplingRingBufferPos = 0;
	unsigned int m_xaSixstepCounter = 6;

	bool m_xaMuted = false;

	// CDDA volumes
	// 80h,0,80h,0 produce normal stereo volume,
	// 40h,40h,40h,40h produce mono output of equivalent volume
	u8 m_cdLeftLeftVolume_pending{}; // ATV0 write
	u8 m_cdLeftRightVolume_pending{}; // ATV1 write
	u8 m_cdRightRightVolume_pending{}; // ATV2 write
	u8 m_cdRightLeftVolume_pending{}; // ATV3 write
	u8 m_cdLeftLeftVolume{}; // Current volumes, which are applied when the CHNGATV flag in ADPCTL is set.
	u8 m_cdLeftRightVolume{};
	u8 m_cdRightRightVolume{};
	u8 m_cdRightLeftVolume{};

	// HSTS (host status) register BUSYSTS bit 7 is set only when a command first response is being processed.
	// After software issues a command, it can wait for this bit to be clear before issuing another.
	// This allows some parallelism: the software is not blocked while second response is outstanding.
	// #TODO: Remove this. Can just use m_commandFirstResponseEvent instead.
	bool m_firstResponseInProgress = false;

	// Command synchronization via IRQ as a lock
	// Commands are blocked if there is an unacknowledged interrupt (IRQ acts like a mutex).
	// When a command is received while an IRQ is pending, it's stored here and executed
	// when the IRQ is acknowledged via writeHCLRCTL.
	bool m_hasPendingCommand = false;
	u8 m_pendingCommand = 0;
	u8 m_pendingParams[16]{};
	unsigned int m_pendingParamCount = 0;

	// Physical drive state
	bool m_shellOpen = false; // In this context "shell" means the plastic case cover that is opened to put a CD in.
	const CD* m_pCD = nullptr;
	bool m_spinningUp = false;
	bool m_stopped = true;

	Stat m_stat{};

	// MSF (minutes, seconds, frames/fragments/sectors) seek params i.e. target head/laser location
	u8 m_targetLocMinutes = 0; // Seek position minutes in decimal, not BCD
	u8 m_targetLocSeconds = 0; // Seek position seconds in decimal, not BCD
	u8 m_targetLocFrames = 0; // Seek position frames (aka fragments, sectors) in decimal, not BCD. There are 75 sectors per second.
	unsigned int m_targetLBA = 0; // flattened seek location
	bool m_setLocPending = false;

	// Current head/laser location
	// Stored as linear Logical Block Address rather than MSF for convenience.
	// There is no need to store head position as a byte offset, because reads are always sector aligned.
	unsigned int m_headLBA = 0;

	u8 m_GetTD_trackNum = 0;

	// Only a single first and second response can be in flight at any one time.
	u32 m_commandFirstResponseEvent{};
	u32 m_commandSecondResponseEvent{};

	// Separate recurring event for Reads and Plays, which can run in parallel with other commands such as Pause, GetStat (Nop)
	u32 m_readSectorEvent{};

	INTC& m_intc; // interrupt controller for raising CDROM IRQ
	Scheduler& m_scheduler;

#if CDROM_SECTOR_DATA_BUFFER_DESCRIPTION_ENABLED
	// Debug human readable description of current contents of m_pSectorDataBuffer
	char m_debugSectorDataBufferDescription[128]{};
#endif

	void writeADDRESS(u8 val);
	void writeCOMMAND(u8 val);
	void writePARAMETER(u8 val);
	void writeHCLRCTL(u8 val);
	void writeWRDATA(u8 val);
	void writeHINTMASK(u8 val);
	void writeHCHPCTL(u8 val);

	void writeCI(u8 val);
	void writeATV0(u8 val);
	void writeATV1(u8 val);
	void writeATV2(u8 val);
	void writeATV3(u8 val);
	void writeADPCTL(u8 val);

	u8 readHSTS() const; // reg 0 all banks
	u8 readRESULT(); // reg 1 all banks
	u8 readRDDATA(); // reg 2 all banks
	u8 readHINTMASK() const; // reg 3 banks 0,2
	u8 readHINTSTS() const; // reg 3 banks 1,3

	// Sets CDROM interrupt flag register (HINTSTS) bits 2:0 to represent INT level (0b001 = INT1, 0b010= INT2, 0b011 = INT3 ...)
	// Returns true if the INT could be generated, false if had to be queued.
	bool generateINT(unsigned int index);

	// Execute a command (used by writeCOMMAND and when executing pending commands)
	void executeCommand(u8 command);

	void cancelCommandSecondResponse();
	void errorINT5(u8 errorByte);

	// Nop
	void executeNopCommand01();
	static void Nop_FirstResponseCallback(void* userdata);
	void executeNop_FirstResponse();

	// SetLoc
	void executeSetLocCommand02();
	static void SetLoc_FirstResponseCallback(void* userdata);
	void executeSetLoc_FirstResponse();

	// Play
	void executePlayCommand03();
	static void Play_FirstResponseCallback(void* userdata);
	void executePlay_FirstResponse();
	static void Play_ReadSectorCallback(void* userdata);
	void Play_ReadSector();

	// ReadN
	void executeReadNCommand06();
	static void ReadN_FirstResponseCallback(void* userdata);
	void executeReadN_FirstResponse();
	static void ReadSectorCallback(void* userdata);
	void readSector();

	// MotorOn
	void executeMotorOnCommand07();
	static void MotorOn_FirstResponseCallback(void* userdata);
	void executeMotorOn_FirstResponse();
	static void MotorOn_SecondResponseCallback(void* userdata);
	void executeMotorOn_SecondResponse();

	// Stop
	void executeStopCommand08();
	static void Stop_FirstResponseCallback(void* userdata);
	void executeStop_FirstResponse();
	static void Stop_SecondResponseCallback(void* userdata);
	void executeStop_SecondResponse();

	// Pause
	void executePauseCommand09();
	static void Pause_FirstResponseCallback(void* userdata);
	void executePause_FirstResponse();
	static void Pause_SecondResponseCallback(void* userdata);
	void executePause_SecondResponse();

	void cancelReadOrPlay();

	// Init
	void executeInitCommand0A();
	static void Init_FirstResponseCallback(void* userdata);
	void executeInit_FirstResponse();
	static void Init_SecondResponseCallback(void* userdata);
	void executeInit_SecondResponse();

	// Setfilter
	void executeSetfilterCommand0D();
	static void Setfilter_FirstResponseCallback(void* userdata);
	void executeSetfilter_FirstResponse();

	// Mute
	void executeMuteCommand0B();

	// Demute
	void executeDemuteCommand0C();
	static void Demute_FirstResponseCallback(void* userdata);
	void executeDemute_FirstResponse();

	// Setmode
	void executeSetmodeCommand0E();
	static void Setmode_FirstResponseCallback(void* userdata);
	void executeSetmode_FirstResponse();

	// GetlocL
	void executeGetlocLCommand10();
	static void GetlocL_FirstResponseCallback(void* userdata);
	void executeGetlocL_FirstResponse();

	// GetlocP
	void executeGetlocPCommand11();
	static void GetlocP_FirstResponseCallback(void* userdata);
	void executeGetlocP_FirstResponse();

	// GetTN
	void executeGetTNCommand13();
	static void GetTN_FirstResponseCallback(void* userdata);
	void executeGetTN_FirstResponse();

	// GetTD
	void executeGetTDCommand14();
	static void GetTD_FirstResponseCallback(void* userdata);
	void executeGetTD_FirstResponse();

	// SeekL
	void executeSeekLCommand15();
	static void SeekL_FirstResponseCallback(void* userdata);
	void executeSeekL_FirstResponse();
	static void SeekL_SecondResponseCallback(void* userdata);
	void executeSeekL_SecondResponse();

	// SeekP
	void executeSeekPCommand16();
	static void SeekP_FirstResponseCallback(void* userdata);
	void executeSeekP_FirstResponse();
	static void SeekP_SecondResponseCallback(void* userdata);
	void executeSeekP_SecondResponse();

	// Test commands
	void executeTestCommand19();
	static void TestCommand_FirstResponseCallback(void* userdata);
	void executeTestCommand_FirstResponse();

	// GetID
	void executeGetIDCommand1A();
	static void GetID_FirstResponseCallback(void* userdata);
	void GetID_FirstResponse();
	static void GetID_SecondResponseCallback(void* userdata);
	void GetID_SecondResponse();

	// ReadS can be emulated using ReadN
	void executeReadSCommand1B();
	static void ReadS_FirstResponseCallback(void* userdata);
	void executeReadS_FirstResponse();

	// Parameter FIFO
	void resetParameterFIFO();

	// Response FIFO
	void resetResponseFIFO();
	void writeResponseFIFO(u8 val);
	u8 readResponseFIFO();

	// Sector data FIFO
	u8 readSectorDataFIFOByte();
	u32 readSectorDataFIFOWord();

	void resetXABuffer();
	void decodeXAADPCMSector(const u8* pSector, u8 codingInfo);
	void resampleStereo(s16 samplesL[XAADPCM::kWordsPerChunk], s16 samplesR[XAADPCM::kWordsPerChunk], bool is18900Hz);
	void resampleMono(s16 samples[2 * XAADPCM::kWordsPerChunk], bool is18900Hz);

	unsigned int findCurrentTrackIndex() const;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
