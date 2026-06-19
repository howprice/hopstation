#pragma once

#include "core/ClassHelpers.h"

class HostWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(HostWindow);

	// Call this every frame
	static void Update();

	static bool s_visible;
};
