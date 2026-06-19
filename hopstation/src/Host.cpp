#include "Host.h"

#include "HostAudioBuffer.h"

#include "AudioDevice.h"

#include "Texture.h"
#include "Renderer.h"

#include "psx-utils/VRAMConvert.h"
#include "psx-utils/TTYLogger.h"

#include "psx/CD.h"
#include "psx/R3000Dasm.h"
#include "psx/Bus.h"

#include "core/RingBuffer.h"
#include "core/Helpers.h"
#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/StringHelpers.h"
#include "core/MathsHelpers.h"

#include "imgui.h"

#include "SDL3/SDL_audio.h"

#include <string.h> // memset

static SDL_Window* s_pWindow;

static Bus s_bus; // the PSX machine

// Emulator display
static u32 s_displayImageData[GPU::kMaxDisplayWidthPixels * GPU::kMaxDisplayHeightPixels];
static Texture* s_pDisplayTexture;
static unsigned int s_displayHeight;

// VRAM
static constexpr unsigned int kVRAMTextureWidthPixels = kVRAMWidth16bpp;
static u32 s_vramImageData[kVRAMTextureWidthPixels * kVRAMHeightLines];
static Texture* s_pVramTexture;

static constexpr unsigned int kHostAudioSampleRate = 44100; // Match PSX SPU output sample rate
static constexpr unsigned int kHostBufferSizeFrames = kHostAudioSampleRate; // 1 second of audio
static HostAudioBuffer* s_pEmulatorAudioBuffer; // PCM buffer written to by emulator
static bool s_audioEnabled;

// Audio resampling
static bool s_dynamicAudioResamplingEnabled = true;
static constexpr unsigned int s_targetAudioLatencyMs = 40; // a couple of frames
static constexpr float kAudioResamplingSmoothingFactor = 0.95f; // Higher = smoother/slower reaction
static constexpr float kAudioResamplingGain = 0.01f;
static constexpr float kAudioResamplingMaxDeltaFrequencyRatio = 0.005f;  // Limit to 0.5% change (hardly audible)
static float s_currentAudioResamplingFrequencyRatio = 1.0f;

static CD* s_pCD; // a single CD

static TTYLogger s_ttyLogger;

#define DEBUGGER_ENABLED 0 // Remove conditional from core loop unless required.
#if DEBUGGER_ENABLED
static bool s_logDisassembly = false;
static u32 s_breakpointAddress = 0x80011718;
#endif

static bool initHostAudio()
{
	// Request a device buffer size of 512 frames / 44100 frames/sec = 11.6 ms
	// This is currently ignored, see AudioDevice::Init()
	unsigned int requestedBufferSizeInSampleFrames = 512;

	if (AudioDevice::Init(requestedBufferSizeInSampleFrames, kHostAudioSampleRate))
	{
		LOG_INFO("Audio device initialised.\n");
	}
	else
	{
		LOG_ERROR("Failed to initialise audio device\n");
		return false;
	}

	// Create buffer for signed 16-bit stereo PCM samples from emulator
	HP_ASSERT(s_pEmulatorAudioBuffer == nullptr);
	s_pEmulatorAudioBuffer = new HostAudioBuffer(kHostBufferSizeFrames * kAudioChannelCount);

	return true;
}

