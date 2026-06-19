#include "DMAWindow.h"

#include "psx/DMA.h"

#include "imgui.h"

#include <inttypes.h> // PRIu64

bool DMAWindow::s_visible = false;

static DMAC::Stats m_statsPrev{}; // for calculating deltas

static void showUI(const DMAC& dmac)
{
	if (ImGui::BeginTable("StatsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		const DMAC::Stats& stats = dmac.GetStats();
		ImGui::TableSetupColumn("");
		ImGui::TableSetupColumn("");
		ImGui::TableSetupColumn("Transfers");
		ImGui::TableHeadersRow();

		for (unsigned int channelIndex = 0; channelIndex < ENUM_COUNT(DMAChannel); channelIndex++)
		{
			ImGui::TableNextRow();

			// Highlight channels with new transfers since last frame
			if (stats.transferCount[channelIndex] > m_statsPrev.transferCount[channelIndex])
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 255, 0, 50)); // semi-transparent green

			ImGui::TableNextColumn();
			ImGui::Text("DMA%u", channelIndex);

			ImGui::TableNextColumn();
			ImGui::Text("%s", kDMAChannelNames[channelIndex]);

			ImGui::TableNextColumn();
			ImGui::Text("%" PRIu64, stats.transferCount[channelIndex]);
		}

		ImGui::EndTable();
	}
}

void DMAWindow::Update(const DMAC& dmac)
{
	if (s_visible)
	{
		if (ImGui::Begin("DMA", &s_visible))
		{
			showUI(dmac);
			ImGui::End();
		}
	}

	m_statsPrev = dmac.GetStats();
}
