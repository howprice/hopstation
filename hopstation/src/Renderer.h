#pragma once

#include "core/ClassHelpers.h"

typedef struct SDL_Rect SDL_Rect;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;

class Texture;

class Renderer
{
public:

	NON_INSTANTIABLE_STATIC_CLASS(Renderer);

	static bool Init(SDL_Window* pWindow);
	static void Shutdown();

	// If returns false then do not render
	[[nodiscard]] static bool Begin();

	static void End();

	static void BeginCopyPass();
	static void UploadTexture(Texture* pTexture);
	static void EndCopyPass();

	static void BeginRenderPass();

	// Reimplementation of SDL_RenderTexture
	// srcrect a pointer to the source rectangle, or NULL for the entire texture.
	// dstrect a pointer to the destination rectangle, or NULL for the entire rendering target.
	static void DrawTexture(Texture* pTexture, SDL_Rect* pSrcRect, SDL_Rect* pDstRect);

	static void EndRenderPass();

	static SDL_GPUDevice* GetDevice();
};
