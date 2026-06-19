#include "MemoryCardFileDialog.h"

#include "Host.h"

#include "psx/SIO.h"
#include "psx/Bus.h"

#include "core/Log.h"
#include "core/StringHelpers.h"
#include "core/hp_assert.h"

#include <SDL3/SDL_dialog.h>

#include <mutex>

enum class PendingOperationType
{
	Load,
	Save
};

// Thread-safe pending save
struct PendingOperation
{
	bool pending = false;
	PendingOperationType type = PendingOperationType::Load;
	char path[256]{};
};

static PendingOperation s_pendingOperation;
static std::mutex s_pendingLoadMutex;
static unsigned int s_portIndex = 0;

void MemoryCardFileDialog::Update()
{
	std::lock_guard<std::mutex> lock(s_pendingLoadMutex);

	if (s_pendingOperation.pending)
	{
		ControllerPort& port = Host::GetBus().GetSIO().GetPort(s_portIndex);
		MemoryCard& memoryCard = port.GetMemoryCard();

		switch (s_pendingOperation.type)
		{
			case PendingOperationType::Load:
			{
				if (memoryCard.LoadFromFile(s_pendingOperation.path))
				{
					if (!port.IsMemoryCardInserted())
						port.SetMemoryCardInserted(true);
				}
				else
				{
					LOG_ERROR("Failed to load memory card from file: %s\n", s_pendingOperation.path);
				}
				s_pendingOperation.pending = false;
				s_pendingOperation.path[0] = '\0';
				break;
			}

			case PendingOperationType::Save:
			{
				if (!memoryCard.SaveToFile(s_pendingOperation.path))
				{
					LOG_ERROR("Failed to save memory card to file: %s\n", s_pendingOperation.path);
				}
				s_pendingOperation.pending = false;
				s_pendingOperation.path[0] = '\0';
				break;
			}
		}
	}
}

static void dialogFileCallback(void* /*userdata*/, const char* const* filelist, int /*filter*/)
{
	// n.b. This callback may be invoked from a different thread, so can't modify global state directly here.
	if (filelist == nullptr)
	{
		LOG_ERROR("SDL FileDialog error: %s\n", SDL_GetError());
		return;
	}

	const char* filename = filelist[0];
	if (filename == nullptr)
		return; // the user either didn't choose any file or cancelled the dialog.

	std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
	SafeStrcpy(s_pendingOperation.path, sizeof(s_pendingOperation.path), filename);
	s_pendingOperation.pending = true;
}

bool MemoryCardFileDialog::ShowOpenFileDialog(unsigned int portIndex, SDL_Window* pWindow)
{
	HP_ASSERT(portIndex < 2);

	{
		std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
		if (s_pendingOperation.pending)
		{
			LOG_WARN("Cannot open dialog while operation pending\n");
			return false;
		}

		s_portIndex = portIndex;
		s_pendingOperation.type = PendingOperationType::Load;
	}

	constexpr static SDL_DialogFileFilter kFilters[] =
	{
		{"Raw binary Images (*.mcd;*.mcr;*.bin)", "mcd;mcr;bin"},
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

bool MemoryCardFileDialog::ShowSaveFileDialog(unsigned int portIndex, SDL_Window* pWindow)
{
	HP_ASSERT(portIndex < 2);

	{
		std::lock_guard<std::mutex> lock(s_pendingLoadMutex);
		if (s_pendingOperation.pending)
		{
			LOG_WARN("Cannot open dialog while operation pending\n");
			return false;
		}

		s_portIndex = portIndex;
		s_pendingOperation.type = PendingOperationType::Save;
	}

	constexpr static SDL_DialogFileFilter kFilters[] =
	{
		{"Raw binary Images (*.mcd;*.mcr;*.bin)", "mcd;mcr;bin"},
		{"All Files (*)", "*"}
	};

	// Async function: returns immediately
	SDL_ShowSaveFileDialog(
		dialogFileCallback, /*pUserData*/nullptr,
		pWindow, // the window that the dialog should be modal for, may be NULL.
		kFilters, COUNTOF_ARRAY(kFilters),
		/*default_location*/nullptr); // #TODO: Set default location

	return true;
}
