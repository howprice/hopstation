#pragma once

#include "Types.h"

#define ALIGN(x, a)          ALIGN_MASK(x, (a)-1)
#define ALIGN_MASK(x, mask)  (((x)+(mask)) & ~(mask))

// https://stackoverflow.com/a/600306
#define IS_POWER_OF_2(x) (((x) & ((x) - 1)) == 0)
inline constexpr bool IsPowerOfTwo(unsigned int v) { return v != 0 && (v & (v - 1)) == 0; }

// https://en.wikipedia.org/wiki/Data_structure_alignment
// If alignment is a power of two then:
//
// padding = (align - (offset & (align - 1))) & (align - 1)
//         = -offset & (align - 1)
// aligned = (offset + (align - 1)) & ~(align - 1)    [1]
//         = (offset + (align - 1)) & -align          [2]
//
// n.b. Because this will often be called with unsigned y, we prefer form [1] to avoid
// compiler warning C4146: unary minus operator applied to unsigned type, result still unsigned
#define ROUND_UP_POWER_OF_TWO(x,y) (((x) + ((y) - 1)) & (~((y) - 1))) // y must be a power of 2

inline constexpr unsigned int RoundUpToNextPowerOf2(unsigned int v)
{
	if (v == 0) return 1;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

template<typename T>
inline constexpr T Min(const T a, const T b)
{
	return (a < b) ? a : b;
}

template<typename T>
inline constexpr T Min3(const T a, const T b, const T c)
{
	return (a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c);
}

template<typename T>
inline constexpr T Max(const T a, const T b)
{
	return (a > b) ? a : b;
}

template<typename T>
inline constexpr T Max3(const T a, const T b, const T c)
{
	return (a > b) ? ((a > c) ? a : c) : ((b > c) ? b : c);
}

template<typename T>
inline constexpr T Clamp(const T v, const T min, const T max)
{
	return (v < min) ? min : (v > max) ? max : v;
}

#define HP_CLAMP(v, min, max) ((v < min) ? min : (v > max) ? max : v)

template<typename T>
inline constexpr T Lerp(T& a, T& b, float t)
{
	return (T)(a + (b - a) * t);
}

inline constexpr float Floor(float t)
{
	return (float)(int)t;
}

inline constexpr float Frac(float t)
{
	return t - Floor(t);
}

inline constexpr s32 SignExtend11BitTo32Bit(u32 value)
{
#if 0
	if (value & 0x400) // Check sign bit (bit 10)
	{
		return (s32)(value | 0xfffff800); // Extend sign bit
	}
	else
	{
		return (s32)(value & 0x7ff); // Positive value
	}
#else
	// Shift left to move bit 10 to bit 31 (sign bit position)
	// Then arithmetic shift right to sign-extend
	return (s32)(value << 21) >> 21;
#endif
}
