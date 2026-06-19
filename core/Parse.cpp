#include "Parse.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h> // GCC/Clang
#include <limits.h> // GCC/Clang

bool ParseInt(const char* arg, int& val)
{
	errno = 0;
	char* pEnd = NULL;
	long longVal = strtol(arg, &pEnd, 0);
	if (arg == pEnd || errno != 0 || longVal == LONG_MIN || longVal == LONG_MAX)
	{
		return false;
	}

	val = (int)longVal;
	return true;
}

bool ParseUnsignedInt(const char* arg, unsigned int& val)
{
	errno = 0;
	char* pEnd = NULL;
	long int iVal = strtol(arg, &pEnd, 0);
	if (iVal < 0 || arg == pEnd || errno != 0 || iVal == LONG_MIN || iVal == LONG_MAX)
	{
		return false;
	}
	val = (unsigned int)iVal;
	return true;
}

bool ParseHexUnsignedInt(const char* arg, unsigned int& val)
{
	errno = 0;
	char* pEnd = NULL;
	val = strtoul(arg, &pEnd, 16);
	if (arg == pEnd || errno != 0)
		return false;
	return true;
}

bool ParseFloat(const char* arg, float& val)
{
	errno = 0;
	char *pEnd = NULL;
	float fVal = strtof(arg, &pEnd);
	if (errno != 0)
	{
		return false;
	}
	val = fVal;
	return true;
}
