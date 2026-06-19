// PSX Sound Processing Unit (SPU) emulation
//
// The SPU plays samples; it does not generate waves.
//
// https://psx-spx.consoledev.net/soundprocessingunitspu/
// https://jsgroth.dev/blog/posts/ps1-spu-part-1/

#pragma once

#include "core/Types.h"

#define SPU_FATAL_UNIMPLEMENTED 1

class CDROM;
class INTC;
class Scheduler;

inline bool g_logSPU = false;
inline bool g_logSPUDataTransfer = false; // very verbose when BIOS is starting up and clearing reverb memory

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif

class SPU
{
public:

	static const unsigned int kVoiceCount = 24;

	// CD quality audio: 44.1 kHz 16 bit
	static constexpr unsigned int kOutputSampleRate = 44100; // Hz
	static constexpr unsigned int kBitDepth = 16;

	enum class ADSRPhase
	{
		Attack,
		Decay,
		Sustain,
		Release,
		None,

		Max = None
	};

	typedef void AudioFrameCallback(const s16 samples[2]); // Two channels: left, right

	enum class SoundRAMTransferMode : u16 // n.b. explicit unsigned storage type so can be stored in 2 bits of bitfield without sign problems
	{
		Stop,
		ManualWrite,
		DMAWrite,
		DMARead,

		Max = DMARead
	};

	// 39-tap PSX SPU FIR filter coefficients, used for downsampling/upsampling between 44100 Hz and 22050 Hz for the reverb unit.
	static const unsigned int kFIRFilterSize = 39;

	// Each 16-byte ADPCM encoded block stores 28 samples.
	static const unsigned int kADPCMEncodedBlockSizeBytes = 16;
	static const unsigned int kADPCMSamplesPerBlock = 28;

	// Eight 16 bit regs per voice
	// #TODO: What are initial values for voice registers?
	struct Voice
	{
		static const unsigned int kMaxAttackShift = 0x1f;
		static const unsigned int kMaxDecayShift = 0xf;    // only 4 bits for decay (see below structs)
		static const unsigned int kMaxSustainShift = 0x1f;
		static const unsigned int kMaxReleaseShift = 0x1f;

		// 1F801C08h+N*10h - Voice 0..23 Attack/Decay/Sustain/Release (ADSR)
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801c08hn10h-voice-023-attackdecaysustainrelease-adsr-32bit
		// Note:
		// - Attack direction is not configurable, it always increases (until 7FFFh)
		// - Decay direction is not configurable, it always decreases (until sustain level)
		// - Decay mode is not configurable, it is always exponential.
		// - Decay step is not configurable, it is always -8
		struct AttackDecaySustainLevel
		{
			union {
				u16 val;

				// n.b. Bits are ordered from least to most significant
				struct {
					u16 sustainLevel : 4; // bits 3:0 [0,f] Level=(N+1)*800h
					u16 decayShift : 4;   // bits 7:4 [0,f] = fast to slow
					u16 attackStep : 2;   // bits 9:8 [0,3] = 7,6,5,4
					u16 attackShift : 5;  // bits 14:10  [0,1f] = fast to slow
					u16 attackMode : 1;   // bit 15  0 = linear, 1 = exponential
				};
			};
		};

		// Note:
		// - Release direction is not configurable, it always decreases (until FFFFh)
		// - Release step is not configurable, it is always -8
		struct SustainReleaseRate
		{
			union {
				u16 val;

				// n.b. Bits are ordered from least to most significant
				struct {
					u16 releaseShift : 5;     // bits 4:0 [0,1f] = fast to slow
					u16 releaseMode : 1;      // bit 5 0 = linear decrease, 1 = exponential decrease
					u16 sustainStep : 2;      // bits 7:6 [0,3] = 7,6,5,4 if increasing, -8,-7,-6,-5 if decreasing
					u16 sustainShift : 5;     // bits 12:8 [0,1f] = fast to slow
					u16 unusedBit13 : 1;      // bit 13 unused? (usually zero)
					u16 sustainDirection : 1; // bit 14 "Sd"  0 = increase, 1 = decrease (until key off)
					u16 sustainMode : 1;      // bit 15 "Sm"  0 = linear, 1 = exponential
				};
			};
		};

		union {
			u16 reg[8];

