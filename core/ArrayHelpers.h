#pragma once

#ifndef _MSC_VER
#include <stddef.h> // GCC size_t
#endif

// http://cnicholson.net/2011/01/stupid-c-tricks-a-better-sizeof_array/
template<typename T, size_t N> constexpr char(&COUNTOF_REQUIRES_ARRAY_ARGUMENT(const T(&)[N]))[N];
#define COUNTOF_ARRAY(_x) sizeof(COUNTOF_REQUIRES_ARRAY_ARGUMENT(_x) )
