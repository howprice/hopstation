#pragma once

#include "core/ClassHelpers.h"

#include <stdint.h>

typedef struct SDL_AudioStream SDL_AudioStream;

static const unsigned int kAudioChannelCount = 2; // stereo
static const unsigned int kAudioFrameSize = sizeof(int16_t) * kAudioChannelCount; // AUDIO_S16SYS stereo frame

class AudioDevice
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(AudioDevice);

	static bool Init(unsigned int requestedBufferSizeInSampleFrames, unsigned int sampleRate);

	static void Shutdown();

	static bool IsInitialised();

	static bool IsPaused();
	static void Pause();
	static void Resume();

	// Can return nullptr if device not initialised.
	static const char* GetDeviceName();

	static unsigned int GetSampleRate();
//	static SDL_AudioFormat GetFormat();
	static unsigned int GetChannelCount();

	static void PutAudioStreamData(const void* data, unsigned int lengthBytes);
	static unsigned int GetQueueSizeFrames();
	static void ClearAudioStream();

	static unsigned int GetBufferSizeFrames();
	static float GetLatencyMs();

	// Utility functions
	static float FramesToMs(unsigned int frames, unsigned int sampleRate);

	static SDL_AudioStream* GetAudioStream();
};
