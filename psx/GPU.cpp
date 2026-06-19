#include "GPU.h"

#include "core/MathsHelpers.h" // SignExtend
#include "core/Log.h"
#include "core/ArrayHelpers.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED, ENUM_COUNT

#include <string.h> // memset

//---------------------------------------------------------------------------------------------------------------

static const char* kDMADirectionNames[] =
{
	"Off",
	"FIFO",
	"CPUToGP0",
	"GPUREADtoCPU"
};
static_assert(COUNTOF_ARRAY(kDMADirectionNames) == ENUM_COUNT(GPU::DMADirection));

static const char* kTextureFormatNames[] =
{
	"4-bit palette (4 bpp)",
	"8-bit palette (8 bpp)",
	"A1B5G5R5 (16 bpp)",
	"Reserved"
};
static_assert(COUNTOF_ARRAY(kTextureFormatNames) == ENUM_COUNT(TextureFormat));

static const char* kRectangleSizeNames[] =
{
	"Variable size",
	"1x1",
	"8x8",
	"16x16",
};
static_assert(COUNTOF_ARRAY(kRectangleSizeNames) == ENUM_COUNT(GPU::RectangleSize));

static const char* kShadingModeNames[] =
{
	"Flat",
	"Gouraud"
};
static_assert(COUNTOF_ARRAY(kShadingModeNames) == ENUM_COUNT(GPU::ShadingMode));

//---------------------------------------------------------------------------------------------------------------
// Helpers
//

static inline void unpackB8G8R8(u32 val, u8& r, u8& g, u8& b)
{
	r = (u8)(val & 0xff);
	g = (u8)((val >> 8) & 0xff);
	b = (u8)((val >> 16) & 0xff);
}

static inline u32 packB8G8R8(u8 r, u8 g, u8 b)
{
	return ((u32)b << 16) | ((u32)g << 8) | r;
}

static inline void unpackB8G8R8to5bit(u32 colourB8G8R8, u8& r5, u8& g5, u8& b5)
{
	r5 = (u8)((colourB8G8R8 >> 3) & 0x1f);
	g5 = (u8)((colourB8G8R8 >> (8 + 3)) & 0x1f);
	b5 = (u8)((colourB8G8R8 >> (16 + 3)) & 0x1f);
}

static inline u16 convertB8G8R8toB5G5R5(u32 colourB8G8R8)
{
	unsigned int r5 = (colourB8G8R8 >> 3) & 0x1f;
	unsigned int g5 = (colourB8G8R8 >> (8 + 3)) & 0x1f;
	unsigned int b5 = (colourB8G8R8 >> (16 + 3)) & 0x1f;
	return (u16)((b5 << 10) | (g5 << 5) | (r5 << 0));
}

static inline u8 convert5bitTo8bit(u8 value5bit)
{
	// Fast, approximate linear conversion by shifting and ORing the upper 3 bits into the space in the lower 3 bits.
	// https://stackoverflow.com/q/2442576
	return (value5bit << 3) | (value5bit >> 2);
}

static inline void unpackA1B5G5R5(u16 colorA1B5G5R5, u8& outR5, u8& outG5, u8& outB5, u8& outA1)
{
	outR5 = colorA1B5G5R5 & 0x1f;
	outG5 = (colorA1B5G5R5 >> 5) & 0x1f;
	outB5 = (colorA1B5G5R5 >> 10) & 0x1f;
	outA1 = (colorA1B5G5R5 >> 15) & 0x01;
}

static inline u16 packA1B5G5R5(u8 r5, u8 g5, u8 b5, u8 a1)
{
	return (a1 << 15) | (b5 << 10) | (g5 << 5) | (r5 << 0);
}

[[maybe_unused]]
static inline void unpackA1B5G5R5to8bit(u16 colorA1B5G5R5, u8& outR8, u8& outG8, u8& outB8, u8& outA1)
{
	u8 r5, g5, b5, a1;
	unpackA1B5G5R5(colorA1B5G5R5, r5, g5, b5, a1);

	outR8 = convert5bitTo8bit(r5);
	outG8 = convert5bitTo8bit(g5);
	outB8 = convert5bitTo8bit(b5);
	outA1 = a1;
}

//---------------------------------------------------------------------------------------------------------------

static unsigned int decodeHorizontalResolution(GPU::StatusRegister gpustat)
{
	// GPUSTAT 
	//  0-1   Horizontal Resolution 1     (0=256, 1=320, 2=512, 3=640) ;GPUSTAT.17-18
	//  ...
	//  6     Horizontal Resolution 2     (0=256/320/512/640, 1=368)   ;GPUSTAT.16
	//  ...
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp108h-display-mode

	if (gpustat.horizontalResolution2)
		return 368;

	static constexpr unsigned int kHorizontalResolutions[] = { 256, 320, 512, 640 };
	return kHorizontalResolutions[gpustat.horizontalResolution1];
}

//---------------------------------------------------------------------------------------------------------------

GPU::GPU()
{
	m_vram = new u8[kVRAMSizeBytes];
	memset(m_vram, 0, kVRAMSizeBytes);
}

GPU::~GPU()
{
	delete[] m_vram;
}

void GPU::Reset()
{
	m_gpuread = 0;
	m_gpustat.val = 0;
	memset(m_vram, 0, kVRAMSizeBytes);

	m_currentCommand = Command::None;
	m_commandWordCount = 0;
	m_fillRectangle = {};
	m_memoryTransfer = {};
	m_polygon = {};
	m_line = {};
	m_rectangle = {};

	m_drawAreaTopLeft = 0;
	m_drawAreaBottomRight = 0;
	m_scissorRect = {};

	m_drawingOffset = 0;
	m_drawingOffsetX = 0;
	m_drawingOffsetY = 0;

	m_clutX_halfwords = 0;
	m_clutY = 0;
	m_clutAddr = 0;
	m_texturePageAddr = 0;

	m_textureWindow = {};
	m_textureWindowPrecomp = {};

	m_rectangleTextureFlipX = false;
	m_rectangleTextureFlipY = false;

	m_maskSetting = {};

	m_displayStartX = 0;
	m_displayStartY = 0;

	m_displayRangeX1 = 0;
	m_displayRangeX2 = 0;

	m_displayRangeY1 = 0;
	m_displayRangeY2 = 0;

	m_stats = {};
}

u32 GPU::GetGPUREAD()
{
	switch (m_currentCommand)
	{
		case Command::None:
			break; // nothing to do
		case Command::FillRectangle:
			break; // nothing to do
		case Command::VramToVramBlit:
			break; // nothing to do
		case Command::CpuToVramBlit:
			break; // nothing to do
		case Command::VramToCpuBlit:
			m_gpuread = updateVramToCpuBlitOutput();
			break;
		case Command::RenderPolygon:
			break; // nothing to do
		case Command::RenderLine:
			break; // nothing to do
		case Command::RenderRectangle:
			break; // nothing to do
	}

	if (s_logGPUREAD)
		LOG_INFO("[GPUREAD] Read value %08X\n", m_gpuread);

	return m_gpuread;
}

// #TODO: Make this const when remove hack
u32 GPU::GetGPUSTAT()
{
	// #HACK: Alternate interlace field every read to allow BIOS to progress.
	// #TODO: Fix this (see GitHub issue #55)
	m_gpustat.drawingEvenOddLinesInInterlaceMode = 1 - m_gpustat.drawingEvenOddLinesInInterlaceMode; // #TEMP: flip each read

	StatusRegister gpustat = m_gpustat;

	gpustat.readyToSendVramToCpu = m_currentCommand == Command::VramToCpuBlit ? 1 : 0; // if VRAM to CPU blit command is active then this should be true

	if (gpustat.readyToSendVramToCpu)
	{
		// While CPU is still reading data from VRAM, don't think the GPU should be receiving new commands via GP0.
		gpustat.readyToReceiveDmaBlock = 0;
		gpustat.readyToReceiveCmdWord = 0;
	}
	else
	{
		// Otherwise always ready, because all other GPU commands complete immediately in current implmentation.
		gpustat.readyToReceiveDmaBlock = 1;
		gpustat.readyToReceiveCmdWord = 1;
	}

	if (s_logGPUSTAT)
		LOG_INFO("[GPUSTAT] Read value %08X\n", gpustat.val);

	return gpustat.val;
}

void GPU::WriteGP0(u32 val)
{
	switch (m_currentCommand)
	{
		case Command::None:
			parseGP0Command(val);
			break;

		case Command::FillRectangle:
			parseFillRectangleWord(val);
			break;

		case Command::VramToVramBlit:
			parseVramToVramBlitWord(val);
			break;

		case Command::CpuToVramBlit:
			parseCpuToVramBlitWord(val);
			break;

		case Command::VramToCpuBlit:
			parseVramToCpuBlitWord(val);
			break;

		case Command::RenderPolygon:
			parseDrawPolygonWord(val);
			break;

		case Command::RenderLine:
			parseDrawLineWord(val);
			break;

		case Command::RenderRectangle:
			parseDrawRectangleWord(val);
			break;
	}

	m_stats.GP0CommandCount++;
}

void GPU::WriteGP1(u32 val)
{
	// Extract command from upper 8 bits
	u8 command = (val >> 24);
	switch (command)
	{
		case 0x00: // GP1(00h) Reset GPU
			if (s_logGP1)
				LOG_INFO("[GP1] %08X Reset GPU\n", val);
			resetGPU();
			break;

		case 0x01: // GP1(01h) Reset GP0 command buffer
		{
			if (s_logGP1)
				LOG_INFO("[GP1] %08X Reset GP0 command buffer\n", val);

			resetCommandBuffer();
			break;
		}

		case 0x02: // GP1(02h) Acknowledge GPU IRQ
		{
			// Resets the IRQ flag in GPUSTAT.24. The flag can be set via GP0(1Fh).
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp102h-acknowledge-gpu-interrupt-irq1
			if (s_logGP1)
				LOG_INFO("[GP1] %08X Acknowledge GPU IRQ\n", val);

			m_gpustat.interruptRequestIRQ1 = 0;
			break;
		}

		case 0x03: // GP1(03h) Enable/disable display 
		{
			// Bit 0     Display On/Off   (0=On, 1=Off)                         ;GPUSTAT.23
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp103h-display-enable

			if (s_logGP1)
				LOG_INFO("[GP1] %08X %s display\n", val, (val & 1) ? "Disable" : "Enable");

			m_gpustat.displayDisable = val & 1;

			break;
		}

		case 0x04: // GP1(04h) Set GPU DMA direction (mainly just affects some DMA-related bits in GPUSTAT)
		{
			// Bits 1:0  DMA Direction (0=Off, 1=FIFO, 2=CPUtoGP0, 3=GPUREADtoCPU) ;GPUSTAT.29-30
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp104h-dma-direction-data-request
			u32 dir = val & 3;

			if (s_logGP1)
				LOG_INFO("[GP1] %08X Set GPU DMA direction %s\n", val, kDMADirectionNames[dir]);

			m_gpustat.dmaDirection = (DMADirection)dir;

			// Set GPUSTAT.25 (bit 25) appropriately
			//	DMA / Data Request, meaning depends on GP1(04h) DMA Direction:
	        //    When GP1(04h)=0 ---> Always zero (0)
	        //    When GP1(04h)=1 ---> FIFO State  (0=Full, 1=Not Full)
	        //    When GP1(04h)=2 ---> Same as GPUSTAT.28
	        //    When GP1(04h)=3 ---> Same as GPUSTAT.27
			switch (m_gpustat.dmaDirection)
			{
				case DMADirection::Off:
					m_gpustat.dmaDataRequest = 0;
					break;
				case DMADirection::FIFO:
					m_gpustat.dmaDataRequest = 1; // always say not full, because the FIFO is not currently emulated
					break;
				case DMADirection::CPUToGP0:
					// GPUSTAT.28 is readyToReceiveDmaBlock.
					// If a VRAM to CPU blit is in progress, then don't receive any more DMA commands until finished.
					// else set to true because all ther GPU commands are immediate and should always be ready to go.
					m_gpustat.dmaDataRequest = (m_currentCommand == Command::VramToCpuBlit) ? 0 : 1;
					break;
				case DMADirection::GPUREADtoCPU:
					m_gpustat.dmaDataRequest = (m_currentCommand == Command::VramToCpuBlit) ? 1 : 0; // GPUSTAT.27 is readyToSendVramToCpu so if this command is active then this should be true
					break;
			}

			break;
		}

		case 0x05: // GP1(05h) Set display area top-left coordinates
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp105h-start-of-display-area-in-vram

			// Bits [9:0] X [0,1023] halfword address in VRAM, relative to begin of VRAM
			m_displayStartX = val & 0x3ff;

			// Bits [18:10] Y [0,511] scanline number in VRAM, relative to begin of VRAM
			m_displayStartY = (val >> 10) & 0x1ff;

			if (s_logGP1)
				LOG_INFO("[GP1] %08X Set display area top-left coordinates (%u, %u)\n", val, m_displayStartX, m_displayStartY);
			break;
		}

		case 0x06: // GP1(06h) Set horizontal display range
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp106h-horizontal-display-range-on-screen

			// Bits 11:0   X1 (260h+0)
			// 260h (608) is the first visible pixel on normal TV Sets i.e. the end of the hblank
			m_displayRangeX1 = val & 0xfff;

			// Bits 23:12  X2 (260h+320*8)
			// 260h+320*8 = C60h = 3168
			m_displayRangeX2 = (val >> 12) & 0xfff;

			if (s_logGP1)
				LOG_INFO("[GP1] %08X Set horizontal display range (%u, %u) (X2 - X1 = %u)\n", val, m_displayRangeX1, m_displayRangeX2, m_displayRangeX2 - m_displayRangeX1);
			break;
		}

		case 0x07: // GP1(07h) Set vertical display range
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp107h-vertical-display-range-on-screen
			
			// Bits 9:0   Y1 (NTSC=88h-(240/2), (PAL=A3h-(288/2))
			m_displayRangeY1 = val & 0x3ff;

			// Bits 19:10 Y2 (NTSC=88h+(240/2), (PAL=A3h+(288/2))
			m_displayRangeY2 = (val >> 10) & 0x3ff;

			if (s_logGP1)
				LOG_INFO("[GP1] %08X Set vertical display range (%u, %u) (Y2 - Y1 = %u)\n", val, m_displayRangeY1, m_displayRangeY2, m_displayRangeY2 - m_displayRangeY1);
			break;
		}

		case 0x08: // GP1(08h) Set display mode (resolution, color depth, interlacing, NTSC vs. PAL)
		{
			// Bit    Meaning                      (values)                    ;GPUSTAT.bit
			//  0-1   Horizontal Resolution 1     (0=256, 1=320, 2=512, 3=640) ;GPUSTAT.17-18
			//  2     Vertical Resolution         (0=240, 1=480, when Bit5=1)  ;GPUSTAT.19
			//  3     Video Mode                  (0=NTSC/60Hz, 1=PAL/50Hz)    ;GPUSTAT.20
			//  4     Display Area Color Depth    (0=15bit, 1=24bit)           ;GPUSTAT.21
			//  5     Vertical Interlace          (0=Off, 1=On)                ;GPUSTAT.22
			//  6     Horizontal Resolution 2     (0=256/320/512/640, 1=368)   ;GPUSTAT.16
			//  7     Flip screen horizontally    (0=Off, 1=On, v1 only)       ;GPUSTAT.14
			//  8-23  Not used (zero)
			// 
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp108h-display-mode

			m_gpustat.horizontalResolution1 = val & 3; // GPUSTAT.17-18
			m_gpustat.verticalResolution = (val >> 2) & 1; // GPUSTAT.19
			m_gpustat.videoMode = (val >> 3) & 1; // GPUSTAT.20
			m_gpustat.displayFormat = (DisplayFormat)((val >> 4) & 1); // GPUSTAT.21
			m_gpustat.verticalInterlace = (val >> 5) & 1; // GPUSTAT.22
			m_gpustat.horizontalResolution2 = (val >> 6) & 1; // GPUSTAT.16
			m_gpustat.flipScreenHorizontally = (val >> 7) & 1; // GPUSTAT.14

			if (s_logGP1)
				LOG_INFO("[GP1] %08X Set display mode (%u x %u) %s %s %s ScreenFlipX %s\n",
					val,
					GetHorizontalResolution(),
					m_gpustat.verticalResolution == 0 ? 240 : 480,
					m_gpustat.videoMode == 0 ? "NTSC" : "PAL",
					kDisplayFormatNames[(int)m_gpustat.displayFormat],
					m_gpustat.verticalInterlace == 0 ? "Non-Interlaced" : "Interlaced",
					m_gpustat.flipScreenHorizontally == 0 ? "Off" : "On");

			// Optimisation: Extract fields now to avoid per-cycle recalculations.
			m_horizontalResolution = decodeHorizontalResolution(m_gpustat);
			break;
		}

		case 0x09: // GP1(09h) Set VRAM size (v2)
		{
			HP_DEBUG_FATAL_ERROR("GP1(09h) Set VRAM size (v2) not supported on PSX");
			break;
		}

		case 0x10: // GP1(10h) Read GPU register
		{
			parseGPUREAD(val);
			break;
		}

		default:
		{
			HP_FATAL_ERROR("[GP1] %08X Unexpected value\n", val);

			// #TODO: GP1(11h..1Fh) - Mirrors of GP1(10h), Read GPU internal register
			if (s_logGP1 || s_logUnimplementedGpuFeatures)
				LOG_ERROR("[GP1] %08X Unexpected value\n", val);
			break;
		}
	}

	m_stats.GP1CommandCount++;
}

