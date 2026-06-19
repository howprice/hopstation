#include "CPUWindow.h"

#include "psx/R3000.h"

#include "imgui.h"

bool CPUWindow::s_visible = false;

void CPUWindow::Update(R3000& r3000)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("CPU", &s_visible))
	{
		ImGui::Text("PC: %08X", r3000.GetPC());
		ImGui::End();
	}
}
