#pragma once

#include "core/ClassHelpers.h"

class SIO;

class MemoryCardWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(MemoryCardWindow);

	// Call this every frame
	static void Update(SIO& sio);

	static bool s_visible;
};
