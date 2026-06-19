#include "Texture.h"

#include "Renderer.h"

#include "core/hp_assert.h"

#include <SDL3/SDL_gpu.h>

Texture::Texture(unsigned int width, unsigned int height, const char* name)
	: m_width(width)
	, m_height(height)
	, m_format(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) // Wanted to use SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM to avoid PSX to host GPU conversion, but infortunately the RGB components are the other way round.
	, m_bytesPerPixel(4)

{
	SDL_GPUDevice* pDevice = Renderer::GetDevice();

	// Create texture
	SDL_GPUTextureCreateInfo textureCreateInfo{};
	textureCreateInfo.type = SDL_GPU_TEXTURETYPE_2D;
	textureCreateInfo.format = m_format;
	textureCreateInfo.width = width;
	textureCreateInfo.height = height;
	textureCreateInfo.layer_count_or_depth = 1;
	textureCreateInfo.num_levels = 1;
	textureCreateInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	m_pTexture = SDL_CreateGPUTexture(pDevice, &textureCreateInfo);
	HP_ASSERT(m_pTexture, "SDL_CreateGPUTexture() failed: %s\n", SDL_GetError());

	if (name && name[0])
		SDL_SetGPUTextureName(pDevice, m_pTexture, name); // not thread safe

	// Create a staging buffer to upload the texture data.
	SDL_GPUTransferBufferCreateInfo uploadBufferInfo{};
	uploadBufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	uploadBufferInfo.size = width * height * m_bytesPerPixel;
	m_pTextureUploadBuffer = SDL_CreateGPUTransferBuffer(pDevice, &uploadBufferInfo);
	HP_ASSERT(m_pTextureUploadBuffer, "SDL_CreateGPUTransferBuffer() failed: %s\n", SDL_GetError());
}

Texture::~Texture()
{
	SDL_GPUDevice* pDevice = Renderer::GetDevice();
	SDL_ReleaseGPUTransferBuffer(pDevice, m_pTextureUploadBuffer);
	SDL_ReleaseGPUTexture(pDevice, m_pTexture);
}

void Texture::CopyImageDataToTransferBuffer(const void* data)
{
	SDL_GPUDevice* pDevice = Renderer::GetDevice();

	void* pTextureData = SDL_MapGPUTransferBuffer(pDevice, m_pTextureUploadBuffer, false);
	HP_ASSERT(pTextureData, "SDL_MapGPUTransferBuffer() failed: %s\n", SDL_GetError());
	SDL_memcpy(pTextureData, data, m_width * m_height * m_bytesPerPixel);
	SDL_UnmapGPUTransferBuffer(pDevice, m_pTextureUploadBuffer);
}
