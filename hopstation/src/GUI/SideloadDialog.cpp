#include "SideloadDialog.h"

#include "Host.h"

#include "psx-utils/Sideload.h"

#include "psx/Bus.h"

#include "core/Log.h"
#include "core/StringHelpers.h"

#include <SDL3/SDL_dialog.h>

#include <mutex>

// Thread-safe pending save
struct PendingLoad
{
	bool pending = false;
	char path[256]{};
};

static PendingLoad s_pendingLoad;
static std::mutex s_pendingLoadMutex;

void SideloadDialog::Update()
{
	std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
	if (s_pendingLoad.pending)
	{
		Host::ResetEmulator();
		if (!Sideload::SideloadExecutable(s_pendingLoad.path, Host::GetBus(), &Host::GetTTYLogger()))
		{
			LOG_ERROR("Failed to sideload executable\n");
		}
		s_pendingLoad.pending = false;
		s_pendingLoad.path[0] = '\0';
	}
}

static void dialogFileCallback(void* /*userdata*/, const char* const* filelist, int /*filter*/)
{
	// n.b. This callback may be invoked from a different thread, so can't modify global state directly here.
	if (filelist == nullptr)
	{
		LOG_ERROR("SDL_ShowOpenFileDialog() error: %s\n", SDL_GetError());
		return;
	}
	const char* filename = filelist[0];
	if (filename == nullptr)
		return; // the user either didn't choose any file or cancelled the dialog.

	std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
	SafeStrcpy(s_pendingLoad.path, sizeof(s_pendingLoad.path), filename);
	s_pendingLoad.pending = true;
}

bool SideloadDialog::ShowOpenFileDialog(SDL_Window* pWindow)
{
	{
		std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
		if (s_pendingLoad.pending)
		{
			LOG_WARN("Cannot open dialog while operation pending\n");
			return false;
		}
	}

	constexpr static SDL_DialogFileFilter kFilters[] =
	{
		{"All Files (*)", "*"}
	};

	// Async function: returns immediately
	SDL_ShowOpenFileDialog(
		dialogFileCallback, /*pUserData*/nullptr,
		pWindow, // the window that the dialog should be modal for, may be NULL.
		kFilters, COUNTOF_ARRAY(kFilters),
		/*default_location*/nullptr, // #TODO: Set default location
		/*allow_many*/false);

	return true;
}
