#include "CDWindow.h"

#include "psx/CD.h"

#include "imgui.h"

bool CDWindow::s_visible = false;

void CDWindow::Update(const CD* pCD)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("CD", &s_visible))
	{
		if (!pCD)
		{
			ImGui::Text("No disc inserted");
			ImGui::End();
			return;
		}

		ImGui::Text("Name: %s", pCD->GetName());
		ImGui::Text("Bytes: %u", pCD->GetSizeBytes());
		ImGui::Text("Sectors: %u", pCD->GetSizeSectors());
		ImGui::Text("Tracks: %u", pCD->GetNumTracks());

		// Tracks table
		// #TODO: Add size column
		if (ImGui::BeginTable("Tracks", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Index");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Start");
			ImGui::TableSetupColumn("Length");
			ImGui::TableSetupColumn("Final");
			ImGui::TableHeadersRow();

			for (unsigned int trackIndex = 0; trackIndex < pCD->GetNumTracks(); trackIndex++)
			{
				ImGui::TableNextRow();

				// Index
				ImGui::TableNextColumn();
				ImGui::Text("%u", trackIndex);

				// Type
				ImGui::TableNextColumn();
				ImGui::Text("%s", kCDTrackDataTypeNames[(int)pCD->GetTrackDataType(trackIndex)]);

				// Start
				ImGui::TableNextColumn();
				unsigned int trackStartLBA = pCD->GetTrackStartLBA(trackIndex);
				unsigned int m, s, f;
				CD::LBAtoMSF(trackStartLBA, m, s, f);
				ImGui::Text("%02u:%02u:%02u %08X", m, s, f, trackStartLBA);

				// #TODO: Display track Length
				ImGui::TableNextColumn();
				ImGui::Text("TODO");

				// Final
				ImGui::TableNextColumn();
				unsigned int trackFinalLBA = pCD->GetTrackFinalLBA(trackIndex);
				CD::LBAtoMSF(trackFinalLBA, m, s, f);
				ImGui::Text("%02u:%02u:%02u %08X", m, s, f, trackFinalLBA);
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
}
