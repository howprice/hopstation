#pragma once

#include "core/ClassHelpers.h"

//#include <SDL3/SDL.h> // Removed - don't want to include SDL.h in this header otherwise anything that includes it will also include SDL.h.
//#include "SDL_Scancode.h" // can't foward declare SDL_Scancode. Removed - doesn't work on Linux

// forward declarations
union SDL_Event;

// Matching SDL codes to avoid including SDL headers
#define KEYBOARD_SCANCODE_A 4
#define KEYBOARD_SCANCODE_B 5
#define KEYBOARD_SCANCODE_C 6
#define KEYBOARD_SCANCODE_D 7
#define KEYBOARD_SCANCODE_E 8
#define KEYBOARD_SCANCODE_F 9
#define KEYBOARD_SCANCODE_G 10
#define KEYBOARD_SCANCODE_H 11
#define KEYBOARD_SCANCODE_I 12
#define KEYBOARD_SCANCODE_J 13
#define KEYBOARD_SCANCODE_K 14
#define KEYBOARD_SCANCODE_L 15
#define KEYBOARD_SCANCODE_M 16
#define KEYBOARD_SCANCODE_N 17
#define KEYBOARD_SCANCODE_O 18
#define KEYBOARD_SCANCODE_P 19
#define KEYBOARD_SCANCODE_Q 20
#define KEYBOARD_SCANCODE_R 21
#define KEYBOARD_SCANCODE_S 22
#define KEYBOARD_SCANCODE_T 23
#define KEYBOARD_SCANCODE_U 24
#define KEYBOARD_SCANCODE_V 25
#define KEYBOARD_SCANCODE_W 26
#define KEYBOARD_SCANCODE_X 27
#define KEYBOARD_SCANCODE_Y 28
#define KEYBOARD_SCANCODE_Z 29

#define KEYBOARD_SCANCODE_1 30
#define KEYBOARD_SCANCODE_2 31
#define KEYBOARD_SCANCODE_3 32
#define KEYBOARD_SCANCODE_4 33
#define KEYBOARD_SCANCODE_5 34
#define KEYBOARD_SCANCODE_6 35
#define KEYBOARD_SCANCODE_7 36
#define KEYBOARD_SCANCODE_8 37
#define KEYBOARD_SCANCODE_9 38
#define KEYBOARD_SCANCODE_0 39

#define KEYBOARD_SCANCODE_RETURN    40
#define KEYBOARD_SCANCODE_ESCAPE    41
#define KEYBOARD_SCANCODE_BACKSPACE 42
#define KEYBOARD_SCANCODE_TAB       43
#define KEYBOARD_SCANCODE_SPACE     44

#define KEYBOARD_SCANCODE_COMMA 54
#define KEYBOARD_SCANCODE_PERIOD 55
#define KEYBOARD_SCANCODE_SLASH 56

#define KEYBOARD_SCANCODE_CAPSLOCK 57

#define KEYBOARD_SCANCODE_F1 58
#define KEYBOARD_SCANCODE_F2 59
#define KEYBOARD_SCANCODE_F3 60
#define KEYBOARD_SCANCODE_F4 61
#define KEYBOARD_SCANCODE_F5 62
#define KEYBOARD_SCANCODE_F6 63
#define KEYBOARD_SCANCODE_F7 64
#define KEYBOARD_SCANCODE_F8 65
#define KEYBOARD_SCANCODE_F9 66
#define KEYBOARD_SCANCODE_F10 67
#define KEYBOARD_SCANCODE_F11 68
#define KEYBOARD_SCANCODE_F12 69

#define KEYBOARD_SCANCODE_HOME 74
#define KEYBOARD_SCANCODE_PAGEUP 75
#define KEYBOARD_SCANCODE_DELETE 76
#define KEYBOARD_SCANCODE_END 77
#define KEYBOARD_SCANCODE_PAGEDOWN 78
#define KEYBOARD_SCANCODE_RIGHT 79
#define KEYBOARD_SCANCODE_LEFT 80
#define KEYBOARD_SCANCODE_DOWN 81
#define KEYBOARD_SCANCODE_UP 82

#define KEYBOARD_SCANCODE_LCTRL  224
#define KEYBOARD_SCANCODE_LSHIFT 225
#define KEYBOARD_SCANCODE_LALT   226
#define KEYBOARD_SCANCODE_RCTRL  228
#define KEYBOARD_SCANCODE_RSHIFT 229
#define KEYBOARD_SCANCODE_RALT   230

#define KEYBOARD_SCANCODE_COUNT 512

enum class MouseButton
{
	Left,
	Right,
	Middle,

	Max = Middle
};

typedef void (*InputCallback)(void* userdata);

class Input
{
	NON_INSTANTIABLE_STATIC_CLASS(Input);

public:

	static void Init();
	static void Shutdown();

	// Call this as close to the start of the frame as possible
	// Preferably immediatly before the SDL event loop
	static void FrameStart();

	// SDL event handler
	// Call this from within  while (SDL_PollEvent(&event)) {} loop each frame
	static void HandleSDLEvent(const SDL_Event& event);

	// Pass SDL_Scancode enum values to this function
	static bool GetKeyState(unsigned int scancode);
	static bool IsKeyDownThisFrame(unsigned int scancode); // was up last frame, now down
	static bool IsKeyReleasedThisFrame(unsigned int scancode); // was down last frame, now up
	static const bool* GetKeyStateArray(); // 512 element array

	static bool IsControllerConnected(unsigned int index);
	static const char* GetControllerName(unsigned int index);

	static bool GetButtonState(unsigned int controllerIndex, unsigned int buttonId);
	static bool IsButtonDownThisFrame(unsigned int controllerIndex, unsigned int buttonId);

	// axisId : SDL_GameControllerAxis
	// Returns [-1.0f,+1.0f] where -ve is left and +ve is right
	static float GetAxisValue(unsigned int controllerIndex, unsigned int axisId);
	static float GetAxisValuePrevFrame(unsigned int controllerIndex, unsigned int axisId);

	static bool IsMouseDown(MouseButton button);
	static bool IsMouseDownThisFrame(MouseButton button);

	static void SetInputCallback(InputCallback callback, void* userdata);
};
