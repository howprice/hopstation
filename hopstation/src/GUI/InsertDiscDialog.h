#pragma once

#include "core/ClassHelpers.h"

typedef struct SDL_Window SDL_Window;

class InsertDiscDialog
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(InsertDiscDialog);

	// Call this every frame to process pending operation.
	static void Update();

	static bool ShowOpenFileDialog(SDL_Window* pWindow);
};