			struct {
				// 1F801C00h+N*10h - Voice 0..23 Volume Left
				// 1F801C02h+N*10h - Voice 0..23 Volume Right
				// Bit 15 selects mode: 0 = Volume mode, 1 = Sweep mode
				// In volume mode the 15 remaining bits are the *signed* volume, where the sign bit can be used to invert the phase.
				// In sweep mode #TODO: See spu.txt
				// See https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801c02hn10h-voice-023-volume-right
				u16 volumeLeft;
				u16 volumeRight;

				// 1F801C04h+N*10h - Voice 0..23 ADPCM Sample Rate (R/W) (VxPitch)
				// 
				// The ADPCM sample rate affects the speed at which the voice moves through decoded samples.
				// 
				// - 0x1000 equates to 44100 Hz, which moves forward one sample each clock.
				// - 0x4000 is the max value, which moves forward 4 samples per clock.
				// - 0x0800 would move forward one sample every other clock.
				//
				// The sample rate is added to the pitch counter at 44100 Hz
				u16 sampleRate;

				// 1F801C06h+N*10h - Voice 0..23 ADPCM Start Address (R/W)
				// This is in 8-byte units, so real sound RAM address is this * 8
				u16 startAddressDiv8;

				// 1F801C08h+N*10h - Voice 0..23 Attack/Decay/Sustain/Release (ADSR)
				// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801c08hn10h-voice-023-attackdecaysustainrelease-adsr-32bit
				AttackDecaySustainLevel attackDecaySustainLevel;
				SustainReleaseRate sustainReleaseRate;

				// #TODO: Should this be signed or unsigned?
				s16 currentADSREnvelopeLevel;

				// 1F801C0Eh+N*10h - Voice 0..23 ADPCM Repeat Address (R/W)
				u16 repeatAddressDiv8;
			};
		};

		// ADSR envelope state
		struct ADSR
		{
			ADSRPhase phase = ADSRPhase::None;
			unsigned int counter{};
			unsigned int counterIncrement{};
			int step{};
			bool decreasing{};
			bool exponential{};
			bool phaseNegative{};
		};

		// Internal state
		u32 currentADPCMAddress{};

		// Bits 15:12 are sample index within an ADPCM block
		// Bits 11:4 are 8-bit Gaussian interpolation value.
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#pitch-counter
		int pitchCounter{};

		s16 decodedSamples[kADPCMSamplesPerBlock]{};
		unsigned int currentSampleIndex{}; // which sample in the buffer is currently being played.

		// Element 0 is the value of the currnet sample, elements 1:3 are previous samples, used for Gaussian interpolation.
		// Note that previous three *decoded* samples, not the previous three output samples (I guess samples could be skipped at high sample rates).
		s16 sampleHistory[4]{};

		s16 sample{}; // The final interpolated sample

		ADSR adsr{};
	};

