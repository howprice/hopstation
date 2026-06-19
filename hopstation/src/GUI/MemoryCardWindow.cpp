#include "MemoryCardWindow.h"

#include "psx/SIO.h"

#include "core/StringHelpers.h"

#include "imgui.h"

bool MemoryCardWindow::s_visible = false;

// Directory Frames (Block 0, Frame 1..15)
// 
//  00h-03h Block Allocation State
//            00000051h - In use ;first-or-only block of a file
//            00000052h - In use ;middle block of a file (if 3 or more blocks)
//            00000053h - In use ;last block of a file   (if 2 or more blocks)
//            000000A0h - Free   ;freshly formatted
//            000000A1h - Free   ;deleted (first-or-only block of file)
//            000000A2h - Free   ;deleted (middle block of file)
//            000000A3h - Free   ;deleted (last block of file)
//  04h-07h Filesize in bytes (2000h..1E000h; in multiples of 8Kbytes)
//  08h-09h Pointer to the NEXT block number (minus 1) used by the file
//            (ie. 0..14 for Block Number 1..15) (or FFFFh if last-or-only block)
//  0Ah-1Eh Filename in ASCII, terminated by 00h (max 20 chars, plus ending 00h)
//  1Fh     Zero (unused)
//  20h-7Eh Garbage (usually 00h-filled)
//  7Fh     Checksum (all above bytes XORed with each other)
// 
// https://psx-spx.consoledev.net/controllersandmemorycards/#directory-frames-block-0-frame-115
//
static void showDirectory(const MemoryCard& memoryCard)
{
	if (ImGui::BeginTable("DirectoryTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableSetupColumn("Sector");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Size");
		ImGui::TableSetupColumn("Next Block");
		ImGui::TableSetupColumn("Filename");
		ImGui::TableSetupColumn("Checksum");
		ImGui::TableHeadersRow();

		const u8* data = memoryCard.GetData();
		for (unsigned int sectorIndex = 1; sectorIndex <= 15; sectorIndex++)  // Skip header frame (Block 0, Frame 0)
		{
			const u8* frame = data + (sectorIndex * MemoryCard::kSectorSizeBytes);
			const MemoryCard::DirectoryFrame* pDirectoryFrame = (MemoryCard::DirectoryFrame*)frame;

			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			ImGui::Text("%u", sectorIndex);
			ImGui::TableNextColumn();
			ImGui::Text("%08X", pDirectoryFrame->blockAllocationState);
			ImGui::TableNextColumn();
			ImGui::Text("%05X", pDirectoryFrame->fileSizeBytes);
			ImGui::TableNextColumn();
			ImGui::Text("%04X", pDirectoryFrame->nextBlockNumberMinus1);
			ImGui::TableNextColumn();
			ImGui::Text("%s", pDirectoryFrame->filename);
			ImGui::TableNextColumn();
			ImGui::Text("%02X", pDirectoryFrame->checksum);
		}
		ImGui::EndTable();
	}
}

static void showMemoryCard(const MemoryCard& memoryCard)
{
	ImGui::Text("State: %s", kMemoryCardStateNames[(int)memoryCard.GetState()]);
	if (ImGui::CollapsingHeader("Directory", ImGuiTreeNodeFlags_DefaultOpen))
	{
		showDirectory(memoryCard);
	}
}

void MemoryCardWindow::Update(SIO& sio)
{
	if (!s_visible)
		return;

	if (ImGui::Begin("Memory Card", &s_visible))
	{
		for (unsigned int portIndex = 0; portIndex < SIO::kNumPorts; portIndex++)
		{
			ImGui::PushID(portIndex); // CollapsingHeader does not create its own ID scope

			ControllerPort& port = sio.GetPort(portIndex);
			MemoryCard& memoryCard = port.GetMemoryCard();

			char label[32];
			unsigned int memCardNumber = 1 + portIndex; // 1-based for user friendliness
			SafeSnprintf(label, sizeof(label), "Memory Card %u", memCardNumber);
			if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (port.IsMemoryCardInserted())
				{
					showMemoryCard(memoryCard);
				}
				else
				{
					ImGui::Text("No memory card inserted");
				}
			}

			ImGui::PopID();
		}
		ImGui::End();
	}
}
