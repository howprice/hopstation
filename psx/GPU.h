/*
PlayStation(PSX) GPU

The PSX GPU is purely 2D. The GTE is responsible for transforming 3D to 2D, but the GPU does not accept Z or W coords.

References:
- https://psx-spx.consoledev.net/graphicsprocessingunitgpu/
- https://jsgroth.dev/blog/posts/ps1-bare-minimum-gpu/
*/

#pragma once

#include "core/Types.h"
#include "core/ArrayHelpers.h" // COUNTOF_ARRAY
#include "core/Helpers.h" // ENUM_COUNT

// VRAM / framebuffer
// 
// PSX has 1 MB VRAM
// Laid out as 512 lines of 2048 bytes.
// Horizontal coordinates depend on pixel format used
//   Unit    4bit  8bit  16bit  24bit   Halfwords
//   Width   4096  2048  1024   682.66  1024

static constexpr u32 kVRAMSizeBytes = 1 * 1024 * 1024; // 1 MB
static constexpr unsigned int kVRAMHeightLines = 512;
static constexpr unsigned int kVRAMWidthBytes = 2048;
static constexpr unsigned int kVRAMWidth16bpp = kVRAMWidthBytes / 2; // 1024. For 16-bit pixels with format A1B5G5R5 (stored in little-endian)
//static constexpr unsigned int kVRAMWidth24bpp = kVRAMWidthBytes / 3; // 682 (rounded down). For 24-bit pixels with format B8G8R8. Deliberately commented out because could lead to bugs due to rounding error.

// The PSX GPU does not have a depth buffer. Software is responsible for depth ordering. See https://jsgroth.dev/blog/posts/ps1-diamond/

// Textures
// ========
// 
// - Texture page is 256x256 area of vram
// - Texture pages must be aligned to 64 halfwords (128 bytes) in x and 256 in y
// - UVs are 8-bit unsigned integers
// - Three texture formats:
//   - 4-bit palette (4 bpp)
//   - 8-bit palette (8 bpp)
//   - A1B5G5R5 (16 bpp)
// - Texture coords confined to "Texture Window", to support repeating
// - GPU has texture cache, but only needs to be implmented to emulate overlapping VRAM to VRAM blits. See https://www.libretro.com/index.php/introducing-vulkan-psx-renderer-for-beetlemednafen-psx/
// - No texture filtering (nearest neighbour only)
// - No mipmapping
// 
// See:
// - https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texture-bitmaps
// - https://www.libretro.com/index.php/introducing-vulkan-psx-renderer-for-beetlemednafen-psx/

enum class TextureFormat : u32 // explicit unsigned storage type so can cast to 2-bit bitfield
{
	k4BitPalette, // 4 bpp
	k8BitPalette, // 8 bpp
	kA1B5G5R5, // 16 bpp
	kReserved, // Texture page colors setting 3 (reserved) is same as setting 2 (15bit).

	Max = kReserved
};

// The GPU can only render in 15 (16) bpp A1B5G5R5 format, but the video out can interpret VRAM as either A1B5G5R5 (16 bpp) or B8G8R8 (24 bpp) format.
// 24 bpp is usually only used for MDEC video playback.
// n.b. PSX stores 24 bpp pixels in RGB order in consecutive bytes, despite being called "B8G8R8" in documentation.
enum class DisplayFormat : u32
{
	A1B5G5R5, // 16 bpp
	B8G8R8,   // 24 bpp

	Max = B8G8R8
};

inline const char* kDisplayFormatNames[] =
{
	"A1B5G5R5",
	"B8G8R8"
};
static_assert(COUNTOF_ARRAY(kDisplayFormatNames) == ENUM_COUNT(DisplayFormat));

inline const unsigned int kDisplayFormatBitsPerPixel[] =
{
	16, // A1B5G5R5
	24, // B8G8R8
};
static_assert(COUNTOF_ARRAY(kDisplayFormatBitsPerPixel) == ENUM_COUNT(DisplayFormat));

class GPU
{
public:

	static constexpr unsigned int kMaxDisplayWidthPixels = 640;
	static constexpr unsigned int kMaxDisplayHeightPixels = 480; // NTSC

	enum class DMADirection : u32 // explicit unsigned storage type so can cast to 2-bit bitfield
	{
		Off,
		FIFO,
		CPUToGP0,
		GPUREADtoCPU,

		Max = GPUREADtoCPU
	};

	enum class ShadingMode : u32
	{
		Flat,    // constant colour
		Gouraud, // colour linearly interpolated between vertices

		Max = Gouraud
	};

	// Size field:
	//  0 (00)      variable size
	//  1 (01)      single pixel (1x1)
	//  2 (10)      8x8 sprite
	//  3 (11)      16x16 sprite
	// 
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-rectangle-commands
	//
	enum class RectangleSize
	{
		Variable,
		k1x1,
		k8x8,
		k16x16,

		Max = k16x16
	};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif
	/*
	GPU Status Register (GPUSTAT) - 32 bits

	  0-3   Texture page X Base   (N*64)                              ;GP0(E1h).0-3
	  4     Texture page Y Base 1 (N*256) (ie. 0, 256, 512 or 768)    ;GP0(E1h).4
	  5-6   Blend mode            (0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)  ;GP0(E1h).5-6
	  7-8   Texture format        (0=4-bit palette (4 bpp), 1=8-bit palette (8 bpp), 2=16-bit A1B5G5R5, 3=Reserved) ;GP0(E1h).7-8
	  9     Dither 24bit to 15bit (0=Off/strip LSBs, 1=Dither Enabled);GP0(E1h).9
	  10    Drawing to display area (0=Prohibited, 1=Allowed)         ;GP0(E1h).10
	  11    Set Mask-bit when drawing pixels (0=No, 1=Yes/Mask)       ;GP0(E6h).0
	  12    Draw Pixels           (0=Always, 1=Not to Masked areas)   ;GP0(E6h).1
	  13    Interlace Field       (or, always 1 when GP1(08h).5=0)
	  14    Flip screen horizontally (0=Off, 1=On, v1 only)           ;GP1(08h).7
	  15    Texture page Y Base 2 (N*512) (only for 2 MB VRAM)        ;GP0(E1h).11
	  16    Horizontal Resolution 2     (0=256/320/512/640, 1=368)    ;GP1(08h).6
	  17-18 Horizontal Resolution 1     (0=256, 1=320, 2=512, 3=640)  ;GP1(08h).0-1
	  19    Vertical Resolution         (0=240, 1=480, when Bit22=1)  ;GP1(08h).2
	  20    Video Mode                  (0=NTSC/60Hz, 1=PAL/50Hz)     ;GP1(08h).3
	  21    Display Area Color Depth    (0=15/16bpp, 1=24bpp)         ;GP1(08h).4
	  22    Vertical Interlace          (0=Off, 1=On)                 ;GP1(08h).5   This is essentially a "line-skipping" bit when in interlace mode
	  23    Display Enable              (0=Enabled, 1=Disabled)       ;GP1(03h).0
	  24    Interrupt Request (IRQ1)    (0=Off, 1=IRQ)       ;GP0(1Fh)/GP1(02h)
	  25    DMA / Data Request, meaning depends on GP1(04h) DMA Direction:
			  When GP1(04h)=0 (DMA off)          ---> Always zero (0)
			  When GP1(04h)=1                    ---> FIFO State  (0=Full, 1=Not Full)
			  When GP1(04h)=2 (CPUtoGP0 DMA)     ---> Same as GPUSTAT.28 (Ready to receive DMA Block)
			  When GP1(04h)=3 (GPUREADtoCPU DMA) ---> Same as GPUSTAT.27 (Ready to send VRAM to CPU)
	  26    Ready to receive Cmd Word   (0=No, 1=Ready)  ;GP0(...) ;via GP0
	  27    Ready to send VRAM to CPU   (0=No, 1=Ready)  ;GP0(C0h) ;via GPUREAD
	  28    Ready to receive DMA Block  (0=No, 1=Ready)  ;GP0(...) ;via GP0
	  29-30 DMA Direction (0=Off, 1=FIFO, 2=CPUtoGP0, 3=GPUREADtoCPU)    ;GP1(04h).0-1
	  31    Drawing even/odd lines in interlace mode (0=Even or Vblank, 1=Odd).

	Source: https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-status-register
	*/
	struct StatusRegister
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 texturePageXBase : 4; // [0:3] (N*64) GP0(E1h).0-3
				u32 texturePageYBase1 : 1; // [4] (N*256) GP0(E1h).4
				u32 blendMode : 2; // [6:5] GP0(E1h).5-6
				TextureFormat textureFormat : 2; // [8:7] GP0(E1h).7-8
				u32 dither24to15bit : 1; // [9] GP0(E1h).9
				u32 drawingToDisplayArea : 1; // [10] GP0(E1h).10
				u32 setMaskBitWhenDrawingPixels : 1; // [11] GP0(E6h).0
				u32 drawPixels : 1; // [12] GP0(E6h).1
				u32 interlaceField : 1; // [13]
				u32 flipScreenHorizontally : 1; // [14] GP1(08h).7
				u32 texturePageYBase2 : 1; // [15] GP0(E1h).11   Only for arcade boards with 2 MB VRAM; not used in PSX 1 MB VRAM
				u32 horizontalResolution2 : 1; // [16] GP1(08h).6
				u32 horizontalResolution1 : 2; // [18:17] GP1(08h).0-1
				u32 verticalResolution : 1; // [19] GP1(08h).2
				u32 videoMode : 1; // [20] GP1(08h).3
				DisplayFormat displayFormat : 1; // [21] GP1(08h).4
				u32 verticalInterlace : 1; // [22] GP1(08h).5
				u32 displayDisable : 1; // [23] GP1(03h).0  n.b. I've renamed this from "displayEnable" to match the bit meaning: 0=Enabled, 1=Disabled
				u32 interruptRequestIRQ1 : 1; // [24]
				u32 dmaDataRequest : 1; // [25]
				u32 readyToReceiveCmdWord : 1; // [26]
				u32 readyToSendVramToCpu : 1; // [27]
				u32 readyToReceiveDmaBlock : 1; // [28]
				DMADirection dmaDirection : 2; // [30:29]
				u32 drawingEvenOddLinesInInterlaceMode : 1; // [31]
			};
		};
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	struct Stats
	{
		u64 GP0CommandCount = 0;
		u64 GP1CommandCount = 0;
		u64 fillRectangleCount = 0;
		u64 vramToVramBlitCount = 0;
		u64 cpuToVramBlitCount = 0;
		u64 vramToCpuBlitCount = 0;
		u64 triangleCount = 0;
		u64 quadCount = 0;
		u64 lineCount = 0;
		u64 polyLineCount = 0;
		u64 drawRectangleCount = 0;
	};

	GPU();
	~GPU();

	// This is not the Reset command, rather resets internal state to known values.
	void Reset();

	// GPU data read port $1F801810
	// Read responses to GP0(C0h) and GP1(10h) commands
	u32 GetGPUREAD();

	// GPU status register read port $1F801814
	// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-status-register
	u32 GetGPUSTAT();

	// GP0 command write port $1F801810
	// Send GP0 Commands/Packets. Rendering and VRAM Access.
	// VRAM data port with some commands.
	void WriteGP0(u32 val);

	// GP1 command write port $1F801814
	// Display control.
	// GP1 commands are send directly to the GPU i.e. they do not go through the command FIFO.
	void WriteGP1(u32 val);

	const u8* GetVRAM() const { return m_vram; }

	// Returns cached value decoded from GPUSTAT
	// One of: 256, 320, 368, 512, 640
	unsigned int GetHorizontalResolution() const { return m_horizontalResolution; }

	// Decodes vertical resolution encoded in GPUSTAT
	unsigned int GetVerticalResolution() const;

	// GP1(05h) start X
	// In halfwords, which at 16 bpp is pixels
	unsigned int GetDisplayStartX() const { return m_displayStartX; }

	// GP1(05h) start Y
	// In scanlines
	unsigned int GetDisplayStartY() const { return m_displayStartY; }

	// Set by GP1(06h)
	unsigned int GetDisplayRangeX1() const { return m_displayRangeX1; }
	unsigned int GetDisplayRangeX2() const { return m_displayRangeX2; }

	// Set by GP1(07h)
	// The number of lines is Y2-Y1
	unsigned int GetDisplayRangeY1() const { return m_displayRangeY1; }
	unsigned int GetDisplayRangeY2() const { return m_displayRangeY2; }

	DisplayFormat GetDisplayFormat() const { return m_gpustat.displayFormat; }

	const Stats& GetStats() const { return m_stats; }

