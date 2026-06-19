#include "hp_assert.h"

#include "core/Log.h"
#include "core/StringHelpers.h" // GCC _vscprintf

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h> // malloc, free

void HpAssertMessage(const char* expr, const char* type, const char* file, int line, const char* func, const char* format /*= nullptr*/, ...)
{
	unsigned int messageBufferSize = 1; // default to '\0'
	char* message = nullptr;

	if (format)
	{
		va_list argList;
		va_start(argList, format);
		messageBufferSize = _vscprintf(format, argList) * sizeof(char) + 1; // + 1 to null terminate
		va_end(argList);

		message = (char*)malloc(messageBufferSize);

		va_start(argList, format);
		vsnprintf(message, messageBufferSize, format, argList); // Populate the buffer with the contents of the format string.
		va_end(argList);
	}

	LogMsg(stderr,
		"\n"
		"------------------------------------------------------------------------------------------------------------\n"
		"%20s:\t%s\n"
		"%20s:\t%s\n"
		"%20s:\t%s\n"
		"%20s:\t%s(%d)\n"
		"------------------------------------------------------------------------------------------------------------\n\n",
		type, expr,
		"Message", message ? message : "",
		"Function", func,
		"File", file, line
	);

	if (message != nullptr)
	{
		free(message);
	}
}