unsigned int GPU::GetVerticalResolution() const
{
	// GPUSTAT
	//  ...
	//  2     Vertical Resolution         (0=240, 1=480, when Bit5=1)  ;GPUSTAT.19
	//  ...
	//  5     Vertical Interlace          (0=Off, 1=On)                ;GPUSTAT.22
	//  ...
	// 
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp108h-display-mode

	// Can ignore bit 22, which just switches to interlace mode for 480 to skip every other line every other frame/field i.e. 480i rather than 480p.
	return m_gpustat.verticalResolution == 0 ? 240 : 480;
}

void GPU::parseGP0Command(u32 val)
{
	// GP0 commands can be decoded from the upper 3 bits.
	switch (val >> 29)
	{
		case 0b000: // Misc
		{
			parseGP0MiscCommand(val);
			break;
		}

		case 0b001: // polygon primitive
		{
			m_commandWordCount = 0;
			m_currentCommand = Command::RenderPolygon;
			parseDrawPolygonWord(val);
			break;
		}

		case 0b010: // line primitive
		{
			m_commandWordCount = 0;
			m_currentCommand = Command::RenderLine;
			parseDrawLineWord(val);
			break;
		}

		case 0b011: // rectangle primitive
		{
			m_commandWordCount = 0;
			m_currentCommand = Command::RenderRectangle;
			parseDrawRectangleWord(val);
			break;
		}

		case 0b100: // VRAM to VRAM blit
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vram-to-vram-blitting-command-4-100

			if (s_logGP0)
				LOG_INFO("[GP0] %08X VRAM to VRAM blit command 4 (100)\n", val);

			m_currentCommand = Command::VramToVramBlit;
			m_commandWordCount = 1;

			// No further data encoded in the first command word

			break;
		}

		case 0b101: // CPU to VRAM  blit
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#cpu-to-vram-blitting-command-5-101

			if (s_logGP0)
				LOG_INFO("[GP0] %08X CPU to VRAM blit command 5 (101)\n", val);

			m_currentCommand = Command::CpuToVramBlit;
			m_commandWordCount = 1;

			// No further data encoded in the first command word

			break;
		}

		case 0b110: // VRAM to CPU blit
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vram-to-cpu-blitting-command-6-110
			if (s_logGP0)
				LOG_INFO("[GP0] %08X VRAM to CPU blit command 6 (110)\n", val);

			// No further data encoded in the first command word

			m_currentCommand = Command::VramToCpuBlit;
			m_commandWordCount = 1;

			break;
		}

		case 0b111: // GP0(Exh) environment command
		{
			parseGP0EnvironmentCommand(val);
			break;
		}
	}
}

void GPU::parseGP0MiscCommand(u32 val)
{
	// Decode command from upper 8 bits
	switch (val >> 24)
	{
		case 0x00: // GP0(00h) - NOP
		{
			// This command does not do anything or take any room in the command buffer FIFO.
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp000h-nop
			if (s_logGP0)
				LOG_INFO("[GP0] %08X NOP command\n", val);
			return;
		}

		case 0x01: // GP0(01h) - Clear cache
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#clear-cache
			if (s_logGP0 || s_logUnimplementedGpuFeatures)
				LOG_INFO("[GP0] %08X Clear cache NOT IMPLEMENTED\n", val);

			return;
		}

		case 0x02: // GP0(02h) - Fill VRAM rectangle
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#quick-rectangle-fill
			m_commandWordCount = 0;
			m_currentCommand = Command::FillRectangle;
			parseFillRectangleWord(val);
			return;
		}

		case 0x1F: // GP0(1Fh) - Set IRQ flag
		{
			// Sets the IRQ flag in GPUSTAT.24, which can trigger an interrupt if enabled via GP1(02h).
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp01fh-interrupt-request-irq1
			if (s_logGP0)
				LOG_INFO("[GP0] %08X Set IRQ flag\n", val);
			m_gpustat.interruptRequestIRQ1 = 1;
			break;
		}

		default:
		{
			// The rest of the commmands can just be ignored.
			// Silent Hill spams 030A8F1C and 080B5C4C, which just do nothing.
			if (s_logGP0 || s_logInvalidCommands)
				LOG_INFO("[GP0] Invalid misc (000) command %08X \n", val);
			break;
		}
	}
}

void GPU::parseGP0EnvironmentCommand(u32 val)
{
	HP_DEBUG_ASSERT((val >> 29) == 0b111, "Expected GP0 environment command");

	// Decode command from upper 8 bits
	u8 command = (val >> 24);
	switch (command)
	{
		case 0xE1: // GP0(E1h) - Draw mode setting (aka TEXPAGE)
		{
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e1h-draw-mode-setting-aka-texpage

			// Bits 3:0   Texture page X Base   (N*64) i.e. in 64-halfword steps)
			m_gpustat.texturePageXBase = val & 0xf; // GPUSTAT.0-3

			// Bit 4     Texture page Y Base 1 (N*256) (ie. 0, 256, 512 or 768)
			m_gpustat.texturePageYBase1 = (val >> 4) & 1; // GPUSTAT.4

			// Bits 6:5   Blend mode     (0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)
			m_gpustat.blendMode = (val >> 5) & 0x3; // GPUSTAT.5-6

			// Bits 8:7   Texture format   (0=4bit, 1=8bit, 2=15bit, 3=Reserved)
			u32 textureFormatIndex = (val >> 7) & 3;
			m_gpustat.textureFormat = (TextureFormat)textureFormatIndex; // GPUSTAT.7-8

			// Bit 9     Dither 24bit to 15bit (0=Off/strip LSBs, 1=Dither Enabled)
			m_gpustat.dither24to15bit = (val >> 9) & 1; // GPUSTAT.9

			// Bit 10    Drawing to display area (0=Prohibited, 1=Allowed)
			m_gpustat.drawingToDisplayArea = (val >> 10) & 1; // GPUSTAT.10

			// Bit 11    Texture page Y Base 2 (N*512) (only for 2 MB VRAM i.e. arcade, not PSX)
			m_gpustat.texturePageYBase2 = (val >> 11) & 1;
//			HP_DEBUG_ASSERT(m_gpustat.texturePageYBase2 == 0, "Expect this to be zero on PSX."); // Assert disabled because The Raiden Project sets this, presumably accidentally or for dev purposes

			// Bit 12 Textured Rectangle X-Flip
			m_rectangleTextureFlipX = (val >> 12) & 1;

			// Bit 13 Textured Rectangle Y-Flip
			m_rectangleTextureFlipY = (val >> 13) & 1;

			updateTexturePageAddress();

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E1h) Draw mode setting: texture page (%u,%u) addr=%08X, blend mode=%u, texture format=%s%s, drawing to display area=%s, rect texture flip (X,Y)=%s,%s\n",
					val,
					m_gpustat.texturePageXBase * 64,
					(m_gpustat.texturePageYBase1 + m_gpustat.texturePageYBase2 * 2) * 256,
					m_texturePageAddr,
					m_gpustat.blendMode,
					kTextureFormatNames[textureFormatIndex],
					m_gpustat.dither24to15bit ? ", dither" : "",
					m_gpustat.drawingToDisplayArea ? "Allowed" : "Prohibited",
					m_rectangleTextureFlipX ? "Enabled" : "Disabled",
					m_rectangleTextureFlipY ? "Enabled" : "Disabled");
			break;
		}

		case 0xE2: // GP0(E2h) - Texture Window setting
		{
			m_textureWindow.val = val;

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E2h) Set Texture Window: mask=(%u,%u), offset=(%u,%u)\n",
					val,
					m_textureWindow.maskX, m_textureWindow.maskY,
					m_textureWindow.offsetX, m_textureWindow.offsetY);

			m_textureWindowPrecomp.zero = m_textureWindow.maskX == 0 && m_textureWindow.maskY == 0 && m_textureWindow.offsetX == 0 && m_textureWindow.offsetY == 0;

			// Precompute texture window terms to avoid repeated per-pixel calculations
			// Texcoord = (Texcoord AND (NOT (Mask * 8))) OR ((Offset AND Mask) * 8)
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e2h-texture-window-setting
			m_textureWindowPrecomp.uAND = ~(m_textureWindow.maskX << 3);
			m_textureWindowPrecomp.uOR = (m_textureWindow.offsetX & m_textureWindow.maskX) << 3;
			m_textureWindowPrecomp.vAND = ~(m_textureWindow.maskY << 3);
			m_textureWindowPrecomp.vOR = (m_textureWindow.offsetY & m_textureWindow.maskY) << 3;
			break;
		}

		case 0xE3: // GP0(E3h) - Set Drawing Area top left (X1,Y1)
		{
			// Bits 9:0    X-coordinate (0..1023)
			// Bits 18:10  Y-coordinate (0..511)
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e3h-set-drawing-area-top-left-x1y1

			// Store register value for GPUREAD, discard bits 31:19.
			m_drawAreaTopLeft = val & 0x0007ffff;

			m_scissorRect.left = val & 0x3ff; // Bits 9:0
			m_scissorRect.top = (val >> 10) & 0x1ff; // Bits 18:10

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E3h) Set Drawing Area top-left (X1,Y1) = (%u,%u)\n", val, m_scissorRect.left, m_scissorRect.top);
			break;
		}

		case 0xE4: // GP0(E4h) - Set Drawing Area bottom right (X2,Y2)
		{
			// Bits 9:0    X-coordinate (0..1023)
			// Bits 18:10  Y-coordinate (0..511)
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e4h-set-drawing-area-bottom-right-x2y2

			// Store register value for GPUREAD, discard bits 31:19.
			m_drawAreaBottomRight = val & 0x0007ffff;

			m_scissorRect.right = val & 0x3ff; // Bits 9:0
			m_scissorRect.bottom = (val >> 10) & 0x1ff; // Bits 18:10

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E4h) Set Drawing Area bottom right (X2,Y2) = (%u,%u)\n", val, m_scissorRect.right, m_scissorRect.bottom);
			break;
		}

		case 0xE5: // GP0(E5h) - Set Drawing Offset (X,Y)
		{
			// Bits 10:0   X-offset [-1024,1023]
			// Bits 21:11  Y-offset [-1024,1023]
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e5h-set-drawing-offset-xy

			m_drawingOffset = val & 0x003fffff;

			m_drawingOffsetX = SignExtend11BitTo32Bit(val & 0x7ff); // Bits 10:0
			m_drawingOffsetY = SignExtend11BitTo32Bit((val >> 11) & 0x7ff); // Bits 21:11

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E5h) Set Drawing Offset (X,Y) = (%d,%d)\n", val, m_drawingOffsetX, m_drawingOffsetY);
			break;
		}

		case 0xE6: // GP0(E6h) - Mask Bit Setting
		{
			// Bit 0	Force set  0 = set from texture bit 15, 1 = always set
			// Bit 1    Test  0 = test disabled (always draw), 1 = test enabled (only draw if bit 15 set)
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting

			m_maskSetting.forceSet = (val & 0x1) != 0;
			m_maskSetting.testMask = (val & 0x2) != 0;

			if (s_logGP0)
				LOG_INFO("[GP0] %08X GP0(E6h) Mask Bit Setting: forceSet=%u, testMask=%u\n",
					val, m_maskSetting.forceSet, m_maskSetting.testMask);
			break;
		}

		case 0xFF: // Invalid command. Intelligent Qube seems to spam this: FFFFFFFF, FFFFFFF7, FFFFFFEF, FFFFFFE7 ...
		{
			if (s_logGP0 || s_logInvalidCommands)
				LOG_INFO("[GP0] Invalid environment command %08X \n", val);
			break;
		}

		default:
		{
			HP_FATAL_ERROR("Unexpected GP0 environment command: value %08X", val);
			break;
		}
	}
}

void GPU::endCommand()
{
	m_currentCommand = Command::None;
	m_commandWordCount = 0;
}

