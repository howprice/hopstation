#pragma once

#include <SDL3/SDL_gpu.h>

class Texture
{
public:

	static const unsigned int kBytesPerPixel = 4;

	Texture(unsigned int width, unsigned int height, const char* name);
	~Texture();

	// Copy image data into transfer buffer in preparation for upload
	void CopyImageDataToTransferBuffer(const void* data);

	unsigned int GetWidth() const { return m_width; }
	unsigned int GetHeight() const { return m_height; }
	SDL_GPUTextureFormat GetFormat() const { return m_format; }
	SDL_GPUTexture* GetTexture() const { return m_pTexture; }
	SDL_GPUTransferBuffer* GetTransferBuffer() const { return m_pTextureUploadBuffer; };


private:
	unsigned int m_width;
	unsigned int m_height;
	SDL_GPUTextureFormat m_format;
	unsigned int m_bytesPerPixel;

	SDL_GPUTexture* m_pTexture = nullptr;
	SDL_GPUTransferBuffer* m_pTextureUploadBuffer = nullptr;
};
