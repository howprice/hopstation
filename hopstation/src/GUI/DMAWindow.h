#pragma once

#include "core/ClassHelpers.h"

class DMAC;

class DMAWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(DMAWindow);

	// Call this every frame
	static void Update(const DMAC& dmac);

	static bool s_visible;
};
