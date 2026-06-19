// Hosts the emulator "guest"
// Provides native platform features: rendering, audio, input

#pragma once

#include "core/ClassHelpers.h"

class Bus;
class CD;
class TTYLogger;

typedef struct SDL_Window SDL_Window;

class Host
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(Host);

	static bool Init(SDL_Window* pWindow, bool initAudio, const char* biosPath);
	static void Shutdown();

	// displayRefreshPeriodSeconds is fixed for display e.g. 1 / 60 = 0.0166 seconds
	// frameDeltaTimeSeconds may be larger than displayRefreshPeriodSeconds if application is framing out
	static void Update(double displayRefreshPeriodSeconds, double frameDeltaTimeSeconds);

	static void Render(int y);

	static void ResetEmulator();

	static Bus& GetBus();
	static CD& GetCD();
	static TTYLogger& GetTTYLogger();

	static bool IsDynamicAudioResamplingEnabled();
	static void SetDynamicAudioResamplingEnabled(bool enabled);
	static float GetCurrentAudioResamplingFrequencyRatio();

	static inline bool s_paused = false;
	static inline bool s_drawDisplay = true;
	static inline bool s_drawOverscan = true; // Offset display by vertical display range Y1
	static inline unsigned int s_displayScale = 1;
	static inline bool s_drawVRAM = true;
	static inline bool s_playTestTone = false;
};