	/*
	1F801DAEh - SPU Status Register (SPUSTAT) (R)

	15-12 Unknown/Unused (seems to be usually zero)
	11    Writing to First/Second half of Capture Buffers (0=First, 1=Second)
	10    Data Transfer Busy Flag          (0=Ready, 1=Busy)
	9     Data Transfer DMA Read Request   (0=No, 1=Yes)
	8     Data Transfer DMA Write Request  (0=No, 1=Yes)
	7     Data Transfer DMA Read/Write Request ;seems to be same as SPUCNT.Bit5
	6     IRQ9 Flag                        (0=No, 1=Interrupt Request)
	5-0   Current SPU Mode   (same as SPUCNT.Bit5-0, but, applied a bit delayed)
	*/
	struct Status
	{
		union {
			u16 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u16 CDAudioEnable : 1;                         // 0
				u16 ExternalAudioEnable : 1;                   // 1
				u16 CDAudioReverb : 1;                         // 2
				u16 ExternalAudioReverb : 1;                   // 3
				SoundRAMTransferMode soundRAMTransferMode : 2; // 5:4
				u16 irq : 1;                                   // 6
				u16 dmaReadWriteRequest : 1;                   // 7
				u16 dmaWriteRequest : 1;                       // 8
				u16 dmaReadRequest : 1;                        // 9
				u16 dataTransferBusy : 1;                      // 10
				u16 captureBufferHalf : 1;                     // 11
				u16 unknownBits15_12 : 4;                      // 15:12
			};
		};
	};

	/*
	1F801DAAh - SPU Control Register (SPUCNT)

	  15    SPU Enable              (0=Off, 1=On)       (Don't care for CD Audio)
	  14    Mute SPU                (0=Mute, 1=Unmute)  (Don't care for CD Audio)
	  13-10 Noise Frequency Shift   (0..Fh = Low .. High Frequency)
	  9-8   Noise Frequency Step    (0..03h = Step "4,5,6,7")
	  7     Reverb Master Enable    (0=Disabled, 1=Enabled)
	  6     IRQ9 Enable (0=Disabled/Acknowledge, 1=Enabled; only when Bit15=1)
	  5-4   Sound RAM Transfer Mode (0=Stop, 1=ManualWrite, 2=DMAwrite, 3=DMAread)
	  3     External Audio Reverb   (0=Off, 1=On)
	  2     CD Audio Reverb         (0=Off, 1=On) (for CD-DA and XA-ADPCM)
	  1     External Audio Enable   (0=Off, 1=On)
	  0     CD Audio Enable         (0=Off, 1=On) (for CD-DA and XA-ADPCM)
	*/
	struct SPUCNT
	{
		union {
			u16 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u16 CDAudioEnable : 1;                          // 0
				u16 ExternalAudioEnable : 1;                    // 1
				u16 CDAudioReverb : 1;                          // 2
				u16 ExternalAudioReverb : 1;                    // 3
				SoundRAMTransferMode soundRAMTransferMode : 2; // 5:4
				u16 IRQ9Enable : 1;                             // 6
				u16 ReverbMasterEnable : 1;                     // 7
				u16 NoiseFrequencyStep : 2;                     // 9:8
				u16 NoiseFrequencyShift : 4;                    // 13:10
				u16 MuteSPU : 1;                                // 14  n.b. Active low mute (0=Mute, 1=Unmute)
				u16 SPUEnable : 1;                              // 15
			};
		};
	};

	// Each 32-bit register contains 1 bit for each of the 24 voices.
	// #TODO: What are initial values for voice flags?
	struct VoiceFlags
	{
		// 1F801D88h - Voice 0..23 Key ON (Start Attack/Decay/Sustain) (KON) (Write only)
		//   0-23  Voice 0..23 On  (0=No change, 1=Start Attack/Decay/Sustain)
		//  24-31 Not used
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d88h-voice-023-key-on-start-attackdecaysustain-kon-w
		u32 kon{}; // 1F801D88h

		// 1F801D8Ch - Voice 0..23 Key OFF (Start Release) (KOFF) (Write only)
		//   0-23  Voice 0..23 Off (0=No change, 1=Start Release)
		//  24-31 Not used
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d8ch-voice-023-key-off-start-release-koff-w
		u32 koff{}; // 1F801D8Ch

		// 1F801D90h Voice 0..23 Pitch Modulation (FM LFO) Enable Flags (PMON) (R/W)
		u32 pmon{};

		// 1F801D94h Voice 0..23 Noise mode enable (NON) (R/W)
		//   0-23  Voice 0..23 Noise (0=ADPCM, 1=Noise)
		//   24-31 Not used
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d94h-voice-023-noise-mode-enable-non
		u32 non{};

		// 1F801D98h 4  Voice 0..23 Channel Reverb mode aka Echo On (EON) (R/W)
		//   0-23  Voice 0..23 Destination (0=To Mixer, 1=To Mixer and to Reverb)
		//  24-31 Not used
		// #TODO: Add notes
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d98h-voice-023-reverb-mode-aka-echo-on-eon-rw
		u32 eon{};

		// 1F801D9Ch - Voice 0..23 ON/OFF (status) (ENDX) (Read-only)
		// 
		//   0-23  Voice 0..23 Status (0=Newly Keyed On, 1=Reached LOOP-END)
		//  24-31 Not used
		// 
		// The bits get CLEARED when setting the corresponding KEY ON bits.
		// The bits get SET when reaching an LOOP-END flag in ADPCM header.bit0.
		// 
		// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801d9ch-voice-023-onoff-status-endx-r
		u32 endx{};
	};

	// Reverb Volume and Address Registers (R/W)
	//
	//   Port      Reg   Name    Type    Expl.
	//   1F801D84h spu   vLOUT   volume  Reverb Output Volume Left
	//   1F801D86h spu   vROUT   volume  Reverb Output Volume Right
	//   1F801DA2h spu   mBASE   base    Reverb Work Area Start Address in Sound RAM
	//   1F801DC0h rev00 dAPF1   disp    Reverb APF Offset 1
	//   1F801DC2h rev01 dAPF2   disp    Reverb APF Offset 2
	//   1F801DC4h rev02 vIIR    volume  Reverb Reflection Volume 1
	//   1F801DC6h rev03 vCOMB1  volume  Reverb Comb Volume 1
	//   1F801DC8h rev04 vCOMB2  volume  Reverb Comb Volume 2
	//   1F801DCAh rev05 vCOMB3  volume  Reverb Comb Volume 3
	//   1F801DCCh rev06 vCOMB4  volume  Reverb Comb Volume 4
	//   1F801DCEh rev07 vWALL   volume  Reverb Reflection Volume 2
	//   1F801DD0h rev08 vAPF1   volume  Reverb APF Volume 1
	//   1F801DD2h rev09 vAPF2   volume  Reverb APF Volume 2
	//   1F801DD4h rev0A mLSAME  src/dst Reverb Same Side Reflection Address 1 Left
	//   1F801DD6h rev0B mRSAME  src/dst Reverb Same Side Reflection Address 1 Right
	//   1F801DD8h rev0C mLCOMB1 src     Reverb Comb Address 1 Left
	//   1F801DDAh rev0D mRCOMB1 src     Reverb Comb Address 1 Right
	//   1F801DDCh rev0E mLCOMB2 src     Reverb Comb Address 2 Left
	//   1F801DDEh rev0F mRCOMB2 src     Reverb Comb Address 2 Right
	//   1F801DE0h rev10 dLSAME  src     Reverb Same Side Reflection Address 2 Left
	//   1F801DE2h rev11 dRSAME  src     Reverb Same Side Reflection Address 2 Right
	//   1F801DE4h rev12 mLDIFF  src/dst Reverb Different Side Reflect Address 1 Left
	//   1F801DE6h rev13 mRDIFF  src/dst Reverb Different Side Reflect Address 1 Right
	//   1F801DE8h rev14 mLCOMB3 src     Reverb Comb Address 3 Left
	//   1F801DEAh rev15 mRCOMB3 src     Reverb Comb Address 3 Right
	//   1F801DECh rev16 mLCOMB4 src     Reverb Comb Address 4 Left
	//   1F801DEEh rev17 mRCOMB4 src     Reverb Comb Address 4 Right
	//   1F801DF0h rev18 dLDIFF  src     Reverb Different Side Reflect Address 2 Left
	//   1F801DF2h rev19 dRDIFF  src     Reverb Different Side Reflect Address 2 Right
	//   1F801DF4h rev1A mLAPF1  src/dst Reverb APF Address 1 Left
	//   1F801DF6h rev1B mRAPF1  src/dst Reverb APF Address 1 Right
	//   1F801DF8h rev1C mLAPF2  src/dst Reverb APF Address 2 Left
	//   1F801DFAh rev1D mRAPF2  src/dst Reverb APF Address 2 Right
	//   1F801DFCh rev1E vLIN    volume  Reverb Input Volume Left
	//   1F801DFEh rev1F vRIN    volume  Reverb Input Volume Right
	//
	// All volume registers are signed 16bit (range -8000h..+7FFFh).
	// All src/dst/disp/base registers are addresses in SPU memory (divided by 8)
	// src/dst are relative to the current buffer address
	// disp registers are relative to src registers
	// The base register defines the start address of the reverb buffer (the end address is fixed, at 7FFFEh).
	// Writing a value to mBASE does additionally set the current buffer address to that value.
	// 
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-reverb-registers
	//
	struct ReverbRegisters
	{
		union {
			u16 reg[32];

			struct {
				u16 dAPF1;
				u16 dAPF2;
				s16 vIIR;
				s16 vCOMB1;
				s16 vCOMB2;
				s16 vCOMB3;
				s16 vCOMB4;
				s16 vWALL;
				s16 vAPF1;
				s16 vAPF2;
				u16 mLSAME;
				u16 mRSAME;
				u16 mLCOMB1;
				u16 mRCOMB1;
				u16 mLCOMB2;
				u16 mRCOMB2;
				u16 dLSAME;
				u16 dRSAME;
				u16 mLDIFF;
				u16 mRDIFF;
				u16 mLCOMB3;
				u16 mRCOMB3;
				u16 mLCOMB4;
				u16 mRCOMB4;
				u16 dLDIFF;
				u16 dRDIFF;
				u16 mLAPF1;
				u16 mRAPF1;
				u16 mLAPF2;
				u16 mRAPF2;
				s16 vLIN;
				s16 vRIN;
			};
		};
	};

	SPU(INTC& intc, Scheduler& scheduler, CDROM& cdrom);
	~SPU();

	void Reset();

	void Write16(unsigned int address, u16 val);
	void Write32(unsigned int address, u32 val);
	u16 Read16(unsigned int address);

	// Write 32-bit value to 1F801DA8h i.e. directly to SPU RAM, bypassing the FIFO register at 1F801DA8h.
	// Used for immediate DMA transfers from RAM to SPU RAM for MDEC audio decompression.
	void WriteData32(u32 val);

	// Optimisation for immediate DMA to copy directly from RAM into MDEC.
	void WriteDataBlock(const u8* data, unsigned int numWords);

	// Optimisation for immediate DMA to copy directly from sector data buffer into RAM.
	// Caller should check that required number of bytes are available and ensure that dst buffer is large enough.
	void ReadDataBlock(u8* dst, unsigned int numWords);

	// SPU clock is 44100 Hz, which is exactly CPU clock / 768
	void Clock();

	void SetAudioFrameCallback(AudioFrameCallback* pCallback) { m_pAudioFrameCallback = pCallback; }

	// Only used for debugging. #TODO: Consider using friend instead
	SPUCNT& GetSPUCNT() { return m_spucnt; }
	const SPUCNT& GetSPUCNT() const { return m_spucnt; }
	const Status& GetSPUSTAT() const { return m_stat; }
	const Voice& GetVoice(unsigned int voiceIndex) const;
	const VoiceFlags& GetVoiceFlags() const { return m_voiceFlags; }
	u16 GetMainVolumeLeft() const { return m_mainVolumeLeft; }
	u16 GetMainVolumeRight() const { return m_mainVolumeRight; }
	s16 GetCDVolumeLeft() const { return m_cdVolumeLeft; }
	s16 GetCDVolumeRight() const { return m_cdVolumeRight; }
	u32 GetSoundRAMIRQAddress() const { return m_soundRAMIRQAddress; }
	u16 GetReverbBaseAddressDiv8() const { return m_soundRAMReverbWorkAreaStartAddressDiv8; } // convert back to real address
	s16 GetReverbOutputVolumeLeft() const { return m_reverbOutputVolumeLeft; }   // 1F801D84h "vLOUT" Reverb Output Volume Left. Signed 16-bit, range -8000h..7FFFh, where the sign bit can be used to invert the phase.
	s16 GetReverbOutputVolumeRight() const { return m_reverbOutputVolumeRight; } // 1F801D86h "vROUT" Reverb Output Volume Right Signed 16-bit, range -8000h..7FFFh, where the sign bit can be used to invert the phase.
	const ReverbRegisters& GetReverbRegisters() const { return m_rev; }

	void DebugDisableReverb(bool val) { m_debugDisableReverb = val; }
	bool DebugIsReverbDisabled() const { return m_debugDisableReverb; }

