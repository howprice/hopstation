// Convert VRAM into host texture formats for output

#pragma once

#include "core/ClassHelpers.h"

#include <stdint.h>

enum class DisplayFormat : uint32_t;

struct Rect
{
	int x;
	int y;
	int w;
	int h;
};

class VRAMConvert
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(VRAMConvert);

	static void ConvertToR8G8B8A8_UNORM(
		const uint8_t* pVram, const Rect& srcRect, DisplayFormat format,
		uint32_t* imageData, const Rect& dstRect, unsigned int imageWidth, unsigned int imageHeight);
};
