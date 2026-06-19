#pragma once

#include "core/ClassHelpers.h"

class CDROM;

class CDROMWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(CDROMWindow);

	// Call this every frame
	static void Update(const CDROM& cdrom);

	static bool s_visible;
};
