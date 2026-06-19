#pragma once

#include "core/ClassHelpers.h"

class GPU;

class GPUWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(GPUWindow);

	// Call this every frame
	static void Update(GPU& gpu);

	static bool s_visible;
};