//
// Called by emulator when a new audio frame is available.
// This is timed to occur at exactly 44100 Hz for the PSX SPU
//
static void audioFrameCallback(const int16_t spuSamples[2])
{
	if (s_pEmulatorAudioBuffer)
	{
 		s_pEmulatorAudioBuffer->WriteSample(spuSamples[0]); // L
 		s_pEmulatorAudioBuffer->WriteSample(spuSamples[1]); // R

		// Newly-opened audio devices start in the paused state. #TODO: Does this still apply in SDL3?
		// #TODO: Only unpause when emulator is running
		if (AudioDevice::IsPaused())
			AudioDevice::Resume();

		// Prevent audio buffer under-run or over-run by dynamically adjusting the sample rate slightly to speed up or slow down the audio output.
		if (s_dynamicAudioResamplingEnabled)
		{
			static float s_smoothedError = 0.0f; // Exponential Moving Average (EMA)

			unsigned int current_queued_frames = AudioDevice::GetQueueSizeFrames();

			unsigned int deviceSampleRate = AudioDevice::GetSampleRate();
			unsigned int targetFrames = (deviceSampleRate * s_targetAudioLatencyMs) / 1000;

			float rawError;
			if (targetFrames == 0)
				rawError = 0;
			else
				rawError = (float)((int)current_queued_frames - (int)targetFrames) / (float)targetFrames;

			// Exponential Moving Average (EMA)
			// This filters out the "noise" from SDL's stepped updates
			s_smoothedError = (s_smoothedError * kAudioResamplingSmoothingFactor) + (rawError * (1.0f - kAudioResamplingSmoothingFactor));

			// Apply a very small gain; we only want to nudge the ratio, not jerk it.
			float deltaFrequencyRatio = s_smoothedError * kAudioResamplingGain;

			// Tight Clamping:  Humans start noticing pitch shifts around 0.5% to 1.0%. 
			// Stay well below that for micro-adjustments.
			deltaFrequencyRatio = Clamp(deltaFrequencyRatio, -kAudioResamplingMaxDeltaFrequencyRatio, kAudioResamplingMaxDeltaFrequencyRatio);

			s_currentAudioResamplingFrequencyRatio = 1.0f + deltaFrequencyRatio;

			SDL_SetAudioStreamFrequencyRatio(AudioDevice::GetAudioStream(), s_currentAudioResamplingFrequencyRatio);

 			static int count = 0;
			if (++count % 128 == 0 && false) // Logging every call (44100 times per second) affects performance
 				LOG_INFO("Audio queue size=%u, target=%u, raw_error=%.2f%%, smoothed_error=%.2f%%, frequency ratio delta: %.4f\n",
					current_queued_frames, targetFrames, rawError * 100.0f, s_smoothedError * 100.0f, deltaFrequencyRatio);
		}
		else
		{
			SDL_AudioStream* pAudioStream = AudioDevice::GetAudioStream();
			if (SDL_GetAudioStreamFrequencyRatio(pAudioStream) != 1.0f)
			{
				s_currentAudioResamplingFrequencyRatio = 1.0f;
				SDL_SetAudioStreamFrequencyRatio(AudioDevice::GetAudioStream(), 1.0f);
			}
		}
	}
}

bool Host::Init(SDL_Window* pWindow, bool initAudio, const char* biosPath)
{
	s_pWindow = pWindow;

	if (!Renderer::Init(pWindow))
	{
		LOG_ERROR("Failed to initialise renderer\n");
		return false;
	}

	s_audioEnabled = false;
	if (initAudio)
	{
		if (!initHostAudio())
		{
			LOG_ERROR("Failed to initialise host audio\n");
			return false;
		}
		s_audioEnabled = true;
	}
	else

	s_bus.Reset();

	if (!biosPath || !biosPath[0])
		biosPath = "bios/SCPH1001.bin";

	if (!s_bus.GetBIOS().Load(biosPath))
	{
		LOG_ERROR("Failed to load BIOS from file: %s\n", biosPath);
		return false;
	}

	s_pDisplayTexture = new Texture(GPU::kMaxDisplayWidthPixels, GPU::kMaxDisplayHeightPixels, "Display");
	s_pVramTexture = new Texture(kVRAMTextureWidthPixels, kVRAMHeightLines, "VRAM");

	s_bus.GetSPU().SetAudioFrameCallback(audioFrameCallback);

	s_pCD = new CD();

	return true;
}

void Host::Shutdown()
{
	delete s_pCD;
	s_pCD = nullptr;

	delete s_pDisplayTexture;
	s_pDisplayTexture = nullptr;

	delete s_pVramTexture;
	s_pVramTexture = nullptr;

	delete s_pEmulatorAudioBuffer;
	s_pEmulatorAudioBuffer = nullptr;

	AudioDevice::Shutdown();

	Renderer::Shutdown();

	s_pWindow = nullptr;
}