private:

	void updateNoise();
	void clockVoices();
	void readCDDAudioSamples(s16 cdSamples[2]);
	void updateCaptureBuffers(const s16 cdSamples[2]);
	void sample(s16 spuSamples[2], s32 reverbSamples[2]) const; // 2 channels (stereo) = 2 samples per frame
	void mix(const s16 spuSamples[2], const s16 cdSamples[2], const s16 reverbOutput[2], s16 mixedSamples[2]) const;
	void updateReverb(const s32 reverbVoiceSamples[2], const s16 cdSamples[2], s16 output[2]);

	void keyOn(u32 voiceBits);
	void keyOff(u32 voiceBits);

	void decodeVoiceBlock(Voice& voice, unsigned int voiceIndex);
	void transitionVoiceToAttack(Voice& voice);
	void transitionVoiceToDecay(Voice& voice);
	void transitionVoiceToSustain(Voice& voice);
	void transitionVoiceToRelease(Voice& voice);
	void transitionVoiceToADSRPhase(Voice& voice, unsigned int stepValue, unsigned int shift, unsigned int maxShift);
	void clockVoiceADSREnvelope(Voice& voice);

	void scheduleClockCallback();
	static void clockCallback(void* userdata);

	void writeRAM16(u32 address, u16 val);

	// Reverb helpers
	u32 reverbAddrToRamAddr(u16 addressDiv8) const;
	s16 readSampleFromReverbBuffer(u16 addressDiv8);
	void writeSampleToReverbBuffer(u16 addressDiv8, s16 val);
	s16 readReverbSampleFromRAM(u32 address);
	void writeReverbSampleToRAM(u32 address, s16 val);

	s32 applyReflection(s32 input, u16 m_addr, u16 d_addr, s16 vIIR, s16 vWALL);
	s32 applyEarlyEcho(u16 mComb1, u16 mComb2, u16 mComb3, u16 mComb4, s16 vComb1, s16 vComb2, s16 vComb3, s16 vComb4);
	s32 applyLateReverb(s32 input, u16 m_addr, u16 d_addr, s16 vAPF);

	// 1F801C00h..1F801D7Fh - Voice 0..23 Registers (eight 16 bit regs per voice)
	Voice m_voices[kVoiceCount]{};

	// Bit 15 selects mode: 0 = Volume mode, 1 = Sweep mode
	// In volume mode the 15 remaining bits are the *signed* volume, where the sign bit can be used to invert the phase.
	// In sweep mode #TODO: See spu.txt
	u16 m_mainVolumeLeft{};  // 1F801D80h
	u16 m_mainVolumeRight{}; // 1F801D82h


	// 1F801D88h..1F801D9Fh - Voice 0..23 Flags (six 1bit flags per voice)
	VoiceFlags m_voiceFlags{};

	// 1F801DA4h - Sound RAM IRQ Address (IRQ9)
	// Only used in some games. See https://psx-spx.consoledev.net/soundprocessingunitspu/#note_1
	u32 m_soundRAMIRQAddress{}; // Implementation note: Store the actual sound RAM address, not the address/8 as written to the register.

	// 1F801DA6h - Sound RAM Data Transfer Address
	// Address in sound buffer divided by eight
	u16 m_soundRAMDataTransferAddress{};
	u32 m_currentSoundRAMDataTransferAddress{}; // This internal register increments, the visible one doesn't

	Status m_stat{};   // 1F801DAEh - SPU Status Register (SPUSTAT) (R)
	SPUCNT m_spucnt{}; // 1F801DAAh - SPU Control Register (SPUCNT)

	// 1F801DACh - Sound RAM Data Transfer Control (should be 0004h)
	u16 m_soundRAMDataTransferControl{};

	// 1F801DB0h (2x 16 bit) - CD Audio Input Volume (for normal CD-DA, and compressed XA-ADPCM)
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801db0h-cd-audio-input-volume-for-normal-cd-da-and-compressed-xa-adpcm
	s16 m_cdVolumeLeft{};
	s16 m_cdVolumeRight{};

	// 1F801DB4h - External Audio Input Volume
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801db4h-external-audio-input-volume
	u16 m_externalVolumeLeft{};
	u16 m_externalVolumeRight{};

	// 1F801DB8h - Current Main Volume Left/Right
	// These are internal registers, normally not used by software (the volume settings are usually set via Main Volume 1F801D80h/1F801D82h and voice volume registers 1F801C00h+N*10h
	// https://psx-spx.consoledev.net/soundprocessingunitspu/#1f801db8h-current-main-volume-leftright
	u16 m_currentMainVolumeLeft{};
	u16 m_currentMainVolumeRight{};

