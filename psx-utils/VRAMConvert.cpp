#include "VRAMConvert.h"

#include "psx/GPU.h"

#include "core/hp_assert.h"

static uint8_t s_5to8[32];
static bool s_lutInitialised;

//
// param val 5-bit colour [0,31]
//
static uint8_t convert5to8(uint8_t val5)
{
	// Use full range to ensure that 0 -> 0 and 0x1f -> 0xff
	// #TODO: Consider replacing with plain val8 = val5 << 3  This would not use the full range, but may match PeterLemon/PSX (no$psx) output better)
	// #TODO: Consider replacing with val8 = (val5 << 3) | (value5bit >> 2)  Fast, approximate linear conversion by shifting and ORing the upper 3 bits into the space in the lower 3 bits. https://stackoverflow.com/q/2442576
	// #TODO [#opt]: Consider using precomputed 32 element LUT
	uint8_t val8 = (uint8_t)((float)val5 * (255.0f / 31.0f));
	return val8;
}

static void buildLUT()
{
	for (unsigned int r5 = 0; r5 < 32; r5++)
	{
		s_5to8[r5] = convert5to8((uint8_t)r5);
	}
}

void VRAMConvert::ConvertToR8G8B8A8_UNORM(
	const uint8_t* pVram, const Rect& srcRect,	DisplayFormat format,
	uint32_t* imageData, const Rect& dstRect, unsigned int imageWidth, unsigned int imageHeight)
{
	HP_ASSERT(srcRect.w > 0 && srcRect.h > 0);
	HP_ASSERT(dstRect.w == srcRect.w && srcRect.h == dstRect.h, "Expect a 1:1 mapping, with no scaling");
	HP_ASSERT(srcRect.y + srcRect.h <= (int)kVRAMHeightLines);
	HP_ASSERT(dstRect.x + dstRect.w <= (int)imageWidth);
	HP_ASSERT(dstRect.y + dstRect.h <= (int)imageHeight);
	HP_UNUSED(imageHeight);

	if (!s_lutInitialised)
	{
		buildLUT();
		s_lutInitialised = true;
	}

	switch (format)
	{
		case DisplayFormat::A1B5G5R5: // 16 bpp
			HP_ASSERT(srcRect.x + srcRect.w <= (int)kVRAMWidth16bpp);
			break;
		case DisplayFormat::B8G8R8: // 24 bpp
			HP_ASSERT((srcRect.x * 2) + (srcRect.w * 3) <= (int)kVRAMWidthBytes); // GP1(05h) start X addresses VRAM in halfwords
			break;
	}

	switch (format)
	{
		case DisplayFormat::A1B5G5R5: // 16 bpp
		{
			constexpr u32 kSrcBytesPerPixel = 2; // 16 bpp = 2 bytes per pixel
			u32 srcRowAddr = (kVRAMWidthBytes * srcRect.y) + (2 * srcRect.x); // GP1(05h) start X addresses VRAM in halfwords
			u32* pDstRow = imageData + (dstRect.y * imageWidth) + dstRect.x; // 4 bytes per pixel
			for (int iy = 0; iy < srcRect.h; iy++)
			{
				u32 srcPixelAddr = srcRowAddr;
				u32* pDstPixel = pDstRow;

				for (int ix = 0; ix < srcRect.w; ix++)
				{
					// Interpret framebuffer as A1B5G5R5 and convert to 8-bit components for output.
					// Each 16-bit pixel is stored as 2 bytes in BGR555 format.
					u16 pixel = *(uint16_t*)(pVram + srcPixelAddr); // assumes host is little-endian

					u8 r = s_5to8[(pixel >> 0) & 0x1f];
					u8 g = s_5to8[(pixel >> 5) & 0x1f];
					u8 b = s_5to8[(pixel >> 10) & 0x1f];
					constexpr u8 a = 0xff;
					*pDstPixel++ = (a << 24) | (b << 16) | (g << 8) | r;

					srcPixelAddr += kSrcBytesPerPixel;
				}

				srcRowAddr += kVRAMWidthBytes;
				pDstRow += imageWidth; // 4 bytes per pixel
			}
			break;
		}
		case DisplayFormat::B8G8R8: // 24 bpp
		{
			constexpr u32 kSrcBytesPerPixel = 3; // 24 bpp = 3 bytes per pixel, tightly packed
			u32 srcRowAddr = (kVRAMWidthBytes * srcRect.y) + (2 * srcRect.x); // GP1(05h) start X addresses VRAM in halfwords
			u32* pDstRow = imageData + (dstRect.y * imageWidth) + dstRect.x; // 4 bytes per pixel
			for (int iy = 0; iy < srcRect.h; iy++)
			{
				u32 srcPixelAddr = srcRowAddr;

				u32* pDstPixel = pDstRow;

				for (int ix = 0; ix < srcRect.w; ix++)
				{
					// Interpret framebuffer as B8G8R8 and convert to 8-bit components for output.
					// It seems like it's actually stored in RGB order in memory!
					u8 r = pVram[srcPixelAddr + 0];
					u8 g = pVram[srcPixelAddr + 1];
					u8 b = pVram[srcPixelAddr + 2];
					constexpr u8 a = 0xff;
					*pDstPixel++ = (a << 24) | (b << 16) | (g << 8) | r;

					srcPixelAddr += kSrcBytesPerPixel;
				}

				srcRowAddr += kVRAMWidthBytes;
				pDstRow += imageWidth; // 4 bytes per pixel
			}
			break;
		}
	}
}
