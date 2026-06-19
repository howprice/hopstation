#pragma once

#include "core/ClassHelpers.h"
#include "core/Types.h"

class GPU;
enum class DisplayFormat : u32;

typedef struct SDL_Window SDL_Window;

class SnapshotDialog
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(SnapshotDialog);

	// Call this every frame to process any pending saves.
	static void Update(const GPU& gpu);

	static bool ShowSaveFileDialog(SDL_Window* pWindow, unsigned int x, unsigned int y, unsigned int w, unsigned int h, DisplayFormat format);
};
