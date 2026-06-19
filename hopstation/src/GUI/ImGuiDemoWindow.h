#pragma once

#include "core/ClassHelpers.h"

struct ImVec4;

class ImGuiDemoWindow
{
public:
	NON_INSTANTIABLE_STATIC_CLASS(ImGuiDemoWindow);

	// Call once per frame
	static void Update();

	static inline bool s_showDemoWindow = false;
	static inline bool s_showAboutWindow = false;
};
