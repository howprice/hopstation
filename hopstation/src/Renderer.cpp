// Originally based on code from https://github.com/TheSpydog/SDL_gpu_examples BasicTriangle

#include "Renderer.h"
#include "Texture.h"

#include "GUI/ImGuiWrap.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/StringHelpers.h"
#include "core/Helpers.h" //HP_UNUSED

#include <SDL3/SDL.h>

struct ViewParams
{
	uint32_t viewportWidth;
	uint32_t viewportHeight;
};

struct RectParams
{
	int32_t x;
	int32_t y;
	uint32_t w;
	uint32_t h;
	float u0;
	float v0;
	float u1;
	float v1;
};

static SDL_GPUDevice* s_pDevice;
static SDL_Window* s_pWindow;

// The shader format supported by the GPU.
static SDL_GPUShaderFormat s_gpuShaderFormat = SDL_GPU_SHADERFORMAT_INVALID;

// Current swapchain texture and dimensions
static SDL_GPUTexture* s_pSwapchainTexture;
static Uint32 s_swapChainWidth, s_swapChainHeight; // current swap chain width (changes when window is resized)

static SDL_GPUCopyPass* s_pCopyPass;
static SDL_GPURenderPass* s_pRenderPass;

// We just use a single command buffer for the whole frame.
// This is acquired and submitted each fraem - it is not reused between frames.
static SDL_GPUCommandBuffer* s_pCommandBuffer;

static SDL_GPUSampler* s_pPointClampSampler;
static SDL_GPUGraphicsPipeline* s_pRectGPO;

//---------------------------------------------------------------------------------------------

//
// #TODO: Move this somewhere else if/when required e.g. ShaderLoader.cpp
//
static SDL_GPUShader* loadShader(
	SDL_GPUDevice* pDevice,
	const char* path,
	SDL_GPUShaderStage stage,
	unsigned int samplerCount, // #TODO: Is there a way to get this from shader metadata i.e. shader reflection API?
	unsigned int uniformBufferCount)  // #TODO: Is there a way to get this from shader metadata i.e. shader reflection API?
{
	// Generate full filename
	char fullPath[256];
	switch (s_gpuShaderFormat)
	{
		case SDL_GPU_SHADERFORMAT_SPIRV:
			SafeSnprintf(fullPath, sizeof(fullPath), "%s.spv", path);
			break;
		case SDL_GPU_SHADERFORMAT_DXIL:
			SafeSnprintf(fullPath, sizeof(fullPath), "%s.dxil", path);
			break;
		case SDL_GPU_SHADERFORMAT_MSL:
			SafeSnprintf(fullPath, sizeof(fullPath), "%s.msl", path);
			break;
		default:
			LOG_ERROR("Unsupported shader format\n");
			return nullptr;
	}

	size_t codeSize;
	void* pCode = SDL_LoadFile(fullPath, &codeSize);
	if (pCode == NULL)
	{
		LOG_ERROR("SDL_LoadFile(%s) failed: %s\n", path, SDL_GetError());
		return nullptr;
	}

	SDL_GPUShaderCreateInfo shaderInfo{};
	shaderInfo.code_size = codeSize;
	shaderInfo.code = (Uint8*)pCode;
	switch (s_gpuShaderFormat)
	{
		case SDL_GPU_SHADERFORMAT_SPIRV:
			shaderInfo.entrypoint = "main";
			break;
		case SDL_GPU_SHADERFORMAT_DXIL:
			shaderInfo.entrypoint = "main";
			break;
		case SDL_GPU_SHADERFORMAT_MSL:
			// SPIRV-Cross generates entry point "main0" when converting HLSL/SPIR-V to MSL, because main is a reserved identifier in C/C++ (which MSL is based on).
			shaderInfo.entrypoint = "main0";
			break;
		default:
			LOG_ERROR("Unsupported shader format\n");
			return nullptr;
	}
	shaderInfo.format = s_gpuShaderFormat;
	shaderInfo.stage = stage;
	shaderInfo.num_samplers = samplerCount;
	shaderInfo.num_storage_textures = /*storageTextureCount*/0;
	shaderInfo.num_storage_buffers = /*storageBufferCount*/0;
	shaderInfo.num_uniform_buffers = uniformBufferCount;

	SDL_GPUShader* pShader = SDL_CreateGPUShader(pDevice, &shaderInfo);
	if (pShader == NULL)
		LOG_ERROR("SDL_CreateGPUShader() failed: %s\n", SDL_GetError());

	SDL_free(pCode);
	return pShader;
}