#pragma region Reverb
	// 1F801DA2h Sound RAM Reverb Work Area Start Address "mBASE"
	u16 m_soundRAMReverbWorkAreaStartAddressDiv8{}; // address divided by 8
	u32 m_reverbBufferStartAddress{}; // actual address in SPU RAM, calculated from the above register
	u32 m_currentReverbBufferHead{}; // Current reverb buffer head address in RAM. This is incremented by sample size (2 bytes) after each pass of the reverb.
	unsigned int m_reverbBufferSizeBytes; // the reverb buffer always extends from mBASE to end of RAM
	s16 m_reverbOutputVolumeLeft{}; // 1F801D84h "vLOUT"  Reverb Output Volume Left. Signed 16-bit, range -8000h..7FFFh, where the sign bit can be used to invert the phase.
	s16 m_reverbOutputVolumeRight{}; // 1F801D86h "vROUT" Reverb Output Volume Right Signed 16-bit, range -8000h..7FFFh, where the sign bit can be used to invert the phase.

	ReverbRegisters m_rev{};
	bool m_reverbCounter{}; // reverb is updated every other sample, so this is used to track whether to update it on the current sample or not.

	// Downsampler for reverb unit from 44100 Hz to 22050 Hz
	s16 m_downsamplerRingbufferL[kFIRFilterSize]{};
	s16 m_downsamplerRingbufferR[kFIRFilterSize]{};
	unsigned int m_downsamplerRingbufferIndex = 0; // Circular buffer index for downsampler input

	// Upsampler from reverb unit from 22050 Hz back to 44100 Hz
	s16 m_upsamplerRingbufferL[kFIRFilterSize]{};
	s16 m_upsamplerRingbufferR[kFIRFilterSize]{};
	unsigned int m_upsamplerRingbufferIndex = 0; // Circular buffer index for upsampler input

	bool m_debugDisableReverb = false;