void Host::ResetEmulator()
{
	// If there is a disc inserted, then want it to still be inserted after reset.
	bool discInserted = s_bus.GetCDROM().IsDiscInserted();

	s_bus.Reset();

	if (discInserted)
		s_bus.GetCDROM().InsertDisc(*s_pCD);

	memset(s_displayImageData, 0, sizeof(s_displayImageData));
	memset(s_vramImageData, 0, sizeof(s_vramImageData));

	if (s_pEmulatorAudioBuffer)
		s_pEmulatorAudioBuffer->Reset();

	// Unpause
	s_paused = false;

	s_ttyLogger.Reset();

}

static void updateDisplayTexture()
{
	const GPU& gpu = s_bus.GetGPU();
	const u8* vram = gpu.GetVRAM();

	unsigned int displayWidth = gpu.GetHorizontalResolution();

	// The vertical display range controls which scanline the first display line is taken from, and how many lines are displayed.
	// The number of lines displayed is Y2 - Y1
	// Silent hill uses this to display only 224 lines when the vertical resolution is set to 240
	unsigned int displayRangeY1 = gpu.GetDisplayRangeY1();
	unsigned int displayRangeY2 = gpu.GetDisplayRangeY2();
	HP_ASSERT(displayRangeY2 >= displayRangeY1);
	s_displayHeight = displayRangeY2 - displayRangeY1;
	unsigned int verticalResolution = gpu.GetVerticalResolution();
	if (verticalResolution == 480) // interlace?
		s_displayHeight *= 2;

	// limit to dst image height to avoid writing out of bounds of dst image
	HP_ASSERT(s_pDisplayTexture->GetHeight() >= s_displayHeight);

	// #TODO [#opt]: Don't clear whole display, just unused parts
	memset(s_displayImageData, 0, sizeof(s_displayImageData));

	if (displayWidth > 0 && s_displayHeight > 0)
	{
		Rect srcRect;
		srcRect.x = gpu.GetDisplayStartX();
		srcRect.y = gpu.GetDisplayStartY();
		srcRect.w = displayWidth;
		srcRect.h = s_displayHeight;

		Rect dstRect;
		dstRect.x = 0;
		dstRect.y = /*displayRangeY1*/0; // Render to top of texture. displayRangeY1 will be honoured when rendering the texture to the window/screen.
		dstRect.w = displayWidth;
		dstRect.h = s_displayHeight;

		VRAMConvert::ConvertToR8G8B8A8_UNORM(
			vram, srcRect, gpu.GetDisplayFormat(),
			s_displayImageData, dstRect, s_pDisplayTexture->GetWidth(), s_pDisplayTexture->GetHeight());
	}

	// #TODO: Is it a race condition to call Texture::CopyImageDataToTransferBuffer and copy data into the transfer buffer while GPU is running? Should this be called within the render phase?
	s_pDisplayTexture->CopyImageDataToTransferBuffer(s_displayImageData);
}

static void updateVramTexture()
{
	const GPU& gpu = s_bus.GetGPU();
	const u8* vram = gpu.GetVRAM();

	Rect srcRect{
		/*.x =*/ 0,
		/*.y =*/ 0,
		/*.w =*/ kVRAMWidth16bpp,
		/*.h =*/ kVRAMHeightLines
	};

	Rect dstRect{
		/*.x =*/ 0,
		/*.y =*/ 0,
		/*.w =*/ kVRAMTextureWidthPixels,
		/*.h =*/ kVRAMHeightLines
	};

	VRAMConvert::ConvertToR8G8B8A8_UNORM(
		vram, srcRect, DisplayFormat::A1B5G5R5,
		s_vramImageData, dstRect, kVRAMTextureWidthPixels, kVRAMHeightLines);

	// #TODO: Is it a race condition to call Texture::CopyImageDataToTransferBuffer and copy data into the transfer buffer while GPU is running? Should this be called within the render phase?
	s_pVramTexture->CopyImageDataToTransferBuffer(s_vramImageData);
}

