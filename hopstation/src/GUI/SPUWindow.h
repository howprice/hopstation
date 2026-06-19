#pragma once

#include "core/ClassHelpers.h"

class SPU;

class SPUWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(SPUWindow);

	// Call this every frame
	static void Update(SPU& spu);

	static bool s_visible;
};