// GP0(02h) - Fill VRAM rectangle
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#quick-rectangle-fill
//
// Command format:
//   1st  Color+Command     (02BbGgRrh)  ;24bit RGB value (see note)
//   2nd  Top Left Corner   (YyyyXxxxh)  ;Xpos counted in halfwords, steps of 10h
//   3rd  Width+Height      (YsizXsizh)  ;Xsiz counted in halfwords, steps of 10h
//
void GPU::parseFillRectangleWord(u32 val)
{
	m_commandWordCount++;
	if (m_commandWordCount == 1)
	{
		HP_DEBUG_ASSERT(val >> 24 == 0x02, "Expected GP0(02h) Fill rectangle command");

		// Bits 23:0 : B8G8R8 colour
		// 8 bpp colour is encoded, but draw commands only support 5 bpp, so discard lower 3 bits of each channel.
		u32 r = (val & 0xff) >> 3;
		u32 g = ((val >> 8) & 0xff) >> 3;
		u32 b = ((val >> 16) & 0xff) >> 3;

		// Encode the colour as 15-bit BGR555 value
		// Note: Rectangle filling is not affected by the GP0(E6h) mask setting, acting as if GP0(E6h).0 and GP0(E6h).1 are both zero.
		m_fillRectangle.pixel = (u16)((b << 10) | (g << 5) | r);

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Fill Rectangle command word: color (R,G,B)=(%u,%u,%u) => pixel value B5G5R5 = 0x%04X\n",
				val, r << 3, g << 3, b << 3, m_fillRectangle.pixel);
	}
	else if (m_commandWordCount == 2)
	{
		// Mask and round. See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-and-rounding-for-fill-command-parameters
		m_fillRectangle.x = val & 0x3f0; // 10 bits, in steps of 10h
		m_fillRectangle.y = (val >> 16) & 0x1ff; // 9 bits, [0,511]

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Fill Rectangle param word: top-left corner (X,Y)=(%u halfwords,%u lines)\n",
				val, m_fillRectangle.x, m_fillRectangle.y);
	}
	else if (m_commandWordCount == 3)
	{
		// Mask and round. See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-and-rounding-for-fill-command-parameters
		// n.b. width and height of zero does *not* mean maximum here, unlike blit commands.
		u32 width = ((val & 0x3ff) + 0xf) & ~0xf; // [0,400h] in steps of 10h
		u32 height = (val >> 16) & 0x1ff; // 9 bits, [0,511]

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Fill Rectangle param word: size = %u halfwords x %u rows\n",
				val, width, height);

		// Wrapping https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#wrapping

		// Fill the rectangle in VRAM
		constexpr unsigned int kBPP = 2; // 16-bits per pixel A1B5G5R5 = 2 bytes per pixel. 

		for (u32 iy = 0; iy < height; iy++)
		{
			u32 pixelY = m_fillRectangle.y + iy;

			// Rows and columns wrap-around back to same row/column. https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#wrapping
			pixelY &= 0x1ff; // [0,511]

			for (u32 ix = 0; ix < width; ix++)
			{
				u32 pixelX = m_fillRectangle.x + ix;

				// Rows and columns wrap-around back to same row/column. https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#wrapping
				pixelX &= 0x3ff; // [0,1023]

				u32 vramOffset = (kVRAMWidthBytes * pixelY) + (kBPP * pixelX); // 16 bpp = 2 bytes per pixel
				HP_DEBUG_ASSERT(vramOffset + 2 <= kVRAMSizeBytes, "VRAM offset out of bounds");
				m_vram[vramOffset + 0] = m_fillRectangle.pixel & 0xff; // LSB
				m_vram[vramOffset + 1] = (m_fillRectangle.pixel >> 8) & 0xff; // MSB
			}
		}

		m_stats.fillRectangleCount++;
		endCommand();
	}
	else
	{
		HP_FATAL_ERROR("Unexpected Fill Rectangle command word count");
	}
}

// VRAM to VRAM blitting - command 4 (100)
//
// Command format:
//   1st  Command
//   2nd  Source Coord      (YyyyXxxxh)  ;Xpos counted in halfwords
//   3rd  Destination Coord (YyyyXxxxh)  ;Xpos counted in halfwords
//   4th  Width+Height      (YsizXsizh)  ;Xsiz counted in halfwords
//
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vram-to-vram-blitting-command-4-100
//
void GPU::parseVramToVramBlitWord(u32 val)
{
	HP_DEBUG_ASSERT(m_commandWordCount > 0, "Expect command word to have already been received");
	m_commandWordCount++;
	if (m_commandWordCount == 2)
	{
		// Decode and mask source coords
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		m_memoryTransfer.srcX = (val & 0x3ff); // 10 bits, in halfwords
		m_memoryTransfer.srcY = (val >> 16) & 0x1ff; // 9 bits
		if (s_logGP0)
			LOG_INFO("[GP0] %08X VRAM to VRAM blit param: src (x,y)=(%u,%u)\n", val, m_memoryTransfer.srcX, m_memoryTransfer.srcY);
	}
	else if (m_commandWordCount == 3)
	{
		// Decode and mask destination coords
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		m_memoryTransfer.dstX = (val & 0x3ff); // 10 bits, in halfwords
		m_memoryTransfer.dstY = (val >> 16) & 0x1ff; // 9 bits
		if (s_logGP0)
			LOG_INFO("[GP0] %08X VRAM to VRAM blit param: dst (x,y)=(%u,%u)\n", val, m_memoryTransfer.dstX, m_memoryTransfer.dstY);
	}
	else if (m_commandWordCount == 4)
	{
		// Decode and mask dimensions
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		u32 width = val & 0xffff; // in halfwords
		m_memoryTransfer.width = ((width - 1) & 0x3ff) + 1; // 10 bits. n.b. dimension of zero means maximum
		u32 height = (val >> 16) & 0xffff;
		m_memoryTransfer.height = ((height - 1) & 0x1ff) + 1; // 9 bits. n.b. dimension of zero means maximum
		if (s_logGP0)
			LOG_INFO("[GP0] %08X VRAM to VRAM blit param: size = %u halfwords x %u lines = %u halfwords\n",
				val, m_memoryTransfer.width, m_memoryTransfer.height, m_memoryTransfer.width * m_memoryTransfer.height);

		// Perform the VRAM to VRAM copy
		for (u32 row = 0; row < m_memoryTransfer.height; row++)
		{
			for (u32 col = 0; col < m_memoryTransfer.width; col++)
			{
				// Source coordinates with wrapping
				u32 srcX = (m_memoryTransfer.srcX + col) & 0x3ff; // [0,1023]
				u32 srcY = (m_memoryTransfer.srcY + row) & 0x1ff; // [0,511]
				u32 srcOffset = (kVRAMWidthBytes * srcY) + (2 * srcX); // 16 bits per pixel
				HP_DEBUG_ASSERT(srcOffset + 2 <= kVRAMSizeBytes, "VRAM source offset out of bounds");

				// Destination coordinates with wrapping
				u32 dstX = (m_memoryTransfer.dstX + col) & 0x3ff; // [0,1023]
				u32 dstY = (m_memoryTransfer.dstY + row) & 0x1ff; // [0,511]
				u32 dstOffset = (kVRAMWidthBytes * dstY) + (2 * dstX); // 16 bits per pixel
				HP_DEBUG_ASSERT(dstOffset + 2 <= kVRAMSizeBytes, "VRAM destination offset out of bounds");

				// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting
				if (m_maskSetting.testMask)
				{
					// Draw only if frame buffer bit 15 is *not* set.
					u8 dstMsb = m_vram[dstOffset + 1]; // MSB, little-endian
					if (dstMsb & 0x80) // bit set?
						continue; // don't copy this pixel
				}

				m_vram[dstOffset + 0] = m_vram[srcOffset + 0]; // copy LSB

				u8 msb = m_vram[srcOffset + 1]; // MSB (little-endian)
				if (m_maskSetting.forceSet)
					msb |= 0x80; // set mask bit
				m_vram[dstOffset + 1] = msb;
			}
		}

		m_stats.vramToVramBlitCount++;
		endCommand();
	}
	else
	{
		HP_FATAL_ERROR("Unexpected VRAM to VRAM blit command word count");
	}
}

// CPU to VRAM blitting - command 5 (101)
// 
// Command format:
//   1st  Command
//   2nd  Destination Coord (YyyyXxxxh)  ;Xpos counted in halfwords
//   3rd  Width+Height      (YsizXsizh)  ;Xsiz counted in halfwords
//   ...  Data              (...)      <--- usually transferred via DMA
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#cpu-to-vram-blitting-command-5-101
//
void GPU::parseCpuToVramBlitWord(u32 val)
{
	HP_DEBUG_ASSERT(m_commandWordCount > 0, "Expect command word to have already been received");
	m_commandWordCount++;
	if (m_commandWordCount == 2)
	{
		// Decode and mask destination coords
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		m_memoryTransfer.dstX = (val & 0x3ff); // 10 bits, in halfwords
		m_memoryTransfer.dstY = (val >> 16) & 0x1ff; // 9 bits

		if (s_logGP0)
			LOG_INFO("[GP0] %08X CPU to VRAM blit param: dst (x,y)=(%u,%u)\n", val, m_memoryTransfer.dstX, m_memoryTransfer.dstY);
	}
	else if (m_commandWordCount == 3)
	{
		// Decode and mask dimensions
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		u32 width = val & 0xffff; // in halfwords
		m_memoryTransfer.width = ((width - 1) & 0x3ff) + 1; // 10 bits. n.b. dimension of zero means maximum

		u32 height = (val >> 16) & 0xffff;
		m_memoryTransfer.height = ((height - 1) & 0x1ff) + 1; // 9 bits. n.b. dimension of zero means maximum

		if (s_logGP0)
			LOG_INFO("[GP0] %08X CPU to VRAM blit param: size = %u halfwords x %u lines = %u halfwords\n",
				val, m_memoryTransfer.width, m_memoryTransfer.height, m_memoryTransfer.width * m_memoryTransfer.height);

		// #TODO: Is this the correct time to reset memory transfer state?
		resetMemoryTransferState();
	}
	else
	{
		if (s_logGP0)
			LOG_INFO("[GP0] %08X CPU to VRAM blit data word\n", val);

		// Copy two halfwords of data to VRAM
		for (unsigned int halfwordIndex = 0; halfwordIndex < 2; halfwordIndex++)
		{
			// First halfword is in lower 16 bits
			u16 dataHalfword = (val >> (halfwordIndex * 16)) & 0xffff;

			// Rows and columns wrap-around back to same row/column. https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#wrapping
			u32 pixelX = (m_memoryTransfer.dstX + m_memoryTransfer.col) & 0x3ff; // [0,1023]
			u32 pixelY = (m_memoryTransfer.dstY + m_memoryTransfer.row) & 0x1ff; // [0,511]

			u32 vramOffset = (kVRAMWidthBytes * pixelY) + (2 * pixelX); // 16 bits per pixel
			HP_DEBUG_ASSERT(vramOffset + 2 <= kVRAMSizeBytes, "VRAM offset out of bounds");

			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting
			if (m_maskSetting.testMask)
			{
				// Draw only if frame buffer bit 15 is *not* set.
				u8 dstMsb = m_vram[vramOffset + 1]; // MSB, little-endian
				if (dstMsb & 0x80) // bit set?
					continue; // don't copy this pixel
			}

			m_vram[vramOffset + 0] = dataHalfword & 0xff; // LSB

			u8 msb = (dataHalfword >> 8) & 0xff; // MSB (little-endian)
			if (m_maskSetting.forceSet)
				msb |= 0x80; // set mask bit
			m_vram[vramOffset + 1] = msb;

			// Advance cursor and check stopping criteria
			m_memoryTransfer.col++;
			if (m_memoryTransfer.col == m_memoryTransfer.width)
			{
				m_memoryTransfer.col = 0;
				m_memoryTransfer.row++;
				if (m_memoryTransfer.row == m_memoryTransfer.height)
				{
					m_stats.cpuToVramBlitCount++; // #TODO: Increment at command start instead?
					endCommand();

					// #TODO: Is this the correct time to reset memory transfer state?
					resetMemoryTransferState();

					// Important. If width is an odd number, then we don't want to continue the loop and process the second (invalid) halfword.
					break;
				}
			}
		}
	}
}

// VRAM to CPU blitting - command 6 (110)
// 
// Command format:
//   1st  Command
//   2nd  Source Coord      (YyyyXxxxh)
//   3rd  Width+Height      (YsizXsizh)
// 
// Data words can then be read from GPUREAD port or via DMA
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vram-to-cpu-blitting-command-6-110
//
void GPU::parseVramToCpuBlitWord(u32 val)
{
	HP_DEBUG_ASSERT(m_commandWordCount > 0, "Expect command word to have already been received");
	m_commandWordCount++;
	if (m_commandWordCount == 2)
	{
		// Decode and mask source coords
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		m_memoryTransfer.srcX = (val & 0x3ff); // 10 bits, in halfwords
		m_memoryTransfer.srcY = (val >> 16) & 0x1ff; // 9 bits

		if (s_logGP0)
			LOG_INFO("[GP0] %08X VRAM to CPU blit param: src (x,y)=(%u,%u)\n", val, m_memoryTransfer.srcX, m_memoryTransfer.srcY);
	}
	else if (m_commandWordCount == 3)
	{
		// Decode and mask dimensions
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters
		u32 width = val & 0xffff; // in halfwords
		m_memoryTransfer.width = ((width - 1) & 0x3ff) + 1; // 10 bits. n.b. dimension of zero means maximum

		u32 height = (val >> 16) & 0xffff;
		m_memoryTransfer.height = ((height - 1) & 0x1ff) + 1; // 9 bits. n.b. dimension of zero means maximum

		if (s_logGP0)
			LOG_INFO("[GP0] %08X VRAM to CPU blit param: size = %u halfwords x %u lines = %u halfwords\n",
				val, m_memoryTransfer.width, m_memoryTransfer.height, m_memoryTransfer.width * m_memoryTransfer.height);

		// #TODO: Is this the correct time to reset memory transfer state?
		resetMemoryTransferState();
	}
	else
	{
		HP_FATAL_ERROR("[GP0] %08X Unexpected extra param word value in VRAM to CPU blit command", val);
	}
}

u32 GPU::updateVramToCpuBlitOutput()
{
	u32 output = 0;

	// Copy two halfwords of data from VRAM to GPUREAD
	for (unsigned int halfwordIndex = 0; halfwordIndex < 2; halfwordIndex++)
	{
		// Rows and columns wrap-around back to same row/column. https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#wrapping
		u32 pixelX = (m_memoryTransfer.srcX + m_memoryTransfer.col) & 0x3ff; // [0,1023]
		u32 pixelY = (m_memoryTransfer.srcY + m_memoryTransfer.row) & 0x1ff; // [0,511]

		u32 vramOffset = (kVRAMWidthBytes * pixelY) + (2 * pixelX); // 16 bpp = 2 bytes per pixel
		HP_DEBUG_ASSERT(vramOffset + 2 <= kVRAMSizeBytes, "VRAM offset out of bounds");
		u8 lsb = m_vram[vramOffset + 0]; // LSB
		u8 msb = m_vram[vramOffset + 1]; // MSB
		u16 dataHalfword = ((u16)msb << 8) | lsb;

		// Store in GPUREAD register
		// First halfword is in lower 16 bits
		if (halfwordIndex == 0)
			output = dataHalfword;
		else
			output |= (u32)dataHalfword << 16;

		// Advance cursor and check stopping criteria
		m_memoryTransfer.col++;
		if (m_memoryTransfer.col == m_memoryTransfer.width)
		{
			m_memoryTransfer.col = 0;
			m_memoryTransfer.row++;
			if (m_memoryTransfer.row == m_memoryTransfer.height)
			{
				endCommand();
				m_stats.vramToCpuBlitCount++; // #TODO: Increment at command start instead?

				// #TODO: Is this the correct time to reset memory transfer state?
				resetMemoryTransferState();

				// Important. If width is an odd number, then we don't want to continue the loop and process the second (invalid) halfword.
				break;
			}
		}
	}

	return output;
}