static void generateTestTone(double deltaTimeSeconds)
{
	// #TEST: Play 440 Hz square wave
	float frequency = 440.0f; // periods per second
	unsigned int samplesPerPeriod = (unsigned int)(AudioDevice::GetSampleRate() / frequency); // (samples / second) / (period / second) = samples / period
	static unsigned int s_frameCount = 0; // total audio frames generated

	unsigned int numFrames = (unsigned int)(deltaTimeSeconds * AudioDevice::GetSampleRate()); // total number of frames (all channels) to generate this update

	// Ensure we don't overflow the buffer
	unsigned int maxFrames = s_pEmulatorAudioBuffer->GetCapacity() / kAudioChannelCount;
	if (numFrames > maxFrames)
		numFrames = maxFrames;

	s_pEmulatorAudioBuffer->Reset();
	for (unsigned int i = 0; i < numFrames; i++)
	{
		// Stereo sound, so writing to both channels
		int16_t val = (s_frameCount % samplesPerPeriod) < (samplesPerPeriod / 2) ? INT16_MIN : INT16_MAX;
		s_pEmulatorAudioBuffer->WriteSample(val); // L
		s_pEmulatorAudioBuffer->WriteSample(val); // R
		s_frameCount++;
	}

	unsigned int lengthBytes = numFrames * sizeof(int16_t) * kAudioChannelCount; // total bytes (all channels)
	AudioDevice::PutAudioStreamData(s_pEmulatorAudioBuffer->GetBuffer(), lengthBytes);
	s_pEmulatorAudioBuffer->Reset();
}

static void updateAudio(double displayRefreshPeriodSeconds, double frameDeltaTimeSeconds)
{
	if (Host::s_playTestTone)
	{
		generateTestTone(frameDeltaTimeSeconds);
		return;
	}

	unsigned int lengthBytes = 0;
	// Convert audio from emulator buffer to device buffer
	HP_ASSERT(s_pEmulatorAudioBuffer);
	unsigned int numSamples = s_pEmulatorAudioBuffer->GetNumSamples(); // total samples (all channels)

	static bool s_audioSpam = false;
	if (s_audioSpam)
	{
		unsigned int numFrames = numSamples / kAudioChannelCount; // convert samples to frames
		unsigned int numExpectedFrames = (unsigned int)(displayRefreshPeriodSeconds * AudioDevice::GetSampleRate()); // total number of frames to generate this update
		LOG_INFO("Host::UpdateAudio: deltaTimeSeconds %.4f, numExpectedFrames %u, numFrames %u\n", displayRefreshPeriodSeconds, numExpectedFrames, numFrames);
	}

	lengthBytes = numSamples * sizeof(int16_t); // total bytes (all channels)
	AudioDevice::PutAudioStreamData(s_pEmulatorAudioBuffer->GetBuffer(), lengthBytes);
	s_pEmulatorAudioBuffer->Reset();
}

static void updateWindowTitle()
{
	// HopStation | <debug/release> | Game name | xx FPS e.g. "HopStation | debug | Final Fantasy VII | 60 FPS"
	char title[256]{};
#ifdef DEBUG
	const char* buildType = "debug";
#elif defined RELEASE 
	const char* buildType = "release";
#elif defined PROFILE 
	const char* buildType = "profile";
#else
#error "Unknown build type"
#endif

	const CDROM& cdrom = s_bus.GetCDROM();
	const char* gameName = cdrom.IsDiscInserted() ? cdrom.GetCD()->GetName() : "No disc";

	ImGuiIO& io = ImGui::GetIO();
	float frameTimeMs = 1000.0f / io.Framerate;

	SafeSnprintf(title, sizeof(title), "HopStation | %s | %s | %.3f ms/frame | %.1f FPS", buildType, gameName, frameTimeMs, io.Framerate);

	SDL_SetWindowTitle(s_pWindow, title);
}