#pragma endregion Reverb

	// SPU has 512 KiB internal RAM
	// 
	//  00000h-003FFh  CD Audio left  (1Kbyte) ;\CD Audio before Volume processing
	//  00400h-007FFh  CD Audio right (1Kbyte) ;/signed 16bit samples at 44.1kHz
	//  00800h-00BFFh  Voice 1 mono   (1Kbyte) ;\Voice 1 and 3 after ADSR processing
	//  00C00h-00FFFh  Voice 3 mono   (1Kbyte) ;/signed 16bit samples at 44.1kHz
	//  01000h-xxxxxh  ADPCM Samples  (first 16bytes usually contain a Sine wave)
	//  xxxxxh-7FFFFh  Reverb work area
	// 
	// First 4 KiB is reserved for "capture buffers" #TODO: What are these.
	// Reverb buffer is at the end of RAM (BIOS seems to manually clear 70000h upwards)
	//
	static const unsigned int kRamSizeBytes = 512 * 1024; // 512 KiB
	u8* m_ram{};

	unsigned int m_captureBufferWriteOffset{};

	int m_noiseTimer = 0;
	s16 m_noiseLevel = 0;

	AudioFrameCallback* m_pAudioFrameCallback = nullptr;

	INTC& m_intc; // interrupt controller for raising CDROM IRQ
	Scheduler& m_scheduler;
	CDROM& m_cdrom;

public:
		// Debug
	u32 m_debugVoiceEnabled = 0xffffff; // 24 voices
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline const char* kADSRPhaseNames[] =
{
	"Attack",
	"Decay",
	"Sustain",
	"Release",
	"None",
};

inline const char* kSoundRAMTransferModeNames[] =
{
	"Stop",
	"ManualWrite",
	"DMAWrite",
	"DMARead",
};

inline const char* kReverbRegisterNames[] =
{
	"dAPF1",
	"dAPF2",
	"vIIR",
	"vCOMB1",
	"vCOMB2",
	"vCOMB3",
	"vCOMB4",
	"vWALL",
	"vAPF1",
	"vAPF2",
	"mLSAME",
	"mRSAME",
	"mLCOMB1",
	"mRCOMB1",
	"mLCOMB2",
	"mRCOMB2",
	"dLSAME",
	"dRSAME",
	"mLDIFF",
	"mRDIFF",
	"mLCOMB3",
	"mRCOMB3",
	"mLCOMB4",
	"mRCOMB4",
	"dLDIFF",
	"dRDIFF",
	"mLAPF1",
	"mRAPF1",
	"mLAPF2",
	"mRAPF2",
	"vLIN",
	"vRIN",
};