// #TODO: When is the correct time for memory transfer state (current row and column) to be reset?
// - When parse / decode a transfer command first word ?
// - When parse 3rd and final command word ?
// - When final word of data has been transferred ?
// - In GP1(01h) - Reset Command Buffer command ?
//
void GPU::resetMemoryTransferState()
{
	m_memoryTransfer.col = 0;
	m_memoryTransfer.row = 0;
}

// Fixed function shading.
// 
// "Modulation mode", as opposed to "raw texture mode".
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#modulation-also-known-as-texture-blending
//
static inline u16 modulateTextureColour(u32 vertexColourB8G8R8, u16 texelColourA1B5G5R5)
{
	// Multiply and divide by 128 (per PSX spec)
	// This spec defines this as: finalChannel.rgb = (texel.rgb * vertexColour.rgb) / vec3(128.0)  this is 8-bits * 8-bits = 16-bits >> 7 = 9 bits
	// The idea is that a vertex value of 0x80 will result in no modulation.

	// 8-bit vertex colour components
	u8 vert_r, vert_g, vert_b;
	unpackB8G8R8(vertexColourB8G8R8, vert_r, vert_g, vert_b);

	// 5-bit texel colour components + alpha
	u8 texel_r, texel_g, texel_b, texel_a;
	unpackA1B5G5R5(texelColourA1B5G5R5, texel_r, texel_g, texel_b, texel_a);

	// Multiply and divide by 128 (per PSX spec)
	// This spec defines this as: finalChannel.rgb = (texel.rgb * vertexColour.rgb) / vec3(128.0)  this is 8-bits * 8-bits = 16-bits >> 7 = 9 bits
	// The idea is that a vertex value of 0x80 will result in no modulation.
	// Here we have 5-bit * 8-bit = 13-bit result, which needs to be shifted down 4 bits to give an 9-bit result.
	int r = (vert_r * texel_r) >> 4;
	int g = (vert_g * texel_g) >> 4;
	int b = (vert_b * texel_b) >> 4;

	r = HP_CLAMP(r, 0, 0xff);
	g = HP_CLAMP(g, 0, 0xff);
	b = HP_CLAMP(b, 0, 0xff);

	// Convert to 5-bit
	u8 r5 = (u8)(r >> 3);
	u8 g5 = (u8)(g >> 3);
	u8 b5 = (u8)(b >> 3);

	// alpha comes from the texel
	u16 outColorA1B5G5R5 = (texel_a << 15) | (b5 << 10) | (g5 << 5) | r5;
	return outColorA1B5G5R5;
}

//
// Parse common vertex position attribute parameter word used by line, polygon and rectange primitives commands.
//
static void parsePrimitivePositionParameter(u32 val, /*out*/int& x, /*out*/int& y)
{
	// Sign-extend 11-bit coordinates
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vertex-parameter-for-polygon-line-rectangle-commands
	x = SignExtend11BitTo32Bit(val & 0x7ff); // 11 bits [-1024,1023]
	y = SignExtend11BitTo32Bit((val >> 16) & 0x7ff); // 11 bits [-1024,1023]
}

// Decodes ClutVVUU primitive command parameter word.
// Used in polygons and rectangle commands
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#clut-attribute-color-lookup-table-aka-palette
//
void GPU::decodePrimitiveClutUVParameter(u32 val,	/*out*/unsigned int& u, /*out*/unsigned int& v)
{
	u16 clutAttribute = val >> 16;
	m_clutX_halfwords = (clutAttribute & 0x3f) << 4; // Bits [5:0] (6 bits), in steps of 10h (16 pixels)
	m_clutY = (clutAttribute >> 6) & 0x1ff; // bits [14:6] (9 bits) [0,511]
	m_clutAddr = (m_clutY * kVRAMWidthBytes) + (2 * m_clutX_halfwords); // one halfword = 2 bytes

	// Decode vertex texcoords from lower 16-bits
	u = val & 0xff;
	v = (val >> 8) & 0xff;
}

// Decodes PageVVUU polygon command parameter word.
//
//  Bit    Description
//  0-8    Same as GP0(E1h).Bit0-8 (see there)
//  9-10   Unused (does NOT change GP0(E1h).Bit9-10)
//  11     Same as GP0(E1h).Bit11  (see there)
//  12-13  Unused (does NOT change GP0(E1h).Bit12-13)
//  14-15  Unused (should be 0)
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texpage-attribute-parameter-for-textured-polygons-commands
//
void GPU::decodePolygonTexPageUVParameter(u32 val,
	/*out*/unsigned int& u, /*out*/unsigned int& v)
{
	// Upper 16-bit affect global texture page state.
	u32 texturePageAttribute = val >> 16;
	
	// Bits 3:0   Texture page X Base   (N*64) i.e. in 64-halfword steps)
	m_gpustat.texturePageXBase = texturePageAttribute & 0xf; // GPUSTAT.0-3

	// Bit 4     Texture page Y Base 1 (N*256) (ie. 0, 256, 512 or 768)
	m_gpustat.texturePageYBase1 = (texturePageAttribute >> 4) & 1; // GPUSTAT.4

	// Bits 6:5   Blend mode     (0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)
	m_gpustat.blendMode = (texturePageAttribute >> 5) & 0x3; // GPUSTAT.5-6

	// Bits 8:7   Texture format   (0=4bit, 1=8bit, 2=15bit, 3=Reserved)
	u32 textureFormatIndex = (texturePageAttribute >> 7) & 3;
	m_gpustat.textureFormat = (TextureFormat)textureFormatIndex; // GPUSTAT.7-8

	// Bits 10:9 not used (do not affect GPUSTAT)

	// Bit 11    Texture page Y Base 2 (N*512) (only for 2 MB VRAM i.e. arcade, not PSX)
	m_gpustat.texturePageYBase2 = (texturePageAttribute >> 11) & 1;
//	HP_DEBUG_ASSERT(m_gpustat.texturePageYBase2 == 0, "Is this ever non-zero on PSX? Ignore if it is!"); // Assert disabled because Crash Bandicoot 2 sets this, presumably accidentally or for dev purposes
	
	// Bits 15:12 not used (do not affect GPUSTAT)

	updateTexturePageAddress();

	// Decode vertex texcoords from lower 16-bits
	u = val & 0xff;
	v = (val >> 8) & 0xff;
}

// Packet format
//
// First command word:
//
//  bit number   value   meaning
//   31-29        001    polygon render
//     28         1/0    gouraud / flat shading
//     27         1/0    4 / 3 vertices
//     26         1/0    textured / untextured
//     25         1/0    semi-transparent / opaque
//     24         1/0    raw texture / modulation
//    23-0        rgb    first color value.
//
// Subsequent words are vertex data, depending on vertex type:
//
//   Color      xxBBGGRR                         - optional, only present for gouraud shading (skipped for first vertex because encoded in command word)
//   Position   YYYYXXXX                         - required, two signed 16 bits values
//   TexCoord   ClutVVUU (vert 0), PageVVUU (vert 1) or xxxxVVUU (vert 2+)   - optional, only present for textured polygons

// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-polygon-commands
//
void GPU::parseDrawPolygonWord(u32 val)
{
	m_commandWordCount++;
	if (m_commandWordCount == 1) // command + colour
	{
		HP_DEBUG_ASSERT(val >> 29 == 0b001, "Not a polygon primitive command");

		// Bit 28 shading type
		m_polygon.shadingMode = (ShadingMode)((val >> 28) & 1);

		// Bit 27 quad
		m_polygon.quad = (val & (1 << 27)) != 0;

		// Bit 26 textured
		m_polygon.textured = (val & (1 << 26)) != 0;

		// Bit 25 semi-transparent
		m_polygon.semitransparent = (val & (1 << 25)) != 0;

		// Bit 24 raw texture / modulation
		m_polygon.rawTexture = (val & (1 << 24)) != 0;

		// Bits 23:0 vertex colour in B8G8R8 format
		m_polygon.vertices[0].colourB8G8R8 = val & 0xffffff; // store in first vertex

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Draw polygon command: %s%s%s%s%s vertex 0 colour (R,G,B)=(%u,%u,%u)\n",
				val,
				kShadingModeNames[(int)m_polygon.shadingMode],
				m_polygon.quad ? " quad" : " triangle",
				m_polygon.textured ? " textured" : " untextured",
				m_polygon.semitransparent ? " semi-transparent" : " opaque",
				m_polygon.rawTexture ? " raw" : " modulate",
				(m_polygon.vertices[0].colourB8G8R8 & 0xff), ((m_polygon.vertices[0].colourB8G8R8 >> 8) & 0xff), ((m_polygon.vertices[0].colourB8G8R8 >> 16) & 0xff));

		m_polygon.vertexIndex = 0; // first vertex
		m_polygon.vertexAttribute = VertexAttribute::Position; // The next vertex attribute word will always be position.
	}
	else
	{
		switch (m_polygon.vertexAttribute)
		{
			case VertexAttribute::Colour:
			{
				// Bits 23:0 B8G8R8 colour
				u32 colourB8G8R8 = val & 0xffffff;
				m_polygon.vertices[m_polygon.vertexIndex].colourB8G8R8 = colourB8G8R8;

				if (s_logGP0)
					LOG_INFO("[GP0] %08X Draw polygon param: vertex %u colour (R,G,B)=(%u,%u,%u)\n", val, m_polygon.vertexIndex,
						(colourB8G8R8 & 0xff), ((colourB8G8R8 >> 8) & 0xff), ((colourB8G8R8 >> 16) & 0xff));

				// Position attribute is required and always follows colour
				m_polygon.vertexAttribute = VertexAttribute::Position;
				break;
			}
			case VertexAttribute::Position:
			{
				int x, y;
				parsePrimitivePositionParameter(val, x, y);
				m_polygon.vertices[m_polygon.vertexIndex].x = x;
				m_polygon.vertices[m_polygon.vertexIndex].y = y;

				if (s_logGP0)
					LOG_INFO("[GP0] %08X Draw polygon param: vertex %u position (%d,%d)\n", val, m_polygon.vertexIndex, x, y);

				if (m_polygon.textured)
					m_polygon.vertexAttribute = VertexAttribute::TexCoord;
				else
				{
					m_polygon.vertexIndex++; // final attribute

					if (m_polygon.quad && m_polygon.vertexIndex == 4)
					{
						// All vertices received for quad

						bool dither = shouldDitherPolygon();

						// #TODO: Consider draw function dispatch table. May make code more readable, and could improve performance.

						if (m_polygon.shadingMode == ShadingMode::Gouraud)
						{
							if (m_polygon.textured)
							{
								if (dither)
									drawQuad</*gouraud*/true, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawQuad</*gouraud*/true, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
							else
							{
								if (dither)
									drawQuad</*gouraud*/true, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawQuad</*gouraud*/true, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
						}
						else
						{
							if (m_polygon.textured)
							{
								if (dither)
									drawQuad</*gouraud*/false, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawQuad</*gouraud*/false, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
							else
							{
								if (dither)
									drawQuad</*gouraud*/false, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawQuad</*gouraud*/false, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
						}
						endCommand();
						m_stats.quadCount++;
					}
					else if (!m_polygon.quad && m_polygon.vertexIndex == 3)
					{
						// All vertices received for triangle
						bool dither = shouldDitherPolygon();

						// #TODO: Consider draw function dispatch table. May make code more readable, and could improve performance.

						if (m_polygon.shadingMode == ShadingMode::Gouraud)
						{
							if (m_polygon.textured)
							{
								if (dither)
									drawTriangle</*gouraud*/true, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawTriangle</*gouraud*/true, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
							else
							{
								if (dither)
									drawTriangle</*gouraud*/true, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawTriangle</*gouraud*/true, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);

							}
						}
						else
						{
							if (m_polygon.textured)
							{
								if (dither)
									drawTriangle</*gouraud*/false, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawTriangle</*gouraud*/false, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
							else
							{
								if (dither)
									drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
								else
									drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							}
						}
						endCommand();
						m_stats.triangleCount++;
					}
					else
					{
						// First attribute is colour for Gouraud shaded polygons, or position for flat-shaded polygons.
						if (m_polygon.shadingMode == ShadingMode::Gouraud)
							m_polygon.vertexAttribute = VertexAttribute::Colour;
						else
							m_polygon.vertexAttribute = VertexAttribute::Position;
					}
				}
				break;
			}
			case VertexAttribute::TexCoord:
			{
				if (m_polygon.vertexIndex == 0)
				{
					// First vertex UV word has palette (CLUT) encoded in upper 16-bits

					decodePrimitiveClutUVParameter(val, /*out*/m_polygon.vertices[0].u, /*out*/m_polygon.vertices[0].v);

					if (s_logGP0)
						LOG_INFO("[GP0] %08X Draw polygon param: CLUT (x,y)=(%u,%u) addr=%08X, UV=(%u,%u)\n",
							val, m_clutX_halfwords, m_clutY, m_clutAddr, m_polygon.vertices[0].u, m_polygon.vertices[0].v);
				}
				else if (m_polygon.vertexIndex == 1)
				{
					// Second vertex UV word has tex page in upper 16-bits
					// This function updates global GPUSTAT tex page state
					decodePolygonTexPageUVParameter(val, /*out*/m_polygon.vertices[1].u, /*out*/m_polygon.vertices[1].v);

					if (s_logGP0)
						LOG_INFO("[GP0] %08X Draw polygon param: texture page (%u,%u) addr=%08X, blend mode=%u, texture format=%s, UV=(%u,%u)\n",
							val,
							m_gpustat.texturePageXBase * 64,
							(m_gpustat.texturePageYBase1 + m_gpustat.texturePageYBase2 * 2) * 256,
							m_texturePageAddr,
							m_gpustat.blendMode,
							kTextureFormatNames[(u32)m_gpustat.textureFormat],
							m_polygon.vertices[1].u, m_polygon.vertices[1].v);
				}
				else
				{
					// Subsequent UV command words only encode UVs in lower 16-bits (upper halfword unused)
					HP_DEBUG_ASSERT(m_polygon.vertexIndex < COUNTOF_ARRAY(m_polygon.vertices));
					Vertex& vertex = m_polygon.vertices[m_polygon.vertexIndex];
					vertex.u = val & 0xff;
					vertex.v = (val >> 8) & 0xff;

					if (s_logGP0)
						LOG_INFO("[GP0] %08X Draw polygon param: UV=(%u,%u)\n", val, vertex.u, vertex.v);
				}

				m_polygon.vertexIndex++; // final attribute - end of vertex

				if (m_polygon.quad && m_polygon.vertexIndex == 4)
				{
					// All vertices received for quad

					bool dither = shouldDitherPolygon();

					// #TODO: Consider draw function dispatch table. May make code more readable, and could improve performance.

					if (m_polygon.shadingMode == ShadingMode::Gouraud)
					{
						if (m_polygon.textured)
						{
							if (dither)
								drawQuad</*gouraud*/true, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawQuad</*gouraud*/true, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
						else
						{
							if (dither)
								drawQuad</*gouraud*/true, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawQuad</*gouraud*/true, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
					}
					else
					{
						if (m_polygon.textured)
						{
							if (dither)
								drawQuad</*gouraud*/false, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawQuad</*gouraud*/false, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
						else
						{
							if (dither)
								drawQuad</*gouraud*/false, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawQuad</*gouraud*/false, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.vertices[3], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
					}
					endCommand();
					m_stats.quadCount++;
				}
				else if (!m_polygon.quad && m_polygon.vertexIndex == 3)
				{
					// All vertices received for triangle

					bool dither = shouldDitherPolygon();

					// #TODO: Consider draw function dispatch table. May make code more readable, and could improve performance.

					if (m_polygon.shadingMode == ShadingMode::Gouraud)
					{
						if (m_polygon.textured)
						{
							if (dither)
								drawTriangle</*gouraud*/true, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawTriangle</*gouraud*/true, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
						else
						{
							if (dither)
								drawTriangle</*gouraud*/true, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawTriangle</*gouraud*/true, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
					}
					else
					{
						if (m_polygon.textured)
						{
							if (dither)
								drawTriangle</*gouraud*/false, /*textured*/true, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawTriangle</*gouraud*/false, /*textured*/true, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
						else
						{
							if (dither)
								drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/true>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
							else
								drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/false>(m_polygon.vertices[0], m_polygon.vertices[1], m_polygon.vertices[2], m_polygon.semitransparent, !m_polygon.rawTexture);
						}
					}
					endCommand();
					m_stats.triangleCount++;
				}
				else
				{
					// First attribute is colour for Gouraud shaded polygons, or position for flat-shaded polygons.
					if (m_polygon.shadingMode == ShadingMode::Gouraud)
						m_polygon.vertexAttribute = VertexAttribute::Colour;
					else
						m_polygon.vertexAttribute = VertexAttribute::Position;
				}

				break;
			}
		}
	}
}

#pragma region Rasterization

struct Point
{
	int x{};
	int y{};
};

// For an edge from P0 to P1, and point P returns:
// 
//     0 if P is on the edge (line)
//   > 0 if P is on the same side of the edge as the normal
//   < 0 if P is on the opposite side of the edge to the normal
//
// The Edge Function is defined as
// 
//     e(P) = dot(n, (P - V))
// 
// Where n is a normal vector pointing to the right created by rotating the edge vector 90 degrees:
//     n.x = -(p1.y - p0.y)
//     n.y = p1.x - p0.x
// 
// Where V is a point on the line (e.g. a vertex position) and P is a point to compare against the edge.
//
// x axis is right and y axis is down and we choose to draw triangles with clockwise vertex winding order.
//
// For example:  0---1
//               |  /
//               | /
//               |/
//               2
//
// If we are looking down the edge 0 to 1, then the normal points to the right, and the function returns:
// +ve value if P is to our right as we look down the the edge i.e. below the line in the picture
// -ve if P is to our left i.e. above the line in the picture
// 0 if P is on the line containing the edge.
//
// To calculate the edge function, we chose p0 as the point on the line:
// 
//     e(P) = dot( (-(p1.y - p0.y), p1.x - p0.x), (P - p0))
// 
// If P = (x,y) then
// 
//     e(x,y) = -(p1.y - p0.y) * (x - p0.x) + (p1.x - p0.x) * (y - p0.y)     Equation [A]
//
// This is the expression we evaluate in edgeFunction() below.
//
// We can refactor this to coefficients of x and y:
// 
//     e(x,y) = -(p1.y - p0.y) * x + (p1.x - p0.x) * y + (p1.y - p0.y) * p0.x - (p1.x - p0.x) * p0.y
//            = (p0.y - p1.y) * x + (p1.x - p0.x) * y + (p0.x * p1.y) - (p0.y * p1.x)
//            = ax + by + c     Equation [B]
// Where
//     a = p0.y - p1.y
//     b = p1.x - p0.x
//     c = (p0.x * p1.y) - (p0.y * p1.x)
//
// Stepping
// ========
//
// The edge function is used in rasterization to determine if a pixel is within the triangle being drawn.
// If have evaluated the edge function at position (x,y) and now want to test pixel (x+1,y) then we don't
// have to evaluate the whole expression again.
//
//     e(x,y) = ax + by + c
// So
//     e(x+1,y) = a(x+1) + by + c
//              = a + ax + by + c
//              = a + e(x,y)
// 
// Similarly, for the next pixel in the y direction:
//
//     e(x,y+1) = b + ax + by + c
//
// Integer overflow
// ================
//
// This should not be a problem. The PSX GPU does not use sub-pixel precision for rasterization.
// The range of coordinates is small enough to avoid overflow with 32-bit integers: 11 bits + drawing offset
// See The ryg blog "Triangle rasterization in practice" https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
// 
// Worst-case analysis:
// 
// - PSX coordinates: 11-bit signed values (-1024 to 1023)
// - Drawing offset: typically small, but let's say it could add another ~1024
// - So max coordinate value: ~2047
// - In orient2d, you compute: (p1.x - p0.x) * (p.y - p0.y) - (p1.y - p0.y) * (p.x - p0.x)
// - Max difference: 4094 (from -2047 to 2047)
// - Max product: 4094 × 4094 ≈ 16,760,836
// - This fits comfortably in a 32-bit signed int (max: 2,147,483,647)
//
// References:
// - Real-Time Rendering section 23.1 "Rasterization"
// - The ryg blog "The barycentric conspiracy" https://fgiesen.wordpress.com/2013/02/06/the-barycentric-conspirac/
//
static constexpr int edgeFunction(const Point& p0, const Point& p1, const Point& p)
{
	// This is equation [A] above
	return (p1.x - p0.x) * (p.y - p0.y) - (p1.y - p0.y) * (p.x - p0.x);
}

// Requires triangle to be clockwise ordered as viewed in PSX space with +Y down.
//
// The top-left rule is that a pixel center is defined to lie inside of a triangle if it lies on the top edge or the
// left edge of a triangle.
// 
// See:
// - DirectX Rasterization Rules https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules
// - The ryg blog "Triangle rasterization in practice" https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
//
static constexpr bool isTopOrLeftEdge(const Point& p0, const Point& p1)
{
	// A top edge, is an edge that is exactly horizontal and is above the other edges.
	// For clockwise triangles, an edge is a top edge if it is horizontal and p0.y == p1.y and p0.x < p1.x
	if (p0.y == p1.y)
		return p0.x < p1.x;

	// A left edge, is an edge that is not exactly horizontal and is on the left side of the triangle.
	// A triangle can have one or two left edges.
	// For clockwise triangles, an edge is a left edge if it is not horizontal and end is above start.
	return p1.y < p0.y;
}

//-------------------------------------------------------------------------------------------------------------

// Table of 8-bit RGB colour component offsets applied to each 4x4 area of VRAM, if enabled.
//
// - Polygons (triangles/quads) are dithered ONLY if they do use gouraud shading or modulation.
// - Lines are dithered (no matter if they are mono or do use gouraud shading).
// - Rectangles are NOT dithered (no matter if they do use modulation or not).
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#24bit-rgb-to-15bit-rgb-dithering-enabled-in-texpage-attribute
// https://jsgroth.dev/blog/posts/ps1-diamond/#dithering
//
static const s8 kDitherTable[4][4] =
{
	{-4,  0, -3 , 1},
	{ 2, -2,  3, -1},
	{-3,  1, -4,  0},
	{ 3, -1,  2, -2},
};

// The docs say "Polygons (triangles/quads) are dithered ONLY if they do use gouraud shading or modulation."
// This means:
// - Gouraud: Always dithered
// - Flat + untextured: NOT dithered
// - Flat + textured modulation: Dithered
// - Flat + textured raw: NOT dithered
//
bool GPU::shouldDitherPolygon() const
{
	if (!m_gpustat.dither24to15bit)
		return false;

	return (m_polygon.shadingMode == ShadingMode::Gouraud) || (m_polygon.textured && !m_polygon.rawTexture);
}

bool GPU::shouldDitherLine() const
{
	return m_gpustat.dither24to15bit;
}

static void ditherColour(unsigned int x, unsigned int y, u8& r8, u8& g8, u8& b8)
{
	s8 delta = kDitherTable[x & 3][y & 3]; // 4x4 grid in VRAM space
	r8 = HP_CLAMP((int)r8 + delta, 0, 0xff);
	g8 = HP_CLAMP((int)g8 + delta, 0, 0xff);
	b8 = HP_CLAMP((int)b8 + delta, 0, 0xff);
}

//-------------------------------------------------------------------------------------------------------------
//
// Draws a triangle primitive with optional Gouraud shading.
// For flat shaded triangles, the colour is taken from v0.colourB8G8R8.
//
// psx-spx states: Polygons are displayed up to *excluding* their lower-right coordinates.
// This implies that the top-left rule is used.
//
// The GPU ignores winding order and will draw triangles which are clockwise or anticlockwise. The GTE is expected to cull as required.
//
// References:
// - Real-Time Rendering section 23.1 "Rasterization"
// - The ryg blog "The barycentric conspiracy" https://fgiesen.wordpress.com/2013/02/06/the-barycentric-conspirac/
// - The ryg blog "Triangle rasterization in practice" https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
// - The ryg blog "Optimizing the basic rasterizer" https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/
//
template<bool gouraud, bool textured, bool dither>
void GPU::drawTriangle(const Vertex& vertex0, Vertex vertex1, Vertex vertex2, bool semitransparent, bool modulate)
{
	// Calculate screen space vertex positions
	Point p0{ vertex0.x + m_drawingOffsetX, vertex0.y + m_drawingOffsetY };
	Point p1{ vertex1.x + m_drawingOffsetX, vertex1.y + m_drawingOffsetY };
	Point p2{ vertex2.x + m_drawingOffsetX, vertex2.y + m_drawingOffsetY };

	// If triangle is not clockwise then swap p1 and p2 to make it so.
	// This is required for isTopOrLeftEdge to work correctly.
	int edgeFunc012 = edgeFunction(p0, p1, p2);
	if (edgeFunc012 < 0)
	{
		Swap(vertex1, vertex2);
		Swap(p1, p2);
		edgeFunc012 = -edgeFunc012;
	}

	const u32 colour0 = vertex0.colourB8G8R8; // used for both flat and gouraud shading

	// For Gouraud shading, we need the vertex colors
	u32 colour1, colour2;
	u32 r0, g0, b0, r1, g1, b1, r2, g2, b2;
	if constexpr (gouraud)
	{
		colour1 = vertex1.colourB8G8R8;
		colour2 = vertex2.colourB8G8R8;

		// Extract RGB components from each vertex
		r0 = (colour0 >> 0) & 0xFF;
		g0 = (colour0 >> 8) & 0xFF;
		b0 = (colour0 >> 16) & 0xFF;
		r1 = (colour1 >> 0) & 0xFF;
		g1 = (colour1 >> 8) & 0xFF;
		b1 = (colour1 >> 16) & 0xFF;
		r2 = (colour2 >> 0) & 0xFF;
		g2 = (colour2 >> 8) & 0xFF;
		b2 = (colour2 >> 16) & 0xFF;
	}

	// For textured triangles, we need the UV coordinates.
	u32 u0, v0, u1, v1, u2, v2;
	if constexpr (textured)
	{
		u0 = vertex0.u;
		v0 = vertex0.v;
		u1 = vertex1.u;
		v1 = vertex1.v;
		u2 = vertex2.u;
		v2 = vertex2.v;
	}

	// Compute axis-aligned bounding box for the triangle
	int minX = Min3(p0.x, p1.x, p2.x);
	int minY = Min3(p0.y, p1.y, p2.y);
	int maxX = Max3(p0.x, p1.x, p2.x);
	int maxY = Max3(p0.y, p1.y, p2.y);

	// The maximum distance between two vertices is 1023 horizontally, and 511 vertically.
	// Polygons and lines that are exceeding that dimensions are NOT rendered.
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vertex-parameter-for-polygon-line-rectangle-commands
	if ((maxX - minX) > 1023 || (maxY - minY) > 511)
		return;

	// Clip bounding box drawing window (scissor rectangle)
	minX = Max(minX, (int)m_scissorRect.left);
	minY = Max(minY, (int)m_scissorRect.top);
	maxX = Min(maxX, (int)m_scissorRect.right);
	maxY = Min(maxY, (int)m_scissorRect.bottom);

	if constexpr (textured)
	{
		// #TODO: Clip the UVs to the drawing window
	}

	// Early out if clipped bounding box is empty
	if (minX > maxX || minY > maxY)
		return;

	// Triangle Setup stage.
	// Calculate the coefficients a and b in the edge function Equation [B] for each edge. 
	const int a01 = p0.y - p1.y; // edge from p0 to p1
	const int a12 = p1.y - p2.y; // edge from p1 to p2
	const int a20 = p2.y - p0.y; // edge from p2 to p0
	const int b01 = p1.x - p0.x;
	const int b12 = p2.x - p1.x;
	const int b20 = p0.x - p2.x;

	// Use bias to implement tie-breaking rule i.e. top-left rule
	// This is a rasterization rule to prevent pixels being drawn twice on shared triangle edges.
	const int bias0 = isTopOrLeftEdge(p1, p2) ? 0 : -1;
	const int bias1 = isTopOrLeftEdge(p2, p0) ? 0 : -1;
	const int bias2 = isTopOrLeftEdge(p0, p1) ? 0 : -1;

	// Calculate the barycentric coordinates at the top-left corner of the bounding box
	Point p{ minX, minY };
	int w0_row = edgeFunction(p1, p2, p) + bias0; // edge p0 to p1
	int w1_row = edgeFunction(p2, p0, p) + bias1; // edge p2 to p0
	int w2_row = edgeFunction(p0, p1, p) + bias2; // edge p0 to p1

	// Implementation note: Could early out here if bounding box is empty, but for loop should achieve the same result.
	for (p.y = minY; p.y <= maxY; p.y++)
	{
		// Pre-calculated Barycentric coordinates for start of row
		int w0 = w0_row;
		int w1 = w1_row;
		int w2 = w2_row;

		for (p.x = minX; p.x <= maxX; p.x++)
		{
			// If p is on or inside all edges, render pixel.
			if (w0 >= 0 && w1 >= 0 && w2 >= 0)
			{
				u32 vertexColourB8G8R8;
				if constexpr (gouraud)
				{
					// Interpolate colors using barycentric coordinates (without bias)
					int w0_noBias = w0 - bias0;
					int w1_noBias = w1 - bias1;
					int w2_noBias = w2 - bias2;

					// Interpolate each color channel.
					// Divide by (2x) area of triangle to normalize the barycentric coordinates into [0,1] range weights.
					u32 r = (r0 * w0_noBias + r1 * w1_noBias + r2 * w2_noBias) / edgeFunc012;
					u32 g = (g0 * w0_noBias + g1 * w1_noBias + g2 * w2_noBias) / edgeFunc012;
					u32 b = (b0 * w0_noBias + b1 * w1_noBias + b2 * w2_noBias) / edgeFunc012;
//					HP_DEBUG_ASSERT(r <= 255 && g <= 255 && b <= 255, "Gouraud interpolated color channel out of range");

					vertexColourB8G8R8 = (b << 16) | (g << 8) | r;
				}
				else // flat
				{
					vertexColourB8G8R8 = colour0;
				}

				if constexpr (textured)
				{
					// Textured pixel

					// Interpolate UVs using barycentric coordinates (without bias)
					int w0_noBias = w0 - bias0;
					int w1_noBias = w1 - bias1;
					int w2_noBias = w2 - bias2;

					// Divide by (2x) area of triangle to normalize the barycentric coordinates into [0,1] range weights.
					u32 u = (u0 * w0_noBias + u1 * w1_noBias + u2 * w2_noBias) / edgeFunc012;
					u32 v = (v0 * w0_noBias + v1 * w1_noBias + v2 * w2_noBias) / edgeFunc012;
//					HP_DEBUG_ASSERT(u <= 255 && v <= 255, "Interpolated UVs out of range");

					u16 texColourA1B5G5R5 = sampleTexture(u, v);

					// Texture color black (0x0000) is treated as fully-transparent.
					// This means that textures cannot contain black pixels!
					// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texture-color-black-limitations
					if (texColourA1B5G5R5 != 0x0000)
					{
						// The psx-spx docs say:
						//     For textured primitives using 4-bit or 8-bit textures, bit 15 of each CLUT entry acts as a semi-transparency flag
						//     and determines whether to apply semi-transparency to the pixel or not.
						//     If the semi-transparency flag is off, the new pixel is written to VRAM as-is.
						// However, this seems to apply to *all* texture formats.
						if (semitransparent)
							semitransparent = (texColourA1B5G5R5 & 0x8000) != 0;

						// Shade

						if (modulate)
						{
							// Modulate (shader) in 8-bits. Don't clamp or downsample to 5 bits

							// 8-bit vertex colour components
							u8 vert_r, vert_g, vert_b;
							unpackB8G8R8(vertexColourB8G8R8, vert_r, vert_g, vert_b);

							// 5-bit texel colour components + alpha
							u8 texel_r, texel_g, texel_b, texel_a;
							unpackA1B5G5R5(texColourA1B5G5R5, texel_r, texel_g, texel_b, texel_a);

							// Multiply and divide by 128 (per PSX spec)
							// This spec defines this as: finalChannel.rgb = (texel.rgb * vertexColour.rgb) / vec3(128.0)  this is 8-bits * 8-bits = 16-bits >> 7 = 9 bits
							// The idea is that a vertex value of 0x80 will result in no modulation.
							// Here we have 5-bit * 8-bit = 13-bit result, which needs to be shifted down 4 bits to give an 9-bit result.
							int r = (vert_r * texel_r) >> 4;
							int g = (vert_g * texel_g) >> 4;
							int b = (vert_b * texel_b) >> 4;

							// The dither table must be applied to full-precision 8-bit values.
							if constexpr (dither)
							{
								s8 delta = kDitherTable[p.x & 3][p.y & 3]; // 4x4 grid in VRAM space
								r += delta;
								g += delta;
								b += delta;
							}

							// Clamp to 8-bit range after applying dithering
							r = HP_CLAMP(r, 0, 0xff);
							g = HP_CLAMP(g, 0, 0xff);
							b = HP_CLAMP(b, 0, 0xff);

							// Convert to 5-bit
							u8 r5 = (u8)(r >> 3);
							u8 g5 = (u8)(g >> 3);
							u8 b5 = (u8)(b >> 3);

							// alpha comes from the texel
							u16 outColorA1B5G5R5 = (texel_a << 15) | (b5 << 10) | (g5 << 5) | (r5 << 0);

							drawPixelA1B5G5R5(p.x, p.y, outColorA1B5G5R5, semitransparent);
						}
						else // not modulated
						{
							drawPixelA1B5G5R5(p.x, p.y, texColourA1B5G5R5, semitransparent);
						}
					}
				}
				else // not textured
				{
					// Untextured pixel
					drawPixelB8G8R8<dither>(p.x, p.y, vertexColourB8G8R8, semitransparent);
				}
			}

			// step edge functions by one pixel in x
			w0 += a12;
			w1 += a20;
			w2 += a01;
		}

		// Step edge functions by one pixel in y
		w0_row += b12;
		w1_row += b20;
		w2_row += b01;
	}
}

// Explicit template instantiations
template void GPU::drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/false>(const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/true,  /*textured*/false, /*dither*/false>(const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/false, /*textured*/true,  /*dither*/false>(const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/true,  /*textured*/true,  /*dither*/false>(const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/false, /*textured*/false, /*dither*/true> (const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/true,  /*textured*/false, /*dither*/true> (const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/false, /*textured*/true,  /*dither*/true> (const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
template void GPU::drawTriangle</*gouraud*/true,  /*textured*/true,  /*dither*/true> (const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);

template<bool gouraud, bool textured, bool dither>
void GPU::drawQuad(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate)
{
	// Quads are internally processed as two triangles, the first consisting of vertices 1,2,3, and the second of vertices 2,3,4.
	// - https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-polygon-commands
	drawTriangle<gouraud, textured, dither>(v0, v1, v2, semitransparent, modulate);

	if constexpr (gouraud)
		drawTriangle<gouraud, textured, dither>(v1, v2, v3, semitransparent, modulate);
	else
	{
		// For flat shaded triangles, the colour is taken from the first vertex of the triangle.
		// Only v0 will contain the colour, so need to copy to v1
		Vertex v1_coloured = v1;
		v1_coloured.colourB8G8R8 = v0.colourB8G8R8;
		drawTriangle<gouraud, textured, dither>(v1_coloured, v2, v3, semitransparent, modulate);
	}
}

// Explicit template instantiations
template void GPU::drawQuad</*gouraud*/false, /*textured*/false, /*dither*/false>(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/true,  /*textured*/false, /*dither*/false>(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/false, /*textured*/true , /*dither*/false>(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/true,  /*textured*/true , /*dither*/false>(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/false, /*textured*/false, /*dither*/true> (const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/true,  /*textured*/false, /*dither*/true> (const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/false, /*textured*/true , /*dither*/true> (const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);
template void GPU::drawQuad</*gouraud*/true,  /*textured*/true , /*dither*/true> (const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);

#pragma endregion Rasterization

// Packet format
//
// First command word:
//
// bit number   value   meaning
//  31-29        010    line render
//    28         1/0    gouraud / flat shading
//    27         1/0    polyline / single line
//    25         1/0    semi-transparent / opaque
//   23-0        rgb    first color value.
//
// Each subsequent vertex is 1 or 2 words:
//
//   Color      xxBBGGRR    - optional, only present for gouraud shading (skipped for first vertex because encoded in command word)
//   Position   YYYYXXXX    - required, two signed 16 bits values
//
// Polyline is terminated when word & 0xF000F000) == 0x50005000
//
// If the 2 vertices in a line are at same position, then the GPU will draw a 1x1 rectangle in the location of the 2 vertices using the colour of the first vertex.
//
// #TODO: If dithering is enabled (via Texpage command), then both monochrome and shaded lines are drawn with dithering (this differs from monochrome polygons and monochrome rectangles).
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-line-commands
//
void GPU::parseDrawLineWord(u32 val)
{
	m_commandWordCount++;
	if (m_commandWordCount == 1) // command + colour
	{
		HP_DEBUG_ASSERT(val >> 29 == 0b010, "Not a line primitive command");

		// Bit 28 shading type
		m_line.shadingMode = (ShadingMode)((val >> 28) & 1);

		// Bit 27 polyline
		m_line.polyline = (val & (1 << 27)) != 0;

		// Bit 25 semi-transparent
		m_line.semitransparent = (val & (1 << 25)) != 0;

		// Bits 23:0 B8G8R8 colour
		m_line.colourB8G8R8 = val & 0xffffff;

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Render line command: %s%s%s RGB=(%u,%u,%u)\n",
				val,
				kShadingModeNames[(int)m_line.shadingMode],
				m_line.polyline ? " polyline" : " single line",
				m_line.semitransparent ? " semi-transparent" : " opaque",
				(m_line.colourB8G8R8 & 0xff), ((m_line.colourB8G8R8 >> 8) & 0xff), ((m_line.colourB8G8R8 >> 16) & 0xff));

		m_line.vertexIndex = 0; // first vertex
		m_line.vertexAttribute = VertexAttribute::Position; // The next vertex attribute word will always be position.
	}
	else if (m_line.polyline && (val & 0xf000f000) == 0x50005000) // polyline terminator?
	{
		if (s_logGP0)
			LOG_INFO("[GP0] %08X Render Line param: polyline terminator\n", val);

		endCommand();
		m_stats.polyLineCount++;
	}
	else // vertex data
	{
		switch (m_line.vertexAttribute)
		{
			case VertexAttribute::Colour:
			{
				// Bits 23:0 B8G8R8 colour
				m_line.colourB8G8R8 = val & 0xffffff;

				if (s_logGP0)
					LOG_INFO("[GP0] %08X Render Line param: vertex colour (R,G,B)=(%u,%u,%u)\n", val,
						(m_line.colourB8G8R8 & 0xff), ((m_line.colourB8G8R8 >> 8) & 0xff), ((m_line.colourB8G8R8 >> 16) & 0xff));

				// Position attribute is required and always follows colour
				m_line.vertexAttribute = VertexAttribute::Position;
				break;
			}

			case VertexAttribute::Position:
			{
				int x, y;
				parsePrimitivePositionParameter(val, x, y);

				if (s_logGP0)
					LOG_INFO("[GP0] %08X Render Line param: position (%d,%d)\n", val, x, y);

				// Vertex position is final vertex attribute received so move to next vertex.
				m_line.vertexIndex++;

				if (m_line.vertexIndex == 1)
				{
					m_line.x = x;
					m_line.y = y;

					if (m_line.shadingMode == ShadingMode::Gouraud)
					{
						m_line.colourB8G8R8_prev = m_line.colourB8G8R8; // store colour of first vertex
						m_line.vertexAttribute = VertexAttribute::Colour; // For Gouraud shaded lines, the first vertex attribute is colour.
					}
					else // flat shading
						m_line.vertexAttribute = VertexAttribute::Position; // For flat shaded lines, the first and only vertex attribute is position.
				}
				else if (m_line.polyline) // polyline second vertex onwards
				{
					if (m_line.shadingMode == ShadingMode::Gouraud)
					{
						if (shouldDitherLine())
							drawLineGouraud</*dither*/true>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8_prev, m_line.colourB8G8R8, m_line.semitransparent);
						else
							drawLineGouraud</*dither*/false>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8_prev, m_line.colourB8G8R8, m_line.semitransparent);

						m_line.colourB8G8R8_prev = m_line.colourB8G8R8; // store colour for first vertex of subsequent line
						m_line.vertexAttribute = VertexAttribute::Colour; // For Gouraud shaded lines, the first vertex attribute is colour.
					}
					else // flat shading
					{
						if (shouldDitherLine())
							drawLineFlat</*dither*/true>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8, m_line.semitransparent);
						else
							drawLineFlat</*dither*/false>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8, m_line.semitransparent);
						m_line.vertexAttribute = VertexAttribute::Position; // For flat shaded lines, the first and only vertex attribute is position.
					}

					// second vert = first vert for next line
					m_line.x = x;
					m_line.y = y;

					m_stats.lineCount++;
				}
				else // single line second (and final) vertex
				{
					HP_DEBUG_ASSERT(m_line.vertexIndex == 2);
					if (m_line.shadingMode == ShadingMode::Gouraud)
					{
						if (shouldDitherLine())
							drawLineGouraud</*dither*/true>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8_prev, m_line.colourB8G8R8, m_line.semitransparent);
						else
							drawLineGouraud</*dither*/false>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8_prev, m_line.colourB8G8R8, m_line.semitransparent);
					}
					else
					{
						if (shouldDitherLine())
							drawLineFlat</*dither*/true>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8, m_line.semitransparent);
						else
							drawLineFlat</*dither*/false>(m_line.x, m_line.y, x, y, m_line.colourB8G8R8, m_line.semitransparent);
					}

					// End of command
					endCommand();
					m_stats.lineCount++;
				}
				break;
			}

			default:
				HP_DEBUG_FATAL_ERROR("Unexpected vertex attribute for line primitive.");
				break;
		}
	}
}

//
// PSX line drawing algorithm
//
// Bresenham is not used. Instead 16.16 fixed point is used to draw the line.
// This doesn't seem to be documented, but MAME implements it this way and matches PeterLemon/PSX reference images.
//
template<bool dither>
void GPU::drawLineFlat(int x1, int y1, int x2, int y2, u32 colourB8G8R8, bool semitransparent)
{
	// By inspection of PeterLemon/GPU/16BPP/RenderLine/RenderLine16BPP.exe reference image it looks like draw direction
	// can be swapped.
	if (y1 > y2 || x1 > x2)
	{
		Swap(x1, x2);
		Swap(y1, y2);
	}

	const int dx = x2 - x1;
	const int dy = y2 - y1;

	// The psx-spx docs are incorrect when they say:
	//     Lines are displayed up to and *including* their lower-right coordinates (ie. unlike as for polygons, the lower-right coordinate is not excluded)
	// A pixel *is* excluded.
	const unsigned int nx = dx < 0 ? -dx : dx;
	const unsigned int ny = dy < 0 ? -dy : dy;
	unsigned int len = (unsigned int)Max(nx, ny);
	len = Max(len, 1u); // Two coincident verts results in a single pixel

	// Discard lines exceeding maximum dimension
	// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vertex-parameter-for-polygon-line-rectangle-commands
	if (nx >= 1024 || ny >= 512)
		return;

	// Initial position, including drawing offset.
	// Positions are represented in 16.16 fixed-point format to allow sub-pixel accuracy
	int x_16_16 = (x1 + m_drawingOffsetX) << 16;
	int y_16_16 = (y1 + m_drawingOffsetY) << 16;

	// Stepping deltas use 16.16 fixed-point to represent sub-pixel positions
	const s32 dx_16_16 = (dx << 16) / (s32)len;
	const s32 dy_16_16 = (dy << 16) / (s32)len;

	for (unsigned int i = 0; i < len; i++)
	{
		int x = x_16_16 >> 16;
		int y = y_16_16 >> 16;

		if (x >= (int)m_scissorRect.left && x <= (int)m_scissorRect.right && y >= (int)m_scissorRect.top && y <= (int)m_scissorRect.bottom)
			drawPixelB8G8R8<dither>(x, y, colourB8G8R8, semitransparent);

		// increment for next pixel
		x_16_16 += dx_16_16;
		y_16_16 += dy_16_16;
	}
}

template<bool dither>
void GPU::drawLineGouraud(int x1, int y1, int x2, int y2, u32 colourB8G8R8_1, u32 colourB8G8R8_2, bool semitransparent)
{
	// By inspection of PeterLemon/GPU/16BPP/RenderLine/RenderLine16BPP.exe reference image it looks like draw direction
	// can be swapped.
	if (y1 > y2 || x1 > x2)
	{
		Swap(x1, x2);
		Swap(y1, y2);
		Swap(colourB8G8R8_1, colourB8G8R8_2);
	}

	const int dx = x2 - x1;
	const int dy = y2 - y1;

	// The psx-spx docs are incorrect when they say:
	//     Lines are displayed up to and *including* their lower-right coordinates (ie. unlike as for polygons, the lower-right coordinate is not excluded)
	// A pixel *is* excluded.
	const unsigned int nx = dx < 0 ? -dx : dx;
	const unsigned int ny = dy < 0 ? -dy : dy;
	unsigned int len = (unsigned int)Max(nx, ny);
	len = Max(len, 1u); // Two coincident verts results in a single pixel

	// Discard lines exceeding maximum dimension
	// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vertex-parameter-for-polygon-line-rectangle-commands
	if (nx >= 1024 || ny >= 512)
		return;

	// Initial pixel values
	// Values are represented in 16.16 fixed-point format to allow sub-pixel accuracy.
	int x_16_16 = x1 << 16;
	int y_16_16 = y1 << 16;
	u32 r1 = colourB8G8R8_1 & 0xff;
	u32 g1 = (colourB8G8R8_1 >> 8) & 0xff;
	u32 b1 = (colourB8G8R8_1 >> 16) & 0xff;
	u32 r_16_16 = (r1 << 16);
	u32 g_16_16 = (g1 << 16);
	u32 b_16_16 = (b1 << 16);

	// Stepping deltas use 16.16 fixed-point to represent sub-pixel positions
	const s32 dx_16_16 = (dx << 16) / (s32)len;
	const s32 dy_16_16 = (dy << 16) / (s32)len;

	// Also interpolate 8-bit colour channels in 16.16 format.
	// I tried 8.8 fixed point interpolation, but didn't make results match reference image any better.
	const s32 dr_16_16 = ((s16)((colourB8G8R8_2 & 0xff) - r1) << 16) / (s32)len;
	const s32 dg_16_16 = ((s16)(((colourB8G8R8_2 >> 8) & 0xff) - g1) << 16) / (s32)len;
	const s32 db_16_16 = ((s16)(((colourB8G8R8_2 >> 16) & 0xff) - b1) << 16) / (s32)len;

	for (unsigned int i = 0; i < len; i++)
	{
		int x = x_16_16 >> 16;
		int y = y_16_16 >> 16;

		int pixelX = m_drawingOffsetX + x;
		int pixelY = m_drawingOffsetY + y;

		if (pixelX >= (int)m_scissorRect.left && pixelX <= (int)m_scissorRect.right && pixelY >= (int)m_scissorRect.top && pixelY <= (int)m_scissorRect.bottom)
		{
			u32 colourB8G8R8 = packB8G8R8((u8)(r_16_16 >> 16), (u8)(g_16_16 >> 16), (u8)(b_16_16 >> 16));
			drawPixelB8G8R8<dither>(pixelX, pixelY, colourB8G8R8, semitransparent);
		}

		// increment for next pixel
		x_16_16 += dx_16_16;
		y_16_16 += dy_16_16;
		r_16_16 += dr_16_16;
		g_16_16 += dg_16_16;
		b_16_16 += db_16_16;
	}
}

// First command word:
// 
//  bit number   value   meaning
//   31-29        011    rectangle render
//   28-27        ss     rectangle size
//     26         0/1    untextured / textured
//     25         0/1    opaque / semi-transparent
//     24         1/0    raw texture / modulation
//    23-0        rgb    first color value.
//
// Size field:
//  0 (00)      variable size
//  1 (01)      single pixel (1x1)
//  2 (10)      8x8 sprite
//  3 (11)      16x16 sprite
//
// Command parameters:
//   Color         ccBBGGRR    - command + color; color is ignored when textured
//   Vertex1       YYYYXXXX    - required, indicates the upper left corner to render. Each component is 11-bits and must be sign-extended.
//   UV            ClutVVUU    - optional, only present for textured rectangles
//   Width+Height  YsizXsiz    - optional, dimensions for variable sized rectangles (max 1023x511)
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-rectangle-commands
//
void GPU::parseDrawRectangleWord(u32 val)
{
	m_commandWordCount++;
	if (m_commandWordCount == 1) // command + colour
	{
		HP_DEBUG_ASSERT(val >> 29 == 0b011, "Not a rectangle primitive command");

		// Bits 28:27 Size field
		m_rectangle.size = (RectangleSize)((val >> 27) & 3);

		// Bit 26 textured
		m_rectangle.textured = (val & (1 << 26)) != 0;

		// Bit 25 semi-transparent
		m_rectangle.semitransparent = (val & (1 << 25)) != 0;

		// Bit 24 raw texture / modulation
		m_rectangle.rawTexture = (val & (1 << 24)) != 0;

		// Bits 23:0 vertex colour in B8G8R8 format
		m_rectangle.vertexB8G8R8 = val & 0xffffff;

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Render Rectangle command: %s%s%s%s RGB=(%u,%u,%u)\n",
				val,
				kRectangleSizeNames[(int)m_rectangle.size],
				m_rectangle.textured ? " textured" : " untextured",
				m_rectangle.semitransparent ? " semi-transparent" : " opaque",
				m_rectangle.rawTexture ? " raw" : " modulate",
				(m_rectangle.vertexB8G8R8 & 0xff), ((m_rectangle.vertexB8G8R8 >> 8) & 0xff), ((m_rectangle.vertexB8G8R8 >> 16) & 0xff));
	}
	else if (m_commandWordCount == 2) // pos
	{
		parsePrimitivePositionParameter(val, /*out*/m_rectangle.x, /*out*/m_rectangle.y);

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Render Rectangle param: position (x,y)=(%d,%d)\n", val, m_rectangle.x, m_rectangle.y);

		if (m_rectangle.size != RectangleSize::Variable && !m_rectangle.textured) // fixed size untextured rectangle?
		{
			// All command words received for fixed-size untextured rectangle, render it now.

			switch (m_rectangle.size)
			{
				case RectangleSize::k1x1:
				{
					// Draw a single pixel to VRAM

					// Apply drawing offset
					int x = m_drawingOffsetX + m_rectangle.x;
					int y = m_drawingOffsetY + m_rectangle.y;

					// Scissor (cull)
					if (x >= (int)m_scissorRect.left && x <= (int)m_scissorRect.right && y >= (int)m_scissorRect.top && y <= (int)m_scissorRect.bottom)
						drawPixelB8G8R8</*dither*/false>(x, y, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					break;
				}
				case RectangleSize::k8x8:
				{
					drawUntexturedRectangle(m_rectangle.x, m_rectangle.y, 8, 8, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					break;
				}
				case RectangleSize::k16x16:
				{
					drawUntexturedRectangle(m_rectangle.x, m_rectangle.y, 16, 16, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					break;
				}
				default:
					HP_FATAL_ERROR("Unreachable code");
					break;
			}

			endCommand();
			m_stats.drawRectangleCount++;
		}
	}
	else if (m_commandWordCount == 3) // ClutVVUU for textured rectangles or width+height for variable sized untextured rectangles
	{
		if (m_rectangle.textured)
		{
			// Parse ClutVVUU word
			decodePrimitiveClutUVParameter(val, /*out*/m_rectangle.u, /*out*/m_rectangle.v);

			if (s_logGP0)
				LOG_INFO("[GP0] %08X Render Rectangle param: CLUT (x,y)=(%u,%u) addr=%08X, UV=(%u,%u)\n",
					val, m_clutX_halfwords, m_clutY, m_clutAddr, m_rectangle.u, m_rectangle.v);

			switch (m_rectangle.size)
			{
				case RectangleSize::k1x1:
				{
					drawTexturedRectangle(m_rectangle.x, m_rectangle.y, 1, 1, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					endCommand();
					m_stats.drawRectangleCount++;
					break;
				}
				case RectangleSize::k8x8:
				{
					drawTexturedRectangle(m_rectangle.x, m_rectangle.y, 8, 8, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					endCommand();
					m_stats.drawRectangleCount++;
					break;
				}
				case RectangleSize::k16x16:
				{
					drawTexturedRectangle(m_rectangle.x, m_rectangle.y, 16, 16, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);
					endCommand();
					m_stats.drawRectangleCount++;
					break;
				}
				case RectangleSize::Variable:
					// Variable size textured rectangle
					// Width+ height command word still to come.
					break;
			}
		}
		else // Variable size untextured rectangle
		{
			// Parse width+height word
			u32 width = val & 0x3ff; // 10 bits [0,1023]
			u32 height = (val >> 16) & 0x1ff; // 9 bits [0,511]

			if (s_logGP0)
				LOG_INFO("[GP0] %08X Render Rectangle param: size = %u x %u\n", val, width, height);

			// Draw variable size rectangle
//			HP_DEBUG_ASSERT(width > 0 && height > 0, "Is zero width or height valid?"); // Assert removed: Castlevania SOTN seems to draw 0 width rectangles (Dracula battle at start of game)
			if (width > 0 && height > 0)
				drawUntexturedRectangle(m_rectangle.x, m_rectangle.y, width, height, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);

			endCommand();
			m_stats.drawRectangleCount++;
		}
	}
	else if (m_commandWordCount == 4) // width+height for variable sized textured rectangles
	{
		// Parse width+height word
		u32 width = val & 0x3ff; // 10 bits [0,1023]
		u32 height = (val >> 16) & 0x1ff; // 9 bits [0,511]

		if (s_logGP0)
			LOG_INFO("[GP0] %08X Render Rectangle param: size = %u x %u\n", val, width, height);

//		HP_DEBUG_ASSERT(width > 0 && height > 0, "Is zero width or height valid?"); // Assert removed: Parodius uses Width 0
		if (width > 0 && height > 0)
			drawTexturedRectangle(m_rectangle.x, m_rectangle.y, width, height, m_rectangle.vertexB8G8R8, m_rectangle.semitransparent);

		endCommand();
		m_stats.drawRectangleCount++;
	}
	else
	{
		HP_UNREACHABLE();
	}
}

void GPU::drawUntexturedRectangle(int rectX, int rectY, unsigned int w, unsigned int h, u32 colourB8G8R8, bool semitransparent)
{
	HP_DEBUG_ASSERT(rectX >= -1024 && rectX <= 1023, "Expect primitives to have signed 11-bit value");

	// Calculate rectangle bounds
	int x_min = m_drawingOffsetX + rectX;
	int y_min = m_drawingOffsetY + rectY;
	HP_DEBUG_ASSERT(w > 0 && h > 0, "Width and height must be greater than zero");
	int x_max = x_min + w - 1; // inclusive
	int y_max = y_min + h - 1; // inclusive

	// Clip to the Drawing Area (scissor rectangle)
	x_min = Max(x_min, (int)m_scissorRect.left);
	y_min = Max(y_min, (int)m_scissorRect.top);
	x_max = Min(x_max, (int)m_scissorRect.right);
	y_max = Min(y_max, (int)m_scissorRect.bottom);
	if (x_max < x_min || y_max < y_min)
		return; // Fully clipped

	for (int y = y_min; y <= y_max; y++)
	{
		for (int x = x_min; x <= x_max; x++)
		{
			drawPixelB8G8R8</*dither*/false>(x, y, colourB8G8R8, semitransparent);
		}
	}
}
void GPU::drawTexturedRectangle(int rectX, int rectY, unsigned int w, unsigned int h, u32 vertexColourB8G8R8, bool semitransparent)
{
	HP_DEBUG_ASSERT(rectX >= -1024 && rectX <= 1023, "Expect primitives to have signed 11-bit value");

	// Calculate rectangle bounds
	int x_min = m_drawingOffsetX + rectX;
	int y_min = m_drawingOffsetY + rectY;
	HP_DEBUG_ASSERT(w > 0 && h > 0, "Width and height must be greater than zero");
	int x_max = x_min + w - 1; // inclusive
	int y_max = y_min + h - 1; // inclusive

	// Clip the rectangle bounds and texcoords to the Drawing Area (scissor rectangle).
	unsigned int u0 = m_rectangle.u;
	unsigned int v0 = m_rectangle.v;
	if (x_min < (int)m_scissorRect.left)
	{
		unsigned int delta = (unsigned int)((int)m_scissorRect.left - x_min);
		u0 = (u0 + delta) & 0xff; // wrap 8-bit texcoord
		x_min = (int)m_scissorRect.left;
	}
	if (y_min < (int)m_scissorRect.top)
	{
		unsigned int delta = (unsigned int)((int)m_scissorRect.top - y_min);
		v0 = (v0 + delta) & 0xff; // wrap 8-bit texcoord
		y_min = (int)m_scissorRect.top;
	}
	x_max = Min(x_max, (int)m_scissorRect.right);
	y_max = Min(y_max, (int)m_scissorRect.bottom);
	if (x_max < x_min || y_max < y_min)
		return; // Fully clipped

	// Rectangle textures can be flipped https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texture-origin-and-xy-flip
	int du = m_rectangleTextureFlipX ? -1 : 1;
	int dv = m_rectangleTextureFlipY ? -1 : 1;

	unsigned int v = v0;
	for (int y = y_min; y <= y_max; y++)
	{
		unsigned int u = u0;
		for (int x = x_min; x <= x_max; x++)
		{
			u16 texColourA1B5G5R5 = sampleTexture(u, v);
			u = (u + du) & 0xff; // wrap-around

			// Texture color black (0x0000) is treated as fully-transparent.
			// This means that textures cannot contain black pixels!
			// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texture-color-black-limitations
			if (texColourA1B5G5R5 == 0x0000) // transparent?
				continue;

			// The psx-spx docs say:
			//     For textured primitives using 4-bit or 8-bit textures, bit 15 of each CLUT entry acts as a semi-transparency flag
			//     and determines whether to apply semi-transparency to the pixel or not.
			//     If the semi-transparency flag is off, the new pixel is written to VRAM as-is.
			// However, this seems to apply to *all* texture formats.
			if (semitransparent)
				semitransparent = (texColourA1B5G5R5 & 0x8000) != 0;

			// Shade
			u16 finalColourA1B5G5R5 = m_rectangle.rawTexture ? texColourA1B5G5R5 : modulateTextureColour(vertexColourB8G8R8, texColourA1B5G5R5);
			drawPixelA1B5G5R5(x, y, finalColourA1B5G5R5, semitransparent);
		}

		v = (v + dv) & 0xff; // wrap-around
	}
}

// When semi-transparency is set for a pixel, the GPU first reads the pixel it wants to write to,
// and then calculates the color it will write from the 2 pixels according to the semi-transparency mode selected.
//
// The mode is set by GP0(E1h) - Draw Mode setting (aka "Texpage")
// 
// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#semi-transparency
//
u16 GPU::blend(unsigned int srcR5, unsigned int srcG5, unsigned int srcB5, unsigned int vramOffset) const
{
	// Read existing pixel
	u8 dstLsb = m_vram[vramOffset + 0]; // LSB
	u8 dstMsb = m_vram[vramOffset + 1]; // MSB
	u16 dstPixel = ((u16)dstMsb << 8) | dstLsb;

	// Extract dst components
	u32 dstR5 = dstPixel & 0x1f;
	u32 dstG5 = (dstPixel >> 5) & 0x1f;
	u32 dstB5 = (dstPixel >> 10) & 0x1f;

	// Blend
	// When using additive blending, if a channel's intensity is greater than 255, it gets clamped to 255 rather than being masked.
	// When using subtractive blending and a channel's intensity ends up being < 0, it is clamped to 0.
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#semi-transparency
	s32 outR, outG, outB;
	switch (m_gpustat.blendMode)
	{
		case 0: // (B/2 + F/2), which is actually implmented as (B + F) / 2
			outR = (dstR5 + srcR5) >> 1;
			outG = (dstG5 + srcG5) >> 1;
			outB = (dstB5 + srcB5) >> 1;
			break;
		case 1: // B+F
			outR = dstR5 + srcR5;
			outG = dstG5 + srcG5;
			outB = dstB5 + srcB5;
			break;
		case 2: // B-F
			outR = (s32)dstR5 - (s32)srcR5;
			outG = (s32)dstG5 - (s32)srcG5;
			outB = (s32)dstB5 - (s32)srcB5;
			break;
		/*case 3:*/default: // B+(F/4)
			outR = dstR5 + (srcR5 >> 2);
			outG = dstG5 + (srcG5 >> 2);
			outB = dstB5 + (srcB5 >> 2);
			break;
	}

	outR = HP_CLAMP(outR, 0, 0x1f);
	outG = HP_CLAMP(outG, 0, 0x1f);
	outB = HP_CLAMP(outB, 0, 0x1f);

	u16 outPixel = (u16)((outB << 10) | (outG << 5) | outR);
	return outPixel;
}

//
// Used for drawing untextured pixels, which derive their colour purely from the 8-bit vertex colour.
//
template<bool dither>
void GPU::drawPixelB8G8R8(unsigned int x, unsigned int y, u32 colorB8G8R8, bool semitransparent)
{
	HP_DEBUG_ASSERT(x < kVRAMWidth16bpp); // 16 bits per pixel
	HP_DEBUG_ASSERT(y < kVRAMHeightLines);
	constexpr unsigned int kBPP = 2; // 16-bits per pixel A1B5G5R5 = 2 bytes per pixel. 
	u32 vramOffset = (kVRAMWidthBytes * y) + (kBPP * x);
	HP_DEBUG_ASSERT(vramOffset + 2 <= kVRAMSizeBytes, "VRAM offset out of bounds");

	if (m_maskSetting.testMask)
	{
		// Draw only if frame buffer bit 15 is *not* set.
		u8 dstMsb = m_vram[vramOffset + 1]; // MSB, little-endian
		if (dstMsb & 0x80) // bit set?
			return;
	}

	u16 outColourA1B5G5R5;

	if constexpr (dither)
	{
		// Apply dithering before blending
		u8 r8 = (u8)(colorB8G8R8 & 0xff);
		u8 g8 = (u8)((colorB8G8R8 >> 8) & 0xff);
		u8 b8 = (u8)((colorB8G8R8 >> 16) & 0xff);
		ditherColour(x, y, r8, g8, b8);

		u8 r5 = (r8 >> 3) & 0x1f;
		u8 g5 = (g8 >> 3) & 0x1f;
		u8 b5 = (b8 >> 3) & 0x1f;

		if (semitransparent)
		{
			// Extract src components, used for semi-transparency blending
			outColourA1B5G5R5 = blend(r5, g5, b5, vramOffset);
		}
		else // opaque
		{
			outColourA1B5G5R5 = packA1B5G5R5(r5, g5, b5, /*a*/1);
		}
	}
	else // no dither
	{
		if (semitransparent)
		{
			u8 srcR5, srcG5, srcB5;
			unpackB8G8R8to5bit(colorB8G8R8, srcR5, srcG5, srcB5);
			outColourA1B5G5R5 = blend(srcR5, srcG5, srcB5, vramOffset);
		}
		else // opaque
		{
			outColourA1B5G5R5 = convertB8G8R8toB5G5R5(colorB8G8R8);
		}
	}

	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting
	if (m_maskSetting.forceSet)
		outColourA1B5G5R5 |= 0x8000; // always set mask bit

#if 0
	m_vram[vramOffset + 0] = outColourA1B5G5R5 & 0xff; // LSB
	m_vram[vramOffset + 1] = (outColourA1B5G5R5 >> 8) & 0xff; // MSB
#else
	// Optimisation: Write 16 bit value in single operation, assuming host is little-endian.
	*(u16*)(m_vram + vramOffset) = outColourA1B5G5R5;
#endif
}

void GPU::drawPixelA1B5G5R5(unsigned int x, unsigned int y, u16 colorA1B5G5R5, bool semitransparent)
{
	HP_DEBUG_ASSERT(x < kVRAMWidth16bpp); // 16 bits per pixel
	HP_DEBUG_ASSERT(y < kVRAMHeightLines);
	constexpr unsigned int kBPP = 2; // 16-bits per pixel A1B5G5R5 = 2 bytes per pixel. 
	u32 vramOffset = (kVRAMWidthBytes * y) + (kBPP * x);
	HP_DEBUG_ASSERT(vramOffset + 2 <= kVRAMSizeBytes, "VRAM offset out of bounds");

	if (m_maskSetting.testMask)
	{
		// Draw only if frame buffer bit 15 is *not* set.
		u8 dstMsb = m_vram[vramOffset + 1]; // MSB, little-endian
		if (dstMsb & 0x80) // bit set?
			return;
	}

	u16 outColourA1B5G5R5;

	if (semitransparent)
	{
		// Extract src components, used for semi-transparency blending
		u8 srcR5, srcG5, srcB5, srcA1;
		unpackA1B5G5R5(colorA1B5G5R5, srcR5, srcG5, srcB5, srcA1);
		outColourA1B5G5R5 = blend(srcR5, srcG5, srcB5, vramOffset);
	}
	else // opaque
	{
		outColourA1B5G5R5 = colorA1B5G5R5;
	}

	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting
	if (m_maskSetting.forceSet)
		outColourA1B5G5R5 |= 0x8000; // always set mask bit
	else
	{
		// For untextured primitives: colorA1B5G5R5 bit 15 is 0, so mask bit stays cleared
		// For textured primitives: colorA1B5G5R5 bit 15 comes from texture sample
		outColourA1B5G5R5 |= (colorA1B5G5R5 & 0x8000);
	}

#if 0
	m_vram[vramOffset + 0] = outColourA1B5G5R5 & 0xff; // LSB
	m_vram[vramOffset + 1] = (outColourA1B5G5R5 >> 8) & 0xff; // MSB
#else
	// Optimisation: Write 16 bit value in single operation, assuming host is little-endian.
	*(u16*)(m_vram + vramOffset) = outColourA1B5G5R5;
#endif
}

//
// Calculates and stores texture page base address to avoid per sample recalculation.
// Texture pages must be aligned to 64 halfwords (128 bytes) in x and 256 in y.
//
void GPU::updateTexturePageAddress()
{
	u32 texturePageX_halfwords = m_gpustat.texturePageXBase * 64;
	u32 texturePageY = 256 * m_gpustat.texturePageYBase1;
	m_texturePageAddr = (texturePageY * kVRAMWidthBytes) + (2 * texturePageX_halfwords);
}

// Always point sampling
u16 GPU::sampleTexture(unsigned int u, unsigned int v) const
{
	HP_DEBUG_ASSERT(u <= 0xff && v <= 0xff);

	// Respect texture window
	if (!m_textureWindowPrecomp.zero)
	{
		// Texcoord = (Texcoord AND (NOT (Mask * 8))) OR ((Offset AND Mask) * 8)
		// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e2h-texture-window-setting
		// Use precomputed terms to avoid repeated per-pixel calculations.
		u = (u & m_textureWindowPrecomp.uAND) | m_textureWindowPrecomp.uOR;
		v = (v & m_textureWindowPrecomp.vAND) | m_textureWindowPrecomp.vOR;
	}

	// Calculate texture pixel coordinates in VRAM

	u16 sample = 0;
	switch (m_gpustat.textureFormat)
	{
		case TextureFormat::k4BitPalette:
		{
			// V is expressed in 4-bit units
			u32 texelAddr = m_texturePageAddr + (v * kVRAMWidthBytes) + (u / 2); // 4 bpp = 0.5 bytes per pixel
			u8 colourIndex = m_vram[texelAddr];
			// low nibble for even u, high nibble for odd u
			if (u & 1)
				colourIndex = (colourIndex >> 4) & 0x0f;
			else
				colourIndex = colourIndex & 0x0f;

			// Sample colour from CLUT
			u32 colourAddr = m_clutAddr + (2 * colourIndex); // A1B5G5R5 16 bpp = 2 bytes per pixel
			sample = m_vram[colourAddr] | (m_vram[colourAddr + 1] << 8); // little-endian

			break;
		}
		case TextureFormat::k8BitPalette:
		{
			// V is expressed in 8-bit units
			u32 texelAddr = m_texturePageAddr + (v * kVRAMWidthBytes) + u; // 8 bpp = 1 byte per pixel
			u8 colourIndex = m_vram[texelAddr];

			// Sample colour from CLUT
			u32 colourAddr = m_clutAddr + (2 * colourIndex); // A1B5G5R5 16 bpp = 2 bytes per pixel
			sample = m_vram[colourAddr] | (m_vram[colourAddr + 1] << 8); // little-endian
			break;
		}
		case TextureFormat::kA1B5G5R5:
		case TextureFormat::kReserved: // Texture page colors setting 3 (reserved) is same as setting 2 (15bit).
		{
			// V is expressed in 16-bit units
			u32 addr = m_texturePageAddr + (v * kVRAMWidthBytes) + (2 * u); // 16 bpp = 2 bytes per pixel
			sample = m_vram[addr] | (m_vram[addr + 1] << 8); // little-endian
			break;
		}
	}

	return sample;
}

// GP1(00h) - Reset GPU
// 
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp100h-reset-gpu
//
void GPU::resetGPU()
{
	// GP1(01h) clear FIFO
	resetCommandBuffer();

	// GP1(02h) ack irq (0)
	m_gpustat.interruptRequestIRQ1 = 0;

	 // GP1(03h) display off(1)
	m_gpustat.displayDisable = 1;

	// GP1(04h)       dma off(0)
	// Bits 1:0  DMA Direction (0=Off, 1=FIFO, 2=CPUtoGP0, 3=GPUREADtoCPU) ;GPUSTAT.29-30
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp104h-dma-direction-data-request
	m_gpustat.dmaDirection = DMADirection::Off;
	m_gpustat.dmaDataRequest = 0; // set value of dependent state appropriately

	// GP1(05h)       display address(0)
	m_displayStartX = 0;
	m_displayStartY = 0;

	// GP1(06h) Horizontal Display range (on Screen) x1, x2 (x1 = 200h, x2 = 200h + 256 * 10)
	m_displayRangeX1 = 0x200;
	m_displayRangeX2 = 0x200 + (256 * 10);

	// GP1(07h)        display y1, y2 (y1 = 010h, y2 = 010h + 240)
	m_displayRangeY1 = 0x10;
	m_displayRangeY2 = 0x10 + 240;

	// GP1(08h) display mode 320x200 NTSC(0)
	m_gpustat.horizontalResolution1 = 1; // GPUSTAT.17-18 320 pixels
	m_gpustat.verticalResolution = 0; // GPUSTAT.19 200 pixels
	m_gpustat.videoMode = 0; // GPUSTAT.20 NTSC
	m_gpustat.displayFormat = (DisplayFormat)0; // GPUSTAT.21 15bit #TODO: Is this correct behaviour? Not documented in psx-spx.
	m_gpustat.verticalInterlace = 0; // GPUSTAT.22 non-interlaced #TODO: Is this correct behaviour? Not documented in psx-spx.
	m_gpustat.horizontalResolution2 = 0; // GPUSTAT.16 256/320/512/640 #TODO: Is this correct behaviour? Not documented in psx-spx.
	m_gpustat.flipScreenHorizontally = 0; // GPUSTAT.14 off #TODO: Is this correct behaviour? Not documented in psx-spx.

	// derived value
	m_horizontalResolution = 320;

	// GP0(E1h..E6h)  rendering attributes(0)

	// GP0(E1h) - Draw mode setting (aka TEXPAGE)
	m_gpustat.texturePageXBase = 0; // GPUSTAT.0-3
	m_gpustat.texturePageYBase1 = 0; // GPUSTAT.4
	m_gpustat.blendMode = 0; // GPUSTAT.5-6
	m_gpustat.textureFormat = (TextureFormat)0; // GPUSTAT.7-8
	m_gpustat.dither24to15bit = 0; // GPUSTAT.9
	m_gpustat.drawingToDisplayArea = 0; // GPUSTAT.10
	m_gpustat.texturePageYBase2 = 0; // only for 2 MB VRAM i.e. arcade, not PSX
	m_rectangleTextureFlipX = 0;
	m_rectangleTextureFlipY = 0;

	m_texturePageAddr = 0; // Reset derived texture page address

	// GP0(E2h)
	m_textureWindow.val = 0;
	m_textureWindowPrecomp = {};

	// GP0(E3h,E4h)
	m_drawAreaTopLeft = 0;
	m_drawAreaBottomRight = 0;
	m_scissorRect.left = 0;
	m_scissorRect.top = 0;
	m_scissorRect.right = 0; // #TODO: Is this correct, or should it be max value?
	m_scissorRect.bottom = 0; // #TODO: Is this correct, or should it be max value?

	// GP0(E5h)
	m_drawingOffset = 0;
	m_drawingOffsetX = 0;
	m_drawingOffsetY = 0;

	// GP0(E6h)
	m_maskSetting.forceSet = 0;
	m_maskSetting.testMask = 0;
}

// GP1(01h) - Reset Command Buffer
//
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp101h-reset-command-buffer
//
void GPU::resetCommandBuffer()
{
	// #TODO: Reset the command buffer (FIFO) and CLUT cache. See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp101h-reset-command-buffer
	m_currentCommand = Command::None;
	m_commandWordCount = 0;
}

// GP1(10h) Read GPU internal register (GPUREAD)
//
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp110h-read-gpu-internal-register
//
void GPU::parseGPUREAD(u32 val)
{
	// After sending the command, the result can be read (immediately) from GPUREAD register. There is no need for a NOP or other delay.
	// Note: GPUSTAT.Bit27 is used only for VRAM reads, but NOT for register reads, so do not try to wait for that flag.

	// Bits 23:0  Register index (via following GPUREAD)
	// 
	// 00h-01h = Returns Nothing (old value in GPUREAD remains unchanged)
	// 02h     = Read Texture Window setting  ;GP0(E2h) ;20bit/MSBs=Nothing
	// 03h     = Read Draw area top left      ;GP0(E3h) ;19bit/MSBs=Nothing
	// 04h     = Read Draw area bottom right  ;GP0(E4h) ;19bit/MSBs=Nothing
	// 05h     = Read Draw offset             ;GP0(E5h) ;22bit
	// 06h-07h = Returns Nothing (old value in GPUREAD remains unchanged)
	// 08h-FFFFFFh = Mirrors of 00h..07h
	unsigned int regIndex = val & 7; // mirror 0,7 up to ffffff
	switch (regIndex)
	{
		case 0:
			break;
		case 1:
			break;
		case 2:
			m_gpuread = m_textureWindow.val;
			break;
		case 3:
			m_gpuread = m_drawAreaTopLeft;
			break;
		case 4:
			m_gpuread = m_drawAreaBottomRight;
			break;
		case 5:
			m_gpuread = m_drawingOffset;
			break;
		case 6:
			break;
		case 7:
			break;
	}

	if (s_logGP1)
		LOG_INFO("[GP1] %08X GP1(10h) read GPU register %u value %08X\n", val, regIndex, m_gpuread);
}
