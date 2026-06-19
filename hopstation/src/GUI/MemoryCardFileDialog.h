#pragma once

#include "core/ClassHelpers.h"

typedef struct SDL_Window SDL_Window;

class MemoryCardFileDialog
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(MemoryCardFileDialog);

	// Call this every frame to process pending operation.
	static void Update();

	// portIndex = 0 for left port, 1 for right port
	static bool ShowOpenFileDialog(unsigned int portIndex, SDL_Window* pWindow);
	static bool ShowSaveFileDialog(unsigned int portIndex, SDL_Window* pWindow);
};
