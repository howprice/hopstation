#pragma once

#include "core/ClassHelpers.h"

class CD;

class CDWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(CDWindow);

	// Call this every frame
	static void Update(const CD* pCD);

	static bool s_visible;
};
