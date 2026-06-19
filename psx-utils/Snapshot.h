#pragma once

#include "core/ClassHelpers.h"

#include "core/Types.h"

enum class DisplayFormat : u32;

class Snapshot
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(Snapshot);

	static bool SaveVramRectAsPPM(const u8* pVram, const char* filename, unsigned int x, unsigned int y, unsigned int w, unsigned int h, DisplayFormat format);

};
