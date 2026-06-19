#pragma once

// This is an expensive system include which can increase compile time, so only include this file if required.
//#include <type_traits> // underlying_type_t

#define PI 3.14159265359f
#define TWOPI 6.28318530718f

// Helper macro to return the number of elements in an an enum class iff has 'Max' enumeration
// which is must be set to the last element. For example:
//
//   enum class State
//   {
//       Walking 
//		 Talking,
//       Shooting,
//
//       Max = Shooting
//   };
//
// This helps to catch missing case statements in switch statements on enumerations at compile time
// instead of run time (for example with MSVC C4062: enumerator 'identifier' in switch of enum 
// 'enumeration' is not handled) and avoids the need for default: FATAL_ERROR("Unhandled case") everywhere.
//
// n.b. Adding Count as last value is not sufficient, because would have unique value and would need
// to be handled in every switch statement.
//
// Unfortunately this introduces the opportunity for human error if a new Enum field is added to the end 
// and Max is not updated. For enums that will never require a switch statement, using the 'Count' method
// may be more robust.
//
#define ENUM_COUNT(EnumType) ((unsigned int)(EnumType::Max) + 1)

#define HP_UNUSED(X)	(void)X

#define FOURCC(a,b,c,d) ( (uint32_t) (((d)<<24) | ((c)<<16) | ((b)<<8) | (a)) )

template<typename T>
inline void Swap(T& a, T& b)
{
	T temp = a;
	a = b;
	b = temp;
}

#if 0 // Disabled due to expensive include. Just use (int)e where needed.
// Convert enum to underlying type
template <typename E>
constexpr auto ToNumber(E e) noexcept
{
	return static_cast<std::underlying_type_t<E>>(e);
}
#endif

// Only call with value != 0
// Prefer to use this macro over function call so is "inlined" in Debug builds (MSVC /Ob0) to save function call overheads (they add up).
#if defined(__GNUC__) || defined(__clang__)
#define COUNT_TRAILING_ZEROS(v) (unsigned int)__builtin_ctz(v)
#define COUNT_LEADING_ZEROS(v) (unsigned int)__builtin_clz(v)
#elif defined(_MSC_VER)
// #include #include <intrin.h>  // Deliberately commented out to avoid expensive system include in header. Include in .cpp file where used.
#define COUNT_TRAILING_ZEROS(v) (unsigned int)_tzcnt_u32(v)
#define COUNT_LEADING_ZEROS(v) (unsigned int)_lzcnt_u32(v)
#else
#error "Unsupported compiler"
#endif

template <typename T>
constexpr T resetLowestSetBit(T v)
{
	return v & (v - 1);
}
