#include "GPUWindow.h"

#include "psx/GPU.h"

#include "core/StringHelpers.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <inttypes.h> // PRIu64

bool GPUWindow::s_visible = false;

static void showDisplay(const GPU& gpu)
{
	if (ImGui::BeginTable("DisplayTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("x");
		ImGui::TableNextColumn();
		ImGui::Text("%u", gpu.GetDisplayStartX());

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("y");
		ImGui::TableNextColumn();
		ImGui::Text("%u", gpu.GetDisplayStartY());

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Width");
		ImGui::TableNextColumn();
		ImGui::Text("%u", gpu.GetHorizontalResolution());

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Height");
		ImGui::TableNextColumn();
		ImGui::Text("%u", gpu.GetVerticalResolution());

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Range X1");
		ImGui::TableNextColumn();
		unsigned int displayRangeX1 = gpu.GetDisplayRangeX1();
		ImGui::Text("%u", displayRangeX1);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Range X2");
		ImGui::TableNextColumn();
		unsigned int displayRangeX2 = gpu.GetDisplayRangeX2();
		ImGui::Text("%u (X2 - X1 = %u)", displayRangeX2, displayRangeX2 - displayRangeX1);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Range Y1");
		ImGui::TableNextColumn();
		unsigned int displayRangeY1 = gpu.GetDisplayRangeY1();
		ImGui::Text("%u", displayRangeY1);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Range Y2");
		ImGui::TableNextColumn();
		unsigned int displayRangeY2 = gpu.GetDisplayRangeY2();
		ImGui::Text("%u (Y2 - Y1 = %u)", displayRangeY2, displayRangeY2 - displayRangeY1);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Format");
		ImGui::TableNextColumn();
		unsigned int displayFormatIndex = (unsigned int)gpu.GetDisplayFormat();
		ImGui::Text("%s (%u bpp)", kDisplayFormatNames[displayFormatIndex], kDisplayFormatBitsPerPixel[displayFormatIndex]);

		ImGui::EndTable();
	}
}

static void showStats(const GPU::Stats& stats)
{
	if (ImGui::BeginTable("StatsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("GP0 commmands");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64 "", stats.GP0CommandCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("GP1 commmands");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.GP1CommandCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Fill rect");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.fillRectangleCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("VRAM to VRAM blits");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.vramToVramBlitCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("CPU to VRAM blits");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.cpuToVramBlitCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("VRAM to CPU blits");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.vramToCpuBlitCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Triangles");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.triangleCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Quads");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.quadCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Lines");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.lineCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Poly lines");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.polyLineCount);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Draw Rect");
		ImGui::TableNextColumn();
		ImGui::Text("%" PRIu64, stats.drawRectangleCount);

		ImGui::EndTable();
	}
}

void GPUWindow::Update(GPU& gpu)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("GPU", &s_visible))
	{
		if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
			showDisplay(gpu);

		if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
			showStats(gpu.GetStats());

		ImGui::End();
	}
}