//---------------------------------------------------------------------------------------------

bool Renderer::Init(SDL_Window* pWindow)
{
	s_pWindow = pWindow;

	unsigned int numDrivers = SDL_GetNumGPUDrivers();
	LOG_INFO("Number of GPU drivers available: %u\n", numDrivers);
	for (unsigned int i = 0; i < numDrivers; i++)
	{
		const char* name = SDL_GetGPUDriver(i);
		LOG_INFO("  GPU Driver %u: %s\n", i, name ? name : "Unknown");
	}

#ifdef DEBUG
	bool hostGpuDebugMode = true;
#else
	bool hostGpuDebugMode = false;
#endif
	// #TODO: May want to specify driver name e.g. "vulkan"
	s_pDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB, hostGpuDebugMode, /*name*/nullptr);
	if (s_pDevice == nullptr)
	{
		LOG_ERROR("SDL_CreateGPUDevice() failed: %s\n", SDL_GetError());
		return false;
	}

	const char* driverName = SDL_GetGPUDeviceDriver(s_pDevice);
	LOG_INFO("Using GPU device driver: %s\n", driverName);

	SDL_PropertiesID deviceProperties = SDL_GetGPUDeviceProperties(s_pDevice);
	if (deviceProperties != 0)
	{
		const char* deviceName = SDL_GetStringProperty(deviceProperties, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
		LOG_INFO("GPU device name: %s\n", deviceName ? deviceName : "Unknown");

		const char* deviceDriverName = SDL_GetStringProperty(deviceProperties, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr);
		LOG_INFO("GPU device driver name: %s\n", deviceDriverName ? deviceDriverName : "Unknown");

		const char* deviceDriverVersionName = SDL_GetStringProperty(deviceProperties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, nullptr);
		LOG_INFO("GPU device driver version: %s\n", deviceDriverVersionName ? deviceDriverVersionName : "Unknown");

		const char* deviceDriverInfo = SDL_GetStringProperty(deviceProperties, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, nullptr);
		LOG_INFO("GPU device driver info: %s\n", deviceDriverInfo ? deviceDriverInfo : "Unknown");
	}

	if (!SDL_ClaimWindowForGPUDevice(s_pDevice, pWindow))
	{
		LOG_ERROR("SDL_ClaimWindowForGPUDevice() failed: %s\n", SDL_GetError());
		return false;
	}

	if (!SDL_SetGPUSwapchainParameters(s_pDevice, pWindow, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC))
	{
		LOG_ERROR("SDL_SetGPUSwapchainParameters() failed: %s\n", SDL_GetError());
		return false;
	}

	SDL_GPUShaderFormat backendFormats = SDL_GetGPUShaderFormats(s_pDevice);
	LOG_INFO("GPU device shader formats: 0x%08X\n", backendFormats);
	s_gpuShaderFormat = SDL_GPU_SHADERFORMAT_INVALID;
	if (backendFormats & SDL_GPU_SHADERFORMAT_SPIRV)
		s_gpuShaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
	else if (backendFormats & SDL_GPU_SHADERFORMAT_DXIL)
		s_gpuShaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
	else if (backendFormats & SDL_GPU_SHADERFORMAT_MSL)
		s_gpuShaderFormat = SDL_GPU_SHADERFORMAT_MSL;
	else
	{
		LOG_ERROR("GPU does not support required shader formats\n");
		return false;
	}

	SDL_GPUShader* pRectVS = loadShader(s_pDevice, "shaders/Rect.vert", SDL_GPU_SHADERSTAGE_VERTEX, /*samplerCount*/0, /*uniformBufferCount*/2);
	if (!pRectVS)
		return false;

	SDL_GPUShader* pTextureFS = loadShader(s_pDevice, "shaders/Texture.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, /*samplerCount*/1, /*uniformBufferCount*/0);
	if (!pTextureFS)
		return false;

	const SDL_GPUTextureFormat textureFormat = SDL_GetGPUSwapchainTextureFormat(s_pDevice, pWindow);

	SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo{};
	pipelineCreateInfo.target_info.num_color_targets = 1;
	SDL_GPUColorTargetDescription colorTargetDescriptions[] = {
		{textureFormat, SDL_GPUColorTargetBlendState() }
	};
	pipelineCreateInfo.target_info.color_target_descriptions = colorTargetDescriptions;
	pipelineCreateInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;

	pipelineCreateInfo.vertex_shader = pRectVS;
	pipelineCreateInfo.fragment_shader = pTextureFS;
	s_pRectGPO = SDL_CreateGPUGraphicsPipeline(s_pDevice, &pipelineCreateInfo);
	if (s_pRectGPO == NULL)
	{
		LOG_ERROR("SDL_CreateGPUGraphicsPipeline() failed: %s\n", SDL_GetError());
		return false;
	}

	// Shaders can be released when referenced (owned) by the pipeline.
	SDL_ReleaseGPUShader(s_pDevice, pRectVS);
	pRectVS = nullptr;
	SDL_ReleaseGPUShader(s_pDevice, pTextureFS);
	pTextureFS = nullptr;

	// Create PointClamp sampler
	SDL_GPUSamplerCreateInfo samplerCreateInfo{};
	samplerCreateInfo.min_filter = SDL_GPU_FILTER_NEAREST;
	samplerCreateInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
	samplerCreateInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	samplerCreateInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerCreateInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerCreateInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	s_pPointClampSampler = SDL_CreateGPUSampler(s_pDevice, &samplerCreateInfo);
	if (s_pPointClampSampler == NULL)
	{
		LOG_ERROR("SDL_CreateGPUSampler() failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

void Renderer::Shutdown()
{
	SDL_WaitForGPUIdle(s_pDevice);

	SDL_ReleaseGPUSampler(s_pDevice, s_pPointClampSampler);
	s_pPointClampSampler = nullptr;

	SDL_ReleaseGPUGraphicsPipeline(s_pDevice, s_pRectGPO);
	s_pRectGPO = nullptr;

	SDL_ReleaseWindowFromGPUDevice(s_pDevice, s_pWindow);
	SDL_DestroyGPUDevice(s_pDevice);
	s_pDevice = nullptr;
}

bool Renderer::Begin()
{
	s_pCommandBuffer = SDL_AcquireGPUCommandBuffer(s_pDevice);
	HP_ASSERT(s_pCommandBuffer, "SDL_AcquireGPUCommandBuffer() failed: %s\n", SDL_GetError());

	if (!SDL_WaitAndAcquireGPUSwapchainTexture(s_pCommandBuffer, s_pWindow, &s_pSwapchainTexture, &s_swapChainWidth, &s_swapChainHeight))
	{
		LOG_ERROR("SDL_WaitAndAcquireGPUSwapchainTexture() failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

void Renderer::BeginCopyPass()
{
	HP_ASSERT(!s_pCopyPass);
	s_pCopyPass = SDL_BeginGPUCopyPass(s_pCommandBuffer);
}

void Renderer::UploadTexture(Texture* pTexture)
{
	HP_ASSERT(s_pCopyPass);

	// Upload staging buffer to GPU into resource memory
	SDL_GPUTextureTransferInfo source{};
	source.transfer_buffer = pTexture->GetTransferBuffer();
	source.offset = 0;
	SDL_GPUTextureRegion destination{};
	destination.texture = pTexture->GetTexture();
	destination.w = pTexture->GetWidth();
	destination.h = pTexture->GetHeight();
	destination.d = 1;
	SDL_UploadToGPUTexture(s_pCopyPass, &source, &destination, /*cycle*/false);
}

void Renderer::EndCopyPass()
{
	HP_ASSERT(s_pCopyPass);
	SDL_EndGPUCopyPass(s_pCopyPass);
	s_pCopyPass = nullptr;
}

void Renderer::BeginRenderPass()
{
	HP_ASSERT(s_pSwapchainTexture);

	SDL_GPUColorTargetInfo colorTargetInfo{};
	colorTargetInfo.texture = s_pSwapchainTexture;
	colorTargetInfo.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
	colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
	colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

	HP_ASSERT(!s_pRenderPass);
	s_pRenderPass = SDL_BeginGPURenderPass(s_pCommandBuffer, &colorTargetInfo, /*numColorTargets*/1, /*depth_stencil_target_info*/NULL);

	ViewParams viewParams{};
	viewParams.viewportWidth = s_swapChainWidth;
	viewParams.viewportHeight = s_swapChainHeight;
	SDL_PushGPUVertexUniformData(s_pCommandBuffer, /*slot*/0, &viewParams, sizeof(viewParams)); // Slot must match HLSL register index (not sure about space)
}

void Renderer::DrawTexture(Texture* pTexture, SDL_Rect* pSrcRect, SDL_Rect* pDstRect)
{
	HP_ASSERT(s_pRenderPass);

	SDL_BindGPUGraphicsPipeline(s_pRenderPass, s_pRectGPO);

	// Bind texture-sampler pair
	SDL_GPUTextureSamplerBinding textureSamplerBinding{};
	textureSamplerBinding.texture = pTexture->GetTexture();
	textureSamplerBinding.sampler = s_pPointClampSampler;
	SDL_BindGPUFragmentSamplers(s_pRenderPass, /*first_slot*/0, &textureSamplerBinding, /*num_bindings*/1);

	// Calculate constant buffer params
	RectParams rectParams{};

	if (pSrcRect)
	{
		rectParams.u0 = (float)pSrcRect->x / (float)pTexture->GetWidth();
		rectParams.v0 = (float)pSrcRect->y / (float)pTexture->GetHeight();
		rectParams.u1 = (float)(pSrcRect->x + pSrcRect->w) / (float)pTexture->GetWidth();
		rectParams.v1 = (float)(pSrcRect->y + pSrcRect->h) / (float)pTexture->GetHeight();
	}
	else
	{
		// use entire src texture
		rectParams.u0 = 0.0f;
		rectParams.v0 = 0.0f;
		rectParams.u1 = 1.0f;
		rectParams.v1 = 1.0f;
	}

	if (pDstRect)
	{
		rectParams.x = pDstRect->x;
		rectParams.y = pDstRect->y;
		rectParams.w = pDstRect->w;
		rectParams.h = pDstRect->h;
	}
	else
	{
		// use entire render target
		rectParams.x = 0;
		rectParams.y = 0;
		rectParams.w = s_swapChainWidth;
		rectParams.h = s_swapChainHeight;
	}

	SDL_PushGPUVertexUniformData(s_pCommandBuffer, /*slot*/1, &rectParams, sizeof(rectParams)); // Slot must match HLSL register index (not sure about space)

	SDL_DrawGPUPrimitives(s_pRenderPass, /*numVerts*/4, /*numInstances*/1, /*firstVert*/0, /*firstInstance*/0);
}

void Renderer::EndRenderPass()
{
	HP_ASSERT(s_pRenderPass);
	SDL_EndGPURenderPass(s_pRenderPass);
	s_pRenderPass = nullptr;
}

void Renderer::End()
{
	HP_ASSERT(s_pCommandBuffer);

	// Render ImGui as overlay
	ImGuiWrap::Render(s_pCommandBuffer, s_pSwapchainTexture);

	// Acquired command buffers must be submitted.
	if (!SDL_SubmitGPUCommandBuffer(s_pCommandBuffer))
	{
		HP_FATAL_ERROR("SDL_SubmitGPUCommandBuffer() failed: %s\n", SDL_GetError());
		return;
	}
	s_pCommandBuffer = nullptr;
}

SDL_GPUDevice* Renderer::GetDevice()
{
	return s_pDevice;
}
