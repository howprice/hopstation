// Procedurally generate rectangle vertex positions and UVs.
// Allows a screen space axis-aligned rectangle to be rendered without the need for vertex and index buffers.

struct ViewParams
{
	uint viewportWidth;
	uint viewportHeight;
};

struct RectParams
{
	uint x;
	uint y;
	uint w;
	uint h;
	float u0;
	float v0;
	float u1;
	float v1;
};

// // See https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader for binding details
// #TODO: Is packoffset(c0) required?
ConstantBuffer<ViewParams> g_viewParamsCB : register(b0, space1);
ConstantBuffer<RectParams> g_rectParamsCB : register(b1, space1);

struct Input
{
	uint VertexID : SV_VertexID;
};

struct Output
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

Output main(Input input)
{
	Output output;

	// Calculate screen space position.
	// (0,0) is top left, (W-1, H-1) bottom right
	int2 posSS; // must be signed in case numbers become negative if does not fit on screen
	posSS.x = input.VertexID & 1; // [0,1]
	posSS.y = input.VertexID >> 1; // [0,1]
	posSS.x = mad(posSS.x, g_rectParamsCB.w, g_rectParamsCB.x); // [0,w]
	posSS.y = mad(posSS.y, g_rectParamsCB.h, g_rectParamsCB.y); // [0,h]
	
	// Transform screen space coords to NDC (clip space)
	// NDC: (-1,-1) lower left, (+1,+1) top right. https://wiki.libsdl.org/SDL3/CategoryGPU#coordinate-system
	float2 posNDC;
	posNDC.x = (posSS.x / (float)g_viewParamsCB.viewportWidth) * 2 - 1;
	posSS.y = g_viewParamsCB.viewportHeight - posSS.y; // screen space +y is down, but NDC +y is up
	posNDC.y = (posSS.y / (float)g_viewParamsCB.viewportHeight) * 2 - 1;
	
	output.Position = float4(posNDC, 0.0f, 1.0f);
	
	// SDL GPU Texture Coordinates: top-left corner (0, 0). bottom-right corner at (1.0, 1.0). +y is down.
	// https://wiki.libsdl.org/SDL3/CategoryGPU#coordinate-system
	output.UV.x = (input.VertexID & 1) == 0 ? g_rectParamsCB.u0 : g_rectParamsCB.u1;
	output.UV.y = (input.VertexID >> 1) == 0 ? g_rectParamsCB.v0 : g_rectParamsCB.v1;

	return output;
}
