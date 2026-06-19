#pragma once

#ifndef HP_ASSERTS_ENABLED  // allow HP_ASSERTS_ENABLED to be defined on the compiler command line
#ifdef RELEASE
#define HP_ASSERTS_ENABLED 0 // Disable asserts and fatal errors in release for performance.
#elif defined DEBUG
#define HP_ASSERTS_ENABLED 1
#else
#define HP_ASSERTS_ENABLED 0
#endif
#endif

// Debug asserts are only enabled in debug builds to catch programmer errors.
// Don't want them enabled in release builds because could impact performance e.g. in performance critical code.
#ifndef HP_DEBUG_ASSERTS_ENABLED  // allow HP_DEBUG_ASSERTS_ENABLED to be defined on the compiler command line
#if defined DEBUG && HP_ASSERTS_ENABLED
#define HP_DEBUG_ASSERTS_ENABLED 1
#else
#define HP_DEBUG_ASSERTS_ENABLED 0
#endif
#endif

// #TODO: Update this to support Clang on Windows
// #TODO: Update this to support Apple silicon
#if defined _MSC_VER
#define HP_BREAK __debugbreak();
#define HP_UNREACHABLE() __assume(0); 
#elif defined __arm__ // 32bit arm, and 32bit arm only.
#define HP_BREAK __builtin_trap();
#define HP_UNREACHABLE() __builtin_unreachable();
#elif defined __aarch64__ // 64bit arm, and 64bit arm only.
#define HP_BREAK __builtin_trap();
#define HP_UNREACHABLE() __builtin_unreachable();
#elif defined __GNUC__
#define HP_BREAK __asm__ ("int $3"); \
__builtin_unreachable(); /* This is required to avoid Werror=array-bounds= in ImGui and application code of the form ASSERT(index < N); array[index]; on GCC13/Ubuntu 24.04 */
#define HP_UNREACHABLE() __builtin_unreachable();
#else
#error Unsupported compiler
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments" // error: token pasting of ',' and __VA_ARGS__ is a GNU extension [-Werror,-Wgnu-zero-variadic-macro-arguments]
#endif

void HpAssertMessage(const char* expr, const char* type, const char* file, int line, const char* func, const char* format = nullptr, ...);

#ifndef HP_ASSERTS_ENABLED
#error HP_ASSERTS_ENABLED must be defined to 0 or 1
#endif

#if HP_ASSERTS_ENABLED

// http://cnicholson.net/2009/02/stupid-c-tricks-adventures-in-assert/
#define HP_ASSERT(expr, ...) \
	do \
	{ \
		if( !(expr) ) \
		{ \
			HpAssertMessage(#expr, "ASSERT", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
		HP_BREAK \
		} \
	} while (0)

#define HP_FATAL_ERROR(...) \
	do \
	{ \
		HpAssertMessage("", "FATAL ERROR", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
		HP_BREAK \
	} while (0)

// this macro is always defined
#define HP_VERIFY(expr, ...) \
	do \
	{ \
		if( !(expr) ) \
		{ \
			HpAssertMessage(#expr, "VERIFY FAIL", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__); \
		HP_BREAK \
		} \
	} while (0)

#else

// Support macros without braces e.g.
//     if (whatever)
//         HP_ASSERT(something);

#define HP_ASSERT(expr, ...) \
	do \
	{ \
	} while (0)

#define HP_FATAL_ERROR(...) \
	do \
	{ \
	} while (0)

#define HP_VERIFY(expr, ...) \
	do \
	{ \
		(void)(expr); \
	} while (0)

#endif


#if HP_DEBUG_ASSERTS_ENABLED
#define HP_DEBUG_ASSERT(expr, ...) HP_ASSERT(expr, ##__VA_ARGS__)
#define HP_DEBUG_FATAL_ERROR(...) HP_FATAL_ERROR(__VA_ARGS__)
#else
#define HP_DEBUG_ASSERT(expr, ...) \
	do \
	{ \
	} while (0)
#define HP_DEBUG_FATAL_ERROR(...) \
	do \
	{ \
	} while (0)
#endif
