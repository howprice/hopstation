#include "SnapshotDialog.h"

#include "psx-utils/Snapshot.h"

#include "psx/GPU.h"

#include "core/Log.h"
#include "core/StringHelpers.h"

#include <SDL3/SDL_dialog.h>

#include <mutex>

// Thread-safe pending save
struct PendingSave
{
	bool pending = false;
	char filename[256]{};
	unsigned int x = 0;
	unsigned int y = 0;
	unsigned int w = 0;
	unsigned int h = 0;
	DisplayFormat format = DisplayFormat::A1B5G5R5;
};

static PendingSave s_pendingSave;
static std::mutex s_pendingSaveMutex;

void SnapshotDialog::Update(const GPU& gpu)
{
	std::lock_guard<std::mutex> lock(s_pendingSaveMutex);
	if (s_pendingSave.pending)
	{
		if (!Snapshot::SaveVramRectAsPPM(gpu.GetVRAM(), s_pendingSave.filename, s_pendingSave.x, s_pendingSave.y, s_pendingSave.w, s_pendingSave.h, s_pendingSave.format))
		{
			LOG_ERROR("Failed to save snapshot\n");
		}
		s_pendingSave.pending = false;
		s_pendingSave.filename[0] = '\0';
	}
}

static void dialogFileCallback(void* /*userdata*/, const char* const* filelist, int /*filter*/)
{
	// n.b. This callback may be invoked from a different thread, so can't modify global state directly here.
	if (filelist == nullptr)
	{
		LOG_ERROR("SDL_ShowSaveFileDialog() error: %s\n", SDL_GetError());
		return;
	}
	const char* filename = filelist[0];
	if (filename == nullptr)
		return; // the user either didn't choose any file or cancelled the dialog.

	std::lock_guard<std::mutex> lock(s_pendingSaveMutex);
	SafeStrcpy(s_pendingSave.filename, sizeof(s_pendingSave.filename), filename);
	s_pendingSave.pending = true;
}

bool SnapshotDialog::ShowSaveFileDialog(SDL_Window* pWindow, unsigned int x, unsigned int y, unsigned int w, unsigned int h, DisplayFormat format)
{
	{
		std::lock_guard<std::mutex> lock(s_pendingSaveMutex);
		if (s_pendingSave.pending)
		{
			LOG_WARN("Cannot open save dialog: a save operation is already pending\n");
			return false;
		}

		s_pendingSave.x = x;
		s_pendingSave.y = y;
		s_pendingSave.w = w;
		s_pendingSave.h = h;
		s_pendingSave.format = format;
	}

	constexpr static SDL_DialogFileFilter kFilters[] =
	{
		{"PPM Files (*.ppm)", "ppm"},
		{"All Files (*)", "*"}
	};

	// Async function: returns immediately
	SDL_ShowSaveFileDialog(
		dialogFileCallback, /*pUserData*/nullptr,
		pWindow, // the window that the dialog should be modal for, may be NULL.
		kFilters, COUNTOF_ARRAY(kFilters),
		/*default_location*/nullptr // #TODO: Set default location
	);

	return true;
}
