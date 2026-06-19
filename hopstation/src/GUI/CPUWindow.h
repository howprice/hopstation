#pragma once

#include "core/ClassHelpers.h"

class R3000;

class CPUWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(CPUWindow);

	// Call this every frame
	static void Update(R3000& r3000);

	static bool s_visible;
};
