#include "ImGuiDemoWindow.h"

#include "imgui.h"

void ImGuiDemoWindow::Update()
{
	// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
	if (s_showDemoWindow)
		ImGui::ShowDemoWindow(&s_showDemoWindow);

	if (s_showAboutWindow)
		ImGui::ShowAboutWindow(&s_showAboutWindow);
}
