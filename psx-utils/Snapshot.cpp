#include "Snapshot.h"

#include "psx/GPU.h"

#include "core/hp_assert.h"
#include "core/Log.h"

#include <stdio.h>

//
// param val 5-bit colour [0,31]
//
static u8 convert5to8(u16 val5)
{
	// Use full range to ensure that 0 -> 0 and 0x1f -> 0xff
	// #TODO: Consider replacing with plain val8 = val5 << 3  This would not use the full range, but may match PeterLemon/PSX (no$psx) output better)
	// #TODO: Consider replacing with val8 = (val5 << 3) | (value5bit >> 2)  Fast, approximate linear conversion by shifting and ORing the upper 3 bits into the space in the lower 3 bits. https://stackoverflow.com/q/2442576
	// #TODO [#opt]: Consider using precomputed 32 element LUT
	u8 val8 = (u8)((float)val5 * (255.0f / 31.0f));
	return val8;
}

bool Snapshot::SaveVramRectAsPPM(const u8* pVram, const char* filename, unsigned int x, unsigned int y, unsigned int w, unsigned int h, DisplayFormat format)
{
	HP_ASSERT(w > 0 && h > 0);
	HP_ASSERT(y + h <= kVRAMHeightLines);

	switch (format)
	{
		case DisplayFormat::A1B5G5R5: // 16 bpp
			HP_ASSERT(x + w <= kVRAMWidth16bpp);
			break;
		case DisplayFormat::B8G8R8: // 24 bpp
			HP_ASSERT(x + w <= kVRAMWidthBytes / 3);
			break;
	}

	// Save in really simple PPM file format. https://rosettacode.org/wiki/Bitmap/Write_a_PPM_file#C
	FILE* fp = fopen(filename, "wb"); /* b - binary mode */
	if (!fp)
	{
		LOG_ERROR("Failed to open file for write: %s\n", filename);
		return false;
	}

	fprintf(fp, "P6\n%d %d\n255\n", w, h); // P6 = binary; max colour value 255

	switch (format)
	{
		case DisplayFormat::A1B5G5R5: // 16 bpp
		{
			constexpr u32 kBytesPerPixel = 2; // 16 bpp = 2 bytes per pixel
			u32 rowAddr = (kVRAMWidthBytes * y) + (kBytesPerPixel * x);
			for (unsigned int iy = 0; iy < h; iy++)
			{
				u32 pixelAddr = rowAddr;

				for (unsigned int ix = 0; ix < w; ix++)
				{
					// Interpret framebuffer as A1B5G5R5 and convert to 8-bit components for output.
					// Each 16-bit pixel is stored as 2 bytes in BGR555 format.
					u16 pixel = pVram[pixelAddr + 0] | ((u16)pVram[pixelAddr + 1] << 8); // little-endian

					u8 rgb[3];
					rgb[0] = convert5to8((pixel >> 0) & 0x1f);
					rgb[1] = convert5to8((pixel >> 5) & 0x1f);
					rgb[2] = convert5to8((pixel >> 10) & 0x1f);
					fwrite(rgb, 1, 3, fp);

					pixelAddr += kBytesPerPixel;
				}

				rowAddr += kVRAMWidthBytes;
			}
			break;
		}
		case DisplayFormat::B8G8R8: // 24 bpp
		{
			constexpr u32 kBytesPerPixel = 3; // 24 bpp = 3 bytes per pixel, tightly packed
			u32 rowAddr = (kVRAMWidthBytes * y) + (kBytesPerPixel * x);
			for (unsigned int iy = 0; iy < h; iy++)
			{
				u32 pixelAddr = rowAddr;

				for (unsigned int ix = 0; ix < w; ix++)
				{
					// Interpret framebuffer as B8G8R8 and convert to 8-bit components for output.
					// It seems like it's actually stored in RGB order in memory!
					u8 r = pVram[pixelAddr + 0];
					u8 g = pVram[pixelAddr + 1];
					u8 b = pVram[pixelAddr + 2];

					u8 rgb[3]{ r, g, b };
					fwrite(rgb, 1, 3, fp);

					pixelAddr += kBytesPerPixel;
				}

				rowAddr += kVRAMWidthBytes;
			}
			break;
		}
	}

	fclose(fp);

	LOG_INFO("Saved VRAM to %s (x,y,w,h,format)=(%u,%u,%u,%u,%s)\n", filename, x, y, w, h, kDisplayFormatNames[(unsigned int)format]);
	return true;
}

