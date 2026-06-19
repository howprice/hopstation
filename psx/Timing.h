// PSX timing

#pragma once

// PSX clocks
// https://psx-spx.consoledev.net/pinouts/#psxntsc (CLK Pinouts PSX/NTSC)
// Three separate oscillators:
//
// - X101: 67.7376 MHz (div2 = CPU Clock = 33.8688 MHz) (div600h = 44100 Hz audio)
// - X201: 53.69 MHz (GPU Clock) (div15 = NTSC color clock)
// - X302: 4.000 MHz (for CDROM SUB CPU)
static constexpr unsigned int kX101 = 67'737'600;
static constexpr unsigned int kCpuClock = kX101 / 2; // 33.8688 MHz exactly
static constexpr unsigned int kAudioClock = kX101 / 0x600; // 44100 Hz exactly
static constexpr unsigned int kGpuClock = 53'690'000; // 53.690 MHz
static constexpr unsigned int kNtscColorClock = kGpuClock / 15; // 3.579333 MHz

// Update video counters
// 
// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-timings
// 
// NTSC video clock = 53.693175 MHz
//
// Vertical video timings:
// - 263 (107h) scanlines per field for NTSC non-interlaced
// - 262.5 scanlines per field for NTSC interlaced
static constexpr unsigned int kVTOT = 263; // Vertical Total (scanlines) NTSC

// Vertical Refresh Rates (NTSC mode on NTSC video clock)
// - Interlaced:     59.940 Hz
// - Non-interlaced: 59.826 Hz
//
// Horizontal timings:
// - NTSC: 3413 video cycles per scanline (or 3413.6 or so?)
static constexpr unsigned int kGpuCyclesPerScanline = 3413; // Horizontal Total (video cycles per scanline)

// Docs state that hblank is 608 video clocks long.
//   GP1(06h) - Horizontal Display range (on Screen)
//     0-11   X1 (260h+0)       ;12bit       ;\counted in 53.222400MHz units,
//     12-23  X2 (260h+320*8)   ;12bit       ;/relative to HSYNC
//
// 260h = 608
// 
// See https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp106h-horizontal-display-range-on-screen

// Stenzac: NTSC active video starts on cycle 488 and ends on 3288 (this gives an Hblank of 613; close to the 608 above)
static constexpr unsigned int kHStartGpuCycles = 488;
static constexpr unsigned int kHStopGpuCycles = 3288;
static constexpr unsigned int kHBlankPeriodGpuCycles = 3413 - (kHStopGpuCycles - kHStartGpuCycles);
static_assert(kHBlankPeriodGpuCycles == 613);

// Implementation note: It is more convenient to work in CPU cycles
static constexpr unsigned int kHStartCpuCycles = (unsigned int)(((u64)kHStartGpuCycles * kCpuClock) / kGpuClock); // take care to avoid overflow
static constexpr unsigned int kHStopCpuCycles = (unsigned int)(((u64)kHStopGpuCycles * kCpuClock) / kGpuClock); // take care to avoid overflow
static constexpr unsigned int kHBlankPeriodCpuCycles = (unsigned int)(((u64)kHBlankPeriodGpuCycles * kCpuClock) / kGpuClock); // take care to avoid overflow

// Hblank does not depend on screen resolution.

static constexpr unsigned int kCpuCyclesPerScanline = (unsigned int)(((u64)kGpuCyclesPerScanline * kCpuClock) / kGpuClock); // 2152. take care to avoid overflow

static constexpr unsigned int kVblankStart = 240; // #TODO: Is it correct to always use 240 as start of vertical blanking period? Get from GPU?
static constexpr unsigned int kVblankPeriodScanlines = kVTOT - kVblankStart;
static_assert(kVblankPeriodScanlines == 23, "Expect the NTSC vertical blanking period is 23 scanlines long");

// Timer1 can use the hblank signal as input, allowing to count scanlines.
// The hblank signal is generated even during vertical blanking/retrace.
// Edge case: If display is configured to 0 pixels width this causes an endless hblank!
//
// Dotclocks (pixel clocks)
// The PSX has 6 different pixel clocks
//  - PSX.256-pix Dotclock =  5.322240MHz (44100Hz*300h*11/7/10)
//  - PSX.320-pix Dotclock =  6.652800MHz (44100Hz*300h*11/7/8)
//  - PSX.368-pix Dotclock =  7.603200MHz (44100Hz*300h*11/7/7)
//  - PSX.512-pix Dotclock = 10.644480MHz (44100Hz*300h*11/7/5)
//  - PSX.640-pix Dotclock = 13.305600MHz (44100Hz*300h*11/7/4)
//  - Namco GunCon 385-pix =  8.000000MHz (from 8.00MHz on lightgun PCB)
// #TODO: Are these for PAL or NTSC or both?
//
// Dots per scanline (NTSC):
//   256pix: 3413/10 = 341.3 dots
//   320pix: 3413/8  = 426.625 dots
//   368pix: 3413/7  = 487.5714 dots
//   512pix: 3413/5  = 682.6 dots
//   640pix: 3413/4  = 853.25 dots
//
// Timer0 can use the dotclock as input, however, the Timer0 input "ignores" the fractional portions.
// In most cases, the values are rounded down, ie. with 340.6 dots/line, the timer increments only 340 times/line;
// the only value that is rounded up is 425.75 dots/line.
// For example, due to the rounding, the timer isn't running exactly twice as fast in 512pix mode than in 256pix mode.
// The dotclock signal is generated even during horizontal/vertical blanking/retrace.
//
// Frame Rate:
//   NTSC: 53.222400MHz/263/3413 = 59.29 Hz (ie. almost 60Hz)   n.b. Video clock 
static constexpr double kRefreshRate = (double)kGpuClock / (kVTOT * kGpuCyclesPerScanline); // 59.813796 Hz

static constexpr unsigned int kCpuCyclesPerFrame = (unsigned int)(kCpuClock / kRefreshRate); // 566232
