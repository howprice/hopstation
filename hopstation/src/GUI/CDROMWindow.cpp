#include "CDROMWindow.h"

#include "psx/CDROM.h"
#include "psx/CD.h"

#include "core/RingBuffer.h"
#include "core/StringHelpers.h"

#include "imgui.h"

bool CDROMWindow::s_visible = false;

static void showMode(const CDROM& cdrom)
{
	const CDROM::Mode& mode = cdrom.GetMode();

//	ImGui::Text("Mode: %02X", mode.val); // redundant - shown in collapsing header

	if (ImGui::BeginTable("ModeTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Speed");
		ImGui::TableNextColumn();
		ImGui::Text("%s", mode.doubleSpeed ? "Double" : "Normal");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("XA-ADPCM");
		ImGui::TableNextColumn();
		ImGui::Text("%u", mode.XA_ADPCM);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Sector Size");
		ImGui::TableNextColumn();
		ImGui::Text("%u = %s", mode.SectorSize, mode.SectorSize == 0 ? "800h (2048) bytes, DataOnly" : "924h (2340) bytes, Whole Sector Except Sync Bytes");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Ignore");
		ImGui::TableNextColumn();
		ImGui::Text("%u %s", mode.IgnoreBit, mode.IgnoreBit == 0 ? "" : "Ignore Sector Size and Setloc position");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("XA-Filter");
		ImGui::TableNextColumn();
		if (mode.XA_Filter)
			ImGui::Text("%u File: %u Channel: %u", mode.XA_Filter, cdrom.GetXAFilterFileNumber(), cdrom.GetXAFilterChannelNumber());
		else
			ImGui::Text("%u", mode.XA_Filter);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Report");
		ImGui::TableNextColumn();
		ImGui::Text("%u", mode.Report);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("AutoPause");
		ImGui::TableNextColumn();
		ImGui::Text("%u", mode.AutoPause);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("CDDA");
		ImGui::TableNextColumn();
		ImGui::Text("%u", mode.CDDA);
		ImGui::EndTable();
	}
}

static void showStat(const CDROM::Stat& stat)
{
//	ImGui::Text("Stat: %02X", stat.val);  // redundant - shown in collapsing header

	if (ImGui::BeginTable("StatTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Play");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.Play);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Seek");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.Seek);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Read");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.Read);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Shell Open");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.ShellOpen);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("ID Error");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.IdError);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Seek Error");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.SeekError);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Spindle Motor");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.SpindleMotor);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Error");
		ImGui::TableNextColumn();
		ImGui::Text("%u", stat.Error);

		ImGui::EndTable();
	}
}

void CDROMWindow::Update(const CDROM& cdrom)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("CDROM", &s_visible))
	{
		unsigned int m, s, f;
		u32 headLBA = cdrom.GetHeadLBA();
		CD::LBAtoMSF(headLBA, m, s, f);
		ImGui::Text("Head MSF: %02u:%02u:%02u LBA: 0x%08X = %u\n", m, s, f, headLBA, headLBA); // MSF conventionally printed in decimal

		char label[32];
		SafeSnprintf(label, sizeof(label), "Mode %02X###Mode", cdrom.GetMode().val);
		if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
			showMode(cdrom);

		SafeSnprintf(label, sizeof(label), "Stat %02X###Stat", cdrom.GetStat().val);
		if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
			showStat(cdrom.GetStat());

		bool xaMuted = cdrom.IsXAMuted();
		ImGui::Checkbox("XA-ADPCM Muted", &xaMuted);

		if (ImGui::CollapsingHeader("CD volume levels", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("CDVolumesTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("Left->Left");
				ImGui::TableNextColumn();
				ImGui::Text("%u", cdrom.GetCDLeftLeftVolume());

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("Left->Right");
				ImGui::TableNextColumn();
				ImGui::Text("%u", cdrom.GetCDLeftRightVolume());

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("Right->Right");
				ImGui::TableNextColumn();
				ImGui::Text("%u", cdrom.GetCDRightRightVolume());

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("Right->Left");
				ImGui::TableNextColumn();
				ImGui::Text("%u", cdrom.GetCDRightLeftVolume());

				ImGui::EndTable();
			}
		}

		// There's not much value in showing this. A high watermark might be better.
//		ImGui::Text("XA-ADPCM buffer: Used: %08X  Free: %08X", cdrom.GetCDXABuffer().GetUsedSpaceBytes(), cdrom.GetCDXABuffer().GetFreeSpaceBytes());

		ImGui::End();
	}
}