void Host::Update(double displayRefreshPeriodSeconds, double frameDeltaTimeSeconds)
{
	if (s_paused)
		return;

	// Always step the emulator exactly the number of cycles that should run in a host frame, assuming the host is not framing out.
	// Passing actual frameTimeSeconds would compound frame-outs.
	unsigned int numCycles = (unsigned int)(displayRefreshPeriodSeconds * (double)kCpuClock);
	const u64 targetCycleCount = s_bus.GetCycleCount() + numCycles;
	const R3000& r3000 = s_bus.GetCPU();
	while (s_bus.GetCycleCount() < targetCycleCount)
	{
#if DEBUGGER_ENABLED
		u32 pc = r3000.GetPC();

		if (s_breakpointAddress && pc == s_breakpointAddress)
			s_breakpointAddress = s_breakpointAddress; // nop for VS breakpoint

		if (s_logDisassembly)
		{
			// Disassemble instruction
			u32 opcode = s_bus.ReadWord(pc);
			char disasmBuffer[64]{};
			if (R3000Dasm::Disassemble(opcode, pc, disasmBuffer, sizeof(disasmBuffer)) == 0)
			{
				HP_FATAL_ERROR("Failed to disassemble opcode: 0x%08X at address 0x%08X\n", opcode, pc);
			}

			LOG_INFO("[DASM] %08X: %08X %s\n", pc, opcode, disasmBuffer);
		}
#endif
		s_bus.StepInstruction();
		s_ttyLogger.Update(r3000, /*callback*/nullptr);
	}

	if (s_audioEnabled)
		updateAudio(displayRefreshPeriodSeconds, frameDeltaTimeSeconds);

	if (s_drawDisplay)
		updateDisplayTexture();
	if (s_drawVRAM)
		updateVramTexture();

	updateWindowTitle();
}

void Host::Render(int y)
{
	if (!Renderer::Begin())
		return; // application probably minimised
	
	Renderer::BeginCopyPass();
	if (s_drawDisplay)
		Renderer::UploadTexture(s_pDisplayTexture);
	if (s_drawVRAM)
		Renderer::UploadTexture(s_pVramTexture);
	Renderer::EndCopyPass();

	Renderer::BeginRenderPass();
	int x = 0;
	if (s_drawDisplay)
	{
		const GPU& gpu = s_bus.GetGPU();

		if (s_drawOverscan)
			y += gpu.GetDisplayRangeY1();

		// Don't draw whole texture - just draw visible region i.e. respect GPU display state
		unsigned int w = gpu.GetHorizontalResolution();
		SDL_Rect srcRect{ 0, 0, (int)w, (int)s_displayHeight}; // the display texture is generated such that (0,0) is the first visible pixel

		unsigned int displayScaleX = s_displayScale;
		unsigned int displayScaleY = s_displayScale;
		// Screens can be a variety of sizes and can be low res or high res horizontally or vertically separately.
		// Doubling scale for "low res" dimensions to keep the display squareish.
		// #TODO: Account for aspect ratio.
		// #TODO: Allow lowres to be scaled to 1x or 2x, rather than always doubling if both dimensions are lowres.
		if (w < 512) // Possible values: 256, 320, 368, 512, 640, so double if < 512
			displayScaleX *= 2;
		unsigned int verticalResolution = gpu.GetVerticalResolution();
		if (verticalResolution == 240) // NTSC height is either 240 or 480
			displayScaleY *= 2;
		SDL_Rect dstRect{ x, y, (int)(displayScaleX * w), (int)(displayScaleY * s_displayHeight) };
		Renderer::DrawTexture(s_pDisplayTexture, &srcRect, &dstRect);

		y += dstRect.h;

		// leave a little gap between display and VRAM, otherwise it can be difficult to see where the display ends and VRAM begins.
		y += 16;
	}
	if (s_drawVRAM)
	{
		// Draw full texture unscaled
		SDL_Rect dstRect{ x, y, (int)s_pVramTexture->GetWidth(), (int)s_pVramTexture->GetHeight() };
		Renderer::DrawTexture(s_pVramTexture, /*pSrcRect*/nullptr, &dstRect);
	}
	Renderer::EndRenderPass();
	Renderer::End();
}

Bus& Host::GetBus()
{
	return s_bus;
}

CD& Host::GetCD()
{
	return *s_pCD;
}

TTYLogger& Host::GetTTYLogger()
{
	return s_ttyLogger;
}

bool Host::IsDynamicAudioResamplingEnabled()
{
	return s_dynamicAudioResamplingEnabled;
}

void Host::SetDynamicAudioResamplingEnabled(bool enabled)
{
	s_dynamicAudioResamplingEnabled = enabled;
}

float Host::GetCurrentAudioResamplingFrequencyRatio()
{
	return s_currentAudioResamplingFrequencyRatio;
}

