#pragma once

#include "core/ClassHelpers.h"

class R3000;

class R3000Utils
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(R3000Utils);

	static void LogState(const R3000& c);
};
