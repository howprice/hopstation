#include "AudioDevice.h"

#include "core/Log.h"
#include "core/MathsHelpers.h"
#include "core/StringHelpers.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED

#include <SDL3/SDL.h>

// #TODO: What is a suitable audio format for the PSX SPU? Look at specs
static const SDL_AudioFormat kAudioFormat = SDL_AUDIO_S16;

static SDL_AudioDeviceID s_audioDeviceID;
static SDL_AudioSpec s_deviceAudioSpec;
static int s_bufferSizeInSampleFrames;
static bool s_audioPaused;
static SDL_AudioStream* s_pAudioStream;

bool AudioDevice::Init(unsigned int requestedBufferSizeInSampleFrames, unsigned int sampleRate)
{
	HP_ASSERT(s_audioDeviceID == 0, "Already initialised");

#ifndef RELEASE
	// List all audio drivers built into SDL3.
	// This is a hardcoded value.
	// n.b. Don't list with index, because index != ID and can confuse the user.
	// This is really for Raspberry Pi in the future where I've had trouble.
	// #TODO: Might need to call SDL_AudioInit explicitly on Raspberry Pi to force a specific driver.
	int numDrivers = SDL_GetNumAudioDrivers();
	LOG_INFO("SDL audio drivers: %d\n", numDrivers);
	for (int i = 0; i < numDrivers; i++)
	{
		LOG_INFO("- %s\n", SDL_GetAudioDriver(i));
	}
#endif

	// Print current SDL audio driver
	LOG_INFO("Current SDL audio driver: %s\n", SDL_GetCurrentAudioDriver());

	// List available audio devices
	int num_devices;
	SDL_AudioDeviceID *pDeviceIDs = SDL_GetAudioPlaybackDevices(&num_devices);
	if (!pDeviceIDs)
	{
		LOG_ERROR("SDL_GetAudioPlaybackDevices failed: %s\n", SDL_GetError());
		return false;
	}

	LOG_INFO("SDL audio devices: %d\n", num_devices);
	for (int i = 0; i < num_devices; i++)
	{
		SDL_AudioDeviceID instance_id = pDeviceIDs[i];
		LOG_INFO("AudioDevice ID %u: %s\n", instance_id, SDL_GetAudioDeviceName(instance_id));
	}
	SDL_free(pDeviceIDs);
	pDeviceIDs = nullptr;

	int bufferSizeInSampleFrames;
	if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &s_deviceAudioSpec, &bufferSizeInSampleFrames))
	{
		LOG_INFO("SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) failed: %s\n", SDL_GetError());
		return false;
	}

	LOG_INFO("SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK  Format: %s  Channels: %d  Frequency: %d Hz  Buffer size: %d sample frames, %.2f ms\n",
		SDL_GetAudioFormatName(s_deviceAudioSpec.format),
		s_deviceAudioSpec.channels,
		s_deviceAudioSpec.freq,
		bufferSizeInSampleFrames,
		FramesToMs(bufferSizeInSampleFrames, s_deviceAudioSpec.freq));

	// Construct desired audio spec
	// SDL3 SDL_AudioSpec only holds format, channel, and sample rate. SDL3 now manages the removed samples field.
	// Apps that want more control over device latency and throughput can force a newly-opened device's sample count with the
	// SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES hint, but most apps should not risk messing with the defaults.
	HP_UNUSED(requestedBufferSizeInSampleFrames); // #TODO: Use SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES to request a specific buffer size.

	SDL_AudioSpec audioSpec;
	SDL_zero(audioSpec);
	audioSpec.freq = sampleRate;
	audioSpec.format = kAudioFormat;
	audioSpec.channels = kAudioChannelCount;

	s_audioDeviceID = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audioSpec);
	if (s_audioDeviceID == 0)
	{
		LOG_ERROR("SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		return false;
	}

	LOG_INFO("\nOpened audio device ID %u \"%s\"\n", s_audioDeviceID, SDL_GetAudioDeviceName(s_audioDeviceID)); // prepend newline due to audio DLL spam

	if (!SDL_GetAudioDeviceFormat(s_audioDeviceID, &s_deviceAudioSpec, &s_bufferSizeInSampleFrames))
	{
		LOG_INFO("SDL_GetAudioDeviceFormat(%u) failed: %s\n", s_audioDeviceID, SDL_GetError());
		return false;
	}

//	unsigned int sampleSizeBits = SDL_AUDIO_BITSIZE(s_audioSpec.format);

	LOG_INFO("Format: %s  Channels: %d  Frequency: %d Hz  Buffer size: %u sample frames, %.2f ms\n",
		SDL_GetAudioFormatName(s_deviceAudioSpec.format),
		s_deviceAudioSpec.channels,
		s_deviceAudioSpec.freq,
		s_bufferSizeInSampleFrames,
		FramesToMs(s_bufferSizeInSampleFrames, s_deviceAudioSpec.freq));

	// In SDL3, Audio devices, opened by SDL_OpenAudioDevice(), no longer start in a paused state, as they don't begin processing audio until a stream is bound.
	s_audioPaused = false;

	// The emulator generates audio in signed 16-bit stereo format.
	// #TODO: Expose this if required.
	SDL_AudioSpec src_spec;
	SDL_zero(src_spec);
	src_spec.freq = s_deviceAudioSpec.freq;
	src_spec.format = kAudioFormat;
	src_spec.channels = kAudioChannelCount;

	s_pAudioStream = SDL_CreateAudioStream(/*src_spec*/&src_spec, /*dst_spec*/&s_deviceAudioSpec);
	if (!s_pAudioStream)
	{
		LOG_ERROR("SDL_CreateAudioStream() failed: %s\n", SDL_GetError());
		return false;
	}

	if (!SDL_BindAudioStream(s_audioDeviceID, s_pAudioStream))
	{
		LOG_ERROR("SDL_BindAudioStream() failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

void AudioDevice::Shutdown()
{
	if (s_pAudioStream)
	{
		SDL_UnbindAudioStream(s_pAudioStream);
		s_pAudioStream = nullptr;
	}

	if (s_audioDeviceID != 0)
	{
		SDL_CloseAudioDevice(s_audioDeviceID);
		s_audioDeviceID = 0;
	}
}

bool AudioDevice::IsInitialised()
{
	return s_audioDeviceID != 0;
}

bool AudioDevice::IsPaused()
{
	return s_audioPaused;
}

void AudioDevice::Pause()
{
	SDL_PauseAudioDevice(s_audioDeviceID);
	s_audioPaused = true;
}

void AudioDevice::Resume()
{
	SDL_ResumeAudioDevice(s_audioDeviceID);
	s_audioPaused = false;
}

const char* AudioDevice::GetDeviceName()
{
	return SDL_GetAudioDeviceName(s_audioDeviceID);
}

unsigned int AudioDevice::GetSampleRate()
{
	return s_deviceAudioSpec.freq;
}

unsigned int AudioDevice::GetChannelCount()
{
	return s_deviceAudioSpec.channels;
}

void AudioDevice::PutAudioStreamData(const void* data, unsigned int lengthBytes)
{
	// #TODO: Is there anyway to monitor the queue size?

	if (!SDL_PutAudioStreamData(s_pAudioStream, data, lengthBytes))
	{
		LOG_ERROR("SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
	}

	// #TODO: Call SDL_FlushAudioStream immediately to reduce latency?

//	LOG_INFO("SDL_GetQueuedAudioSize %u bytes (after)\n", SDL_GetQueuedAudioSize(s_audioDeviceID));
}

unsigned int AudioDevice::GetQueueSizeFrames()
{
	unsigned int queueSizeBytes = SDL_GetAudioStreamQueued(s_pAudioStream);
	unsigned int queueSizeFrames = queueSizeBytes / kAudioFrameSize;
	return queueSizeFrames;
}

void AudioDevice::ClearAudioStream()
{
	SDL_ClearAudioStream(s_pAudioStream);
}

unsigned int AudioDevice::GetBufferSizeFrames()
{
	return s_bufferSizeInSampleFrames;
}

float AudioDevice::FramesToMs(unsigned int frames, unsigned int sampleRate)
{
	// frames / frames per second = seconds
	return (frames * 1000.0f) / sampleRate;
}

SDL_AudioStream* AudioDevice::GetAudioStream()
{
	return s_pAudioStream;
}

float AudioDevice::GetLatencyMs()
{
	return FramesToMs(s_bufferSizeInSampleFrames, s_deviceAudioSpec.freq);
}
