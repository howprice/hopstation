#pragma once

#include "core/ClassHelpers.h"

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUTexture SDL_GPUTexture;

class ImGuiWrap
{
public:

	NON_INSTANTIABLE_STATIC_CLASS(ImGuiWrap);

	static bool Init(SDL_Window* pWindow, SDL_GPUDevice* pDevice, float main_scale);
	static void Shutdown();

	static bool ProcessEvent(const SDL_Event& event);

	// Start the Dear ImGui frame
	static void NewFrame();

	static void Render(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pSwapchainTexture);
};


//
// Add helper functions to ImGui namespace
//
namespace ImGui
{
void HelpMarker(const char* fmt, ...);

void PushStyleCompact();
void PopStyleCompact();
bool SmallCheckbox(const char* label, bool* v);
};
