#include "ImGuiWrap.h"

#include "core/Log.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"

bool ImGuiWrap::Init(SDL_Window* pWindow, SDL_GPUDevice* pDevice, float main_scale)
{
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
// 	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
// 	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();

	// Setup scaling
	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
	style.FontScaleDpi = main_scale;        // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)
//	io.ConfigDpiScaleFonts = true;          // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
//	io.ConfigDpiScaleViewports = true;      // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}

	// Setup Platform/Renderer backends
	if (!ImGui_ImplSDL3_InitForSDLGPU(pWindow))
	{
		LOG_ERROR("Failed to initialize ImGui SDL3 backend for SDL GPU\n");
		return false;
	}
	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = pDevice;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(pDevice, pWindow);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;                      // Only used in multi-viewports mode.
	init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;  // Only used in multi-viewports mode.
	init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
	if (!ImGui_ImplSDLGPU3_Init(&init_info))
	{
		LOG_ERROR("Failed to initialize ImGui SDL3 GPU backend\n");
		return false;
	}

	// Load Fonts
	// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
	// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
	// - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
	// - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
	// - Read 'docs/FONTS.md' for more instructions and details. If you like the default font but want it to scale better, consider using the 'ProggyVector' from the same author!
	// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
	//style.FontSizeBase = 20.0f;
	//io.Fonts->AddFontDefault();
	//io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
	//ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
	//IM_ASSERT(font != nullptr);

	return true;
}

void ImGuiWrap::Shutdown()
{
	ImGui_ImplSDL3_Shutdown();
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui::DestroyContext();
}

bool ImGuiWrap::ProcessEvent(const SDL_Event& event)
{
	return ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiWrap::NewFrame()
{
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

void ImGuiWrap::Render(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pSwapchainTexture)
{
	ImGui::Render();
	ImDrawData* pDrawData = ImGui::GetDrawData();
	const bool is_minimized = (pDrawData->DisplaySize.x <= 0.0f || pDrawData->DisplaySize.y <= 0.0f);

	if (pSwapchainTexture != nullptr && !is_minimized)
	{
		// This is mandatory: call ImGui_ImplSDLGPU3_PrepareDrawData() to upload the vertex/index buffer!
		ImGui_ImplSDLGPU3_PrepareDrawData(pDrawData, pCommandBuffer);

		// Setup and start a render pass
		SDL_GPUColorTargetInfo target_info = {};
		target_info.texture = pSwapchainTexture;
		ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
		target_info.clear_color = SDL_FColor{ clear_color.x, clear_color.y, clear_color.z, clear_color.w };
		target_info.load_op = SDL_GPU_LOADOP_LOAD;
		target_info.store_op = SDL_GPU_STOREOP_STORE;
		target_info.mip_level = 0;
		target_info.layer_or_depth_plane = 0;
		target_info.cycle = false;
		SDL_GPURenderPass* pRenderPass = SDL_BeginGPURenderPass(pCommandBuffer, &target_info, 1, nullptr);

		// Render ImGui
		ImGui_ImplSDLGPU3_RenderDrawData(pDrawData, pCommandBuffer, pRenderPass);

		SDL_EndGPURenderPass(pRenderPass);
	}

	// Update and Render additional Platform Windows
	ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

namespace ImGui
{

void HelpMarker(const char* fmt, ...)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(450.0f);
		va_list args;
		va_start(args, fmt);
		ImGui::TextV(fmt, args);
		va_end(args);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void PushStyleCompact()
{
	ImGuiStyle& style = ImGui::GetStyle();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (float)(int)(style.FramePadding.y * 0.60f)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, (float)(int)(style.ItemSpacing.y * 0.60f)));
}

void PopStyleCompact()
{
	ImGui::PopStyleVar(2);
}

bool SmallCheckbox(const char* label, bool* v)
{
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0)); // small checkbox
	bool ret = ImGui::Checkbox(label, v);
	ImGui::PopStyleVar();
	return ret;
}

} // namespace ImGui