private:

	enum class Command
	{
		None,
		FillRectangle,
		VramToVramBlit,
		CpuToVramBlit,
		VramToCpuBlit,
		RenderPolygon,
		RenderLine,
		RenderRectangle,
	};

	struct FillRectangle
	{
		u16 pixel = 0; // B5G5R5 #TODO: Store semi-transparent bit in MSb?
		u32 x = 0; // src or dst coord
		u32 y = 0;
	};

	// Params and state for memory transfer commands:
	// - VRAM to VRAM blit (GP0 command 4)
	// - CPU to VRAM transfer (GP0 command 5)
	// - VRAM to CPU transfer (GP0 command 6)
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-memory-transfer-commands
	//
	struct MemoryTransfer
	{
		// Params
		u32 srcX = 0;
		u32 srcY = 0;
		u32 dstX = 0;
		u32 dstY = 0;
		u32 width = 0;
		u32 height = 0;

		// State
		u32 col = 0; // offset from x
		u32 row = 0; // offset from y
	};

	enum class VertexAttribute
	{
		Colour,
		Position,
		TexCoord,
	};

	// Full-fat vertex
	struct Vertex
	{
		// Position
		int x = 0;
		int y = 0;

		// Colour
		u32 colourB8G8R8 = 0; // Store as 8-bit throughout pipeline to improve accuracy. Will be converted to 5 bit for write to VRAM display format.

		// Texture coords
		unsigned int u = 0; // 8-bit unsigned
		unsigned int v = 0;
	};

	// Polygon draw state
	struct Polygon
	{
		ShadingMode shadingMode = ShadingMode::Flat;
		bool quad = false; // if false then triangle
		bool textured = false;
		bool semitransparent = false; // if false then opaque
		bool rawTexture = false; // false: texture sample is multiplied (modulated) by interpolated vertex colour. true: use unmodified "raw" texture colour.
		Vertex vertices[4]; // max 4 vertices for quad
		unsigned int vertexIndex = 0;
		VertexAttribute vertexAttribute = VertexAttribute::Colour;
	};

	// Line draw state
	struct Line
	{
		ShadingMode shadingMode = ShadingMode::Flat;
		bool polyline = false;
		bool semitransparent = false; // if false then opaque
		u32 colourB8G8R8 = 0; // Store as 8-bit throughout pipeline to improve accuracy. Will be converted to 5 bit for write to VRAM display format.
		u32 colourB8G8R8_prev = 0; // Two colours are used for gouraud shading
		int x = 0; // [-1024,1023]
		int y = 0; // [-1024,1023]
		unsigned int vertexIndex = 0;
		VertexAttribute vertexAttribute = VertexAttribute::Colour;
	};

	// Rectangle draw state
	struct Rectangle
	{
		RectangleSize size = RectangleSize::Variable;
		int x = 0; // left coord [-1024,1023]
		int y = 0; // top coord [-1024,1023]
		bool textured = false;
		bool semitransparent = false;
		bool rawTexture = false; // false: texture sample is multiplied by interpolated vertex colour. true: use unmodified "raw" texture colour.
		u32 vertexB8G8R8 = 0; // Store as 8-bit throughout pipeline to improve accuracy. Will be converted to 5 bit for write to VRAM display format.

		unsigned int u = 0; // 8-bit unsigned texture coords
		unsigned int v = 0;
	};


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#endif
	/*
	GP0(E2h) - Texture Window setting

    0-4    Texture window Mask X   (in 8 pixel steps)
    5-9    Texture window Mask Y   (in 8 pixel steps)
    10-14  Texture window Offset X (in 8 pixel steps)
    15-19  Texture window Offset Y (in 8 pixel steps)
    20-23  Not used (zero)
    24-31  Command  (E2h)

	Source: https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e2h-texture-window-setting
	*/
	struct TextureWindow
	{
		union {
			u32 val;

			// n.b. Bits are ordered from least to most significant
			struct {
				u32 maskX : 5; // Texture window Mask X (in 8-pixel units) GPUSTAT.4:0  
				u32 maskY : 5; // Texture window Mask Y (in 8-pixel units) GPUSTAT.9:5
				u32 offsetX : 5; // Texture window Offset X (in 8-pixel units) GPUSTAT.14:10
				u32 offsetY : 5; // Texture window Offset Y (in 8-pixel units) Bits 19:15
				u32 unusedBits31_20 : 12; // zero. Don't store the command that was issued.
			};
		};
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	void parseGP0Command(u32 val);
	void parseGP0MiscCommand(u32 val);
	void parseGP0EnvironmentCommand(u32 val);

	void endCommand();

	void resetGPU(); // GP1(00h) - Reset GPU
	void resetCommandBuffer(); // GP1(01h) - Reset Command Buffer
	void parseGPUREAD(u32 val); // GP1(10h) - GPUREAD

	// GP0(02h) - Fill VRAM Rectangle
	void parseFillRectangleWord(u32 val);

	// Memory Transfer Commands
	void parseVramToVramBlitWord(u32 val); // VRAM to VRAM blitting - command 4 (100)

	void parseCpuToVramBlitWord(u32 val); // CPU to VRAM blitting - command 5 (101)

	void parseVramToCpuBlitWord(u32 val); // VRAM to CPU blitting - command 6 (110)
	u32 updateVramToCpuBlitOutput();

	// #TODO: VRAM to VRAM blit command parsing

	void resetMemoryTransferState();

	// Polygon primitive
	void parseDrawPolygonWord(u32 val);

	template<bool gouraud, bool textured, bool dither> void drawTriangle(const Vertex& v0, Vertex v1, Vertex v2, bool semitransparent, bool modulate);
	template<bool gouraud, bool textured, bool dither> void drawQuad(const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3, bool semitransparent, bool modulate);

	bool shouldDitherPolygon() const;
	bool shouldDitherLine() const;

	void decodePolygonTexPageUVParameter(u32 val, /*out*/unsigned int& u, /*out*/unsigned int& v);
	void decodePrimitiveClutUVParameter(u32 val, /*out*/unsigned int& u, /*out*/unsigned int& v);

	// Line primitive
	void parseDrawLineWord(u32 val);

	template<bool dither>
	void drawLineFlat(int x1, int y1, int x2, int y2, u32 colourB8G8R8, bool semitransparent);

	template<bool dither>
	void drawLineGouraud(int x1, int y1, int x2, int y2, u32 colourB8G8R8_1, u32 colourB8G8R8_2, bool semitransparent);

	// Rectangle primitive
	void parseDrawRectangleWord(u32 val);
	void drawUntexturedRectangle(int x, int y, unsigned int w, unsigned int h, u32 colourB8G8R8, bool semitransparent);
	void drawTexturedRectangle(int x, int y, unsigned int w, unsigned int h, u32 vertexColourB8G8R8, bool semitransparent);

	u16 blend(unsigned int srcR5, unsigned int srcG5, unsigned int srcB5, unsigned int vramOffset) const;

	template<bool dither>
	void drawPixelB8G8R8(unsigned int x, unsigned int y, u32 colorB8G8R8, bool semitransparent);

	void drawPixelA1B5G5R5(unsigned int x, unsigned int y, u16 colorA1B5G5R5, bool semitransparent);

	void updateTexturePageAddress();
	u16 sampleTexture(unsigned int u, unsigned int v) const;

	u32 m_gpuread = 0;

	StatusRegister m_gpustat{};

	// GPUSTAT derived values to avoid repeated recalculations
	unsigned int m_horizontalResolution = 320; // GP1(08h) display mode 320x200 NTSC(0)

	u8* m_vram = nullptr; // 1 MB VRAM

	// Command state
	Command m_currentCommand = Command::None;
	unsigned int m_commandWordCount = 0;
	FillRectangle m_fillRectangle;
	MemoryTransfer m_memoryTransfer;
	Polygon m_polygon;
	Line m_line;
	Rectangle m_rectangle;

	// aka Drawing Area
	// Set by GP0(E3h) and GP0(E4h)
	// Used to cull pixels in render commands GP0(20h..7Fh)
	// Note that the values are inclusive e.g. (0,0) to (319,239) for a 320x240 area

	// Raw register values for GPUREAD
	u32 m_drawAreaTopLeft = 0;
	u32 m_drawAreaBottomRight = 0;

	// Derived values for convenience
	struct ScissorRect
	{
		unsigned int left = 0; // X1 10-bits [0,1023]
		unsigned int top = 0; // Y1 9-bits [0,511]
		unsigned int right = 0; // X2 10-bits [0,1023]
		unsigned int bottom = 0; // Y2 9-bits [0,511]
	} m_scissorRect;

	// Set by GP0(E5h)
	// Used in render commands GP0(20h..7Fh)
	u32 m_drawingOffset = 0; // raw reg value for GPUREAD
	int m_drawingOffsetX = 0; // [-1024, +1023]
	int m_drawingOffsetY = 0; // [-1024, +1023]

	// Used by both polygon and rectangle primitives, so as store global state.
	unsigned int m_clutX_halfwords = 0; // CLUT X in halfwords (16 pixels) (really just for debugging)
	unsigned int m_clutY = 0; // CLUT Y in pixels (really just for debugging)
	u32 m_clutAddr = 0;

	// Cached value to avoid repeated per-sample recalculation.
	// Derived from GPUSTAT fields.
	u32 m_texturePageAddr = 0;

	TextureWindow m_textureWindow{};
	struct TextureWindowPrecomp
	{
		// Skip texture window application if zero
		bool zero = true;

		// Precomputed terms for applying per pixel texcoord masking.
		unsigned int uAND = 0;
		unsigned int uOR = 0;
		unsigned int vAND = 0;
		unsigned int vOR = 0;
	} m_textureWindowPrecomp;

	// For rectangle primitive drawing only
	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#texture-origin-and-xy-flip
	bool m_rectangleTextureFlipX = false;
	bool m_rectangleTextureFlipY = false;

	// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e6h-mask-bit-setting
	struct MaskSetting
	{
		bool forceSet = false; // GP0(E6h).0  0 = set from texture bit 15, 1 = always set
		bool testMask = false; // GP0(E6h).1  0 = always draw, 1 = draw only if bit 15 is *not* set
	} m_maskSetting;

	// Set by GP1(05h)
	unsigned int m_displayStartX = 0; // halfword address in VRAM, relative to begin of VRAM
	unsigned int m_displayStartY = 0; // scanline number in VRAM

	// Set by GP1(06h)
	unsigned int m_displayRangeX1 = 0; // left edge pixel coordinate in GPU clock units; $260 / 608 is roughly the left edge of the screen)
	unsigned int m_displayRangeX2 = 0; // right edge pixel coordinate

	// Set by GP1(07h)
	unsigned int m_displayRangeY1 = 0; // top edge scanline coordinate; screen top/bottom values vary between NTSC and PAL modes
	unsigned int m_displayRangeY2 = 0; // bottom edge scanline coordinate

	Stats m_stats;
};

inline bool s_logGP0 = false;
inline bool s_logGP1 = false;
inline bool s_logGPUREAD = false;
inline bool s_logGPUSTAT = false;
inline bool s_logUnimplementedGpuFeatures = false;
inline bool s_logInvalidCommands = false;
