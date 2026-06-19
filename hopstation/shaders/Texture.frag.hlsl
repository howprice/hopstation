// Adapted from https://github.com/TheSpydog/SDL_gpu_examples

// See https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader for binding details
Texture2D<float4> s_texture : register(t0, space2);
SamplerState s_sampler : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
	return s_texture.Sample(s_sampler, uv);
}
