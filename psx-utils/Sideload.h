#pragma once

#include "core/ClassHelpers.h"

class Bus;
class TTYLogger;

//
// PSX executable sideloading
// This allows test programs to be run before the CDROM is implemented i.e. Amidog's CPU test exes.
// https://jsgroth.dev/blog/posts/ps1-sideloading/#exe-sideloading
//
class Sideload
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(Sideload);

	// Steps the bus until BIOS advances PC to correct location for sideloading.
	//
	// Implementation note: More performant to perform this in a separate function because this involves a per-instruction PC check.
	//
	// pLogger can be null
	//
	static bool SideloadExecutable(const char* path, Bus& bus, TTYLogger* pTTYLogger);

	// Enables debug output from Amidog tests.
	// Only tested with psxtest_gte.exe
	static void EnableAmidogTestDebugOutput(Bus& bus);
};
