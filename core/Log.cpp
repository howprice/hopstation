// Adapted from https://github.com/amigadev/modpack

#include "Log.h"

#ifndef _MSC_VER
#include "core/StringHelpers.h" // _vscprintf
#endif

#include "core/hp_assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> // malloc, free

#ifdef _MSC_VER
#include <Windows.h>  // OutputDebugString
#endif

static int s_logLevel = LOG_LEVEL_INFO;
static LogCallback s_pLogCallback = nullptr;

void SetLogLevel(int logLevel)
{
	HP_ASSERT(logLevel >= LOG_LEVEL_MIN && logLevel <= LOG_LEVEL_MAX);
	s_logLevel = logLevel;
}

void SetLogCallback(LogCallback pCallback)
{
	s_pLogCallback = pCallback;
}

int GetLogLevel()
{
	return s_logLevel;
}

void LogMsgV(int logLevel, FILE* pStream, const char* format, va_list argList)
{
	HP_ASSERT(pStream != nullptr);
	HP_ASSERT(format != nullptr);
//	HP_ASSERT(argList != nullptr); // Raspberry Pi error: invalid operands of types ‘va_list’ and ‘std::nullptr_t’ to binary ‘operator!=’

	// n.b. Can't re-use a va_list. Need to make copies each time it is used.
	// Undefined behaviour otherwise. OK on Windows, but segfaults on Linux.
	va_list argcopy; 
	va_copy(argcopy, argList);
	vfprintf(pStream, format, argcopy); // print to stdout
	va_end(argcopy); 

	if (s_pLogCallback)
	{
		va_copy(argcopy, argList);
		s_pLogCallback(logLevel, format, argcopy);
		va_end(argcopy); 
	}
	
	// Send string to debugger (Visual Studio Output window) for convenience
	va_copy(argcopy, argList);
	int bufferSize = _vscprintf(format, argcopy) + 1; // + 1 to null terminate (_vscprintf return value doesn't include null-terminator)
	va_end(argcopy); 

	// Depending on the size of the format string, allocate space on the stack or the heap.
	char* debugString = (char*)malloc(bufferSize);

	// Populate the buffer with the contents of the format string.
	va_copy(argcopy, argList); 
	vsnprintf(debugString, bufferSize, format, argcopy);
	va_end(argcopy); 

#ifdef _MSC_VER
	OutputDebugString(debugString);
#else
	// #TODO: Use syslog() on linux?
#endif

	free(debugString);
}

void LogMsg(FILE* pStream, const char* format, ...)
{
	HP_ASSERT(pStream != nullptr);

	va_list argList;
	va_start(argList, format);
	LogMsgV(LOG_LEVEL_NONE, pStream, format, argList);
	va_end(argList);
}

void LogLevel(int logLevel, const char* format, ...)
{
	if (logLevel > s_logLevel)
		return;

	// LOG_ERROR and LOG_WARN go to stderr. LOG_LEVEL_INFO, LOG_LEVEL_DEBUG and LOG_LEVEL_TRACE go to stdout
	FILE* pStream = logLevel < LOG_LEVEL_INFO ? stderr : stdout;

	va_list argList;
	va_start(argList, format);
	LogMsgV(logLevel, pStream, format, argList);
	va_end(argList);
}

void LogLevelV(int logLevel, const char* format, va_list argList)
{
	if (logLevel > s_logLevel)
		return;

	// LOG_ERROR and LOG_WARN go to stderr. LOG_LEVEL_INFO, LOG_LEVEL_DEBUG and LOG_LEVEL_TRACE go to stdout
	FILE* pStream = logLevel < LOG_LEVEL_INFO ? stderr : stdout;

	LogMsgV(logLevel, pStream, format, argList);
}

void Log(bool verbose, const char* format, ...)
{
	int logLevel = verbose ? LOG_LEVEL_INFO : LOG_LEVEL_TRACE;
	va_list args;
	va_start(args, format);
	LogLevelV(logLevel, format, args);
	va_end(args);
}
