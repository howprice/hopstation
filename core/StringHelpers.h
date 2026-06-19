#pragma once

#include <stdarg.h> // va_list

#ifdef __GNUC__
#include <stddef.h> // size_t

int _vscprintf(const char * format, va_list pargs);

#endif

//
// Substitue for SDL_strlcat to remove dependency on SDL
// Returns the size of the *string* that would have been written if the buffer was large enough
//
size_t Strlcat(char* dst, const char* src, size_t dstSize);

//
// Asserts if the destination buffer is too small
// Use instead of SDL_strlcpy, which truncates the output without any indication
// n.b. Parameter order differs from SDL_strlcpy(char *dst, const char *src, size_t maxlen);
//
void SafeStrcpy(char* dst, size_t dstSize, const char* src);

//
// Asserts if the output was truncated
// Use instead of SDL_snprintf, which silently truncates the output.
//
bool SafeSnprintf(char* buffer, size_t bufferSize, const char* format, ...);
bool SafeVsnprintf(char* buffer, size_t bufferSize, const char* format, va_list argList);

// Shamelessly copied from imgui.cpp
constexpr char ToUpper(char c) { return (c >= 'a' && c <= 'z') ? c &= ~32 : c; }

// Platform-independent case-insensitive string compare
// Shamelessly copied from imgui.cpp
inline int Stricmp(const char* str1, const char* str2)
{
	int d;
	while ((d = ToUpper(*str2) - ToUpper(*str1)) == 0 && *str1) { str1++; str2++; }
	return d;
}