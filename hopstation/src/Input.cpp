#include "Input.h"

#include "core/hp_assert.h"
#include "core/Log.h"
#include "core/FileHelpers.h" // FileExists
#include "core/ArrayHelpers.h" // COUNTOF_ARRAY
#include "core/Helpers.h" // HP_UNUSED, ENUM_COUNT
#include "core/MathsHelpers.h"

#include <SDL3/SDL.h> // Use SDL2 for platform independent input

#include <stdlib.h> // abs
#include <math.h> // fabs

//-------------------------------------------------------------------------------------------------------

#define INPUT_LOG_ENABLED   1
#define INPUT_TRACE_ENABLED 1

#if INPUT_LOG_ENABLED
#define INPUT_LOG(...) LogLevel(LOG_LEVEL_INFO, "[Input] " __VA_ARGS__)
#else
#define INPUT_LOG(...) do {} while (0) // Support macro in if statement without curly braces
#endif

#if INPUT_TRACE_ENABLED
#define INPUT_TRACE(...) LogLevel(LOG_LEVEL_TRACE, "[Input] " __VA_ARGS__)
#else
#define INPUT_TRACE(...) do {} while (0)
#endif

//-------------------------------------------------------------------------------------------------------

static const unsigned int kMaxControllers = 8;

//-------------------------------------------------------------------------------------------------------

// #TODO: Rename this to Gamepad?
struct Controller
{
	static const int kInvalidInstanceId = 0; // SDL_JoystickID value 0 is an invalid ID.

	SDL_Joystick* pJoystick = nullptr;
	SDL_Gamepad* pGamepad = nullptr;
	SDL_JoystickID instanceId = kInvalidInstanceId;

	bool buttonDownThisFrame[SDL_GAMEPAD_BUTTON_COUNT] = { false };
	bool buttonState[SDL_GAMEPAD_BUTTON_COUNT] = { false };

	Sint16 axisValue[SDL_GAMEPAD_AXIS_COUNT];
	Sint16 axisValuePrevFrame[SDL_GAMEPAD_AXIS_COUNT];
};

//-------------------------------------------------------------------------------------------------------

static bool s_keyState[SDL_SCANCODE_COUNT];
static bool s_keyDownThisFrame[SDL_SCANCODE_COUNT]; // was up, now down
static bool s_keyReleasedThisFrame[SDL_SCANCODE_COUNT]; // was down, now released

static Controller s_controllers[kMaxControllers];

static bool s_mouseButtonState[ENUM_COUNT(MouseButton)];
static bool s_mouseButtonDownThisFrame[ENUM_COUNT(MouseButton)];
static bool s_mouseCaptured = false;

static bool s_disableLeftMouseButtonUntilRelease = false; // debounce so mouse button is not read on frame that it is captured, which can be annoying

static InputCallback s_inputCallback;
static void* s_inputCallbackUserData;

//-------------------------------------------------------------------------------------------------------

static void callCallback()
{
	if (s_inputCallback)
		s_inputCallback(s_inputCallbackUserData);
}

static bool addGamepad(SDL_JoystickID joystickID)
{
	HP_ASSERT(SDL_IsGamepad(joystickID));
	HP_ASSERT(joystickID < kMaxControllers);
	
	SDL_Gamepad* pGamepad = SDL_OpenGamepad(joystickID);
	const char* name = SDL_GetGamepadName(pGamepad);
	if(!pGamepad)
	{
		INPUT_LOG("Failed to open gamepad %i \"%s\"\n", joystickID, name);
		return false;
	}

	SDL_Joystick* pJoystick = SDL_GetGamepadJoystick(pGamepad);
	SDL_JoystickID instanceId = SDL_GetJoystickID(pJoystick);
	for(unsigned int i = 0; i < kMaxControllers; ++i)
	{
		if(s_controllers[i].instanceId == instanceId)
		{
			INPUT_TRACE("Controller with instance id %i has already been added\n", instanceId);
			return false;
		}
	}

	// Add to first free slot
	unsigned int controllerIndex;
	for (controllerIndex = 0; controllerIndex < COUNTOF_ARRAY(s_controllers); controllerIndex++)
	{
		if (s_controllers[controllerIndex].instanceId == Controller::kInvalidInstanceId)
			break;
	}

	if (controllerIndex == COUNTOF_ARRAY(s_controllers))
	{
		INPUT_LOG("Maximum number of controllers (%u) reached, cannot add controller %i instance id %i: \"%s\"\n",
			COUNTOF_ARRAY(s_controllers), joystickID, instanceId, name);

		SDL_CloseGamepad(pGamepad);
		return false;
	}
	s_controllers[controllerIndex].pJoystick = nullptr; // don't store point to joystick if is an SDL Controller
	s_controllers[controllerIndex].pGamepad = pGamepad;
	s_controllers[controllerIndex].instanceId = instanceId;

	INPUT_LOG("Added gamepad %i instance id %i: \"%s\"\n", controllerIndex, joystickID, name);
	return true;
}

static bool addJoystick(SDL_JoystickID joystickID)
{
	HP_FATAL_ERROR("#TODO: Fix this up");

	HP_ASSERT(!SDL_IsGamepad(joystickID));
	HP_ASSERT(joystickID < kMaxControllers);

	SDL_Joystick* pJoystick = SDL_OpenJoystick(joystickID);
	s_controllers[joystickID].pJoystick = pJoystick;
	s_controllers[joystickID].pGamepad = nullptr;
	s_controllers[joystickID].instanceId = joystickID;

	const char* name = SDL_GetJoystickName(pJoystick);
	INPUT_LOG("Added joystick %i \"%s\"\n", joystickID, name);
	HP_UNUSED(name);

	return true;
}

//-------------------------------------------------------------------------------------------------------

void Input::Init()
{
	for (unsigned int i = 0; i < SDL_SCANCODE_COUNT; ++i)
	{
		s_keyState[i] = false;
		s_keyDownThisFrame[i] = false;
		s_keyReleasedThisFrame[i] = false;
	}

	for (unsigned int i = 0; i < COUNTOF_ARRAY(s_controllers); ++i)
	{
		for(unsigned int j = 0; j < COUNTOF_ARRAY(s_controllers[i].buttonState); ++j)
		{
			s_controllers[i].buttonState[j] = false;
		}

		for(unsigned int j = 0; j < COUNTOF_ARRAY(s_controllers[i].axisValue); ++j)
		{
			s_controllers[i].axisValue[j] = 0;
			s_controllers[i].axisValuePrevFrame[j] = 0;
		}
	}

	for (unsigned int buttonIndex = 0; buttonIndex < COUNTOF_ARRAY(s_mouseButtonState); buttonIndex++)
	{
		s_mouseButtonState[buttonIndex] = false;
		s_mouseButtonDownThisFrame[buttonIndex] = false;
	}

	s_mouseCaptured = false;

	s_disableLeftMouseButtonUntilRelease = false;

	static const char* kGamepadMappingFilename = "SDL_GameControllerDB/gamecontrollerdb.txt";
	if (FileExists(kGamepadMappingFilename))
	{
		int ret = SDL_AddGamepadMappingsFromFile(kGamepadMappingFilename); // https://github.com/gabomdq/SDL_GameControllerDB
		if (ret == -1)
		{
			INPUT_LOG("SDL_AddGamepadMappingsFromFile failed: %s\n", SDL_GetError());
		}
		else
		{
			INPUT_LOG("Loaded %d gamepad mappings from %s\n", ret, kGamepadMappingFilename);
		}
	}
	else
	{
		LOG_WARN("SDL gamepad mappings file not found: %s\n", kGamepadMappingFilename);
	}

	int joystickCount;
	SDL_JoystickID* pJoystickIDs = SDL_GetJoysticks(&joystickCount);
	INPUT_LOG("Found %d joysticks\n", joystickCount);
	for(int i = 0; i < joystickCount; ++i)
	{
		SDL_JoystickID joystickId = pJoystickIDs[i];
		if(SDL_IsGamepad(joystickId))
			addGamepad(joystickId);
		else
			addJoystick(joystickId);
	}
	SDL_free(pJoystickIDs);
	pJoystickIDs = nullptr;

	// "XInput Controller" = Wired XBOX360 controller
}

void Input::Shutdown()
{
	for(unsigned int i = 0; i < COUNTOF_ARRAY(s_controllers); ++i)
	{
		if(s_controllers[i].instanceId != Controller::kInvalidInstanceId)
		{
			Controller& controller = s_controllers[i];
			if(controller.pJoystick)
			{
				SDL_CloseJoystick(controller.pJoystick);
				controller.pJoystick = nullptr;
			}
			else if(controller.pGamepad)
			{
				SDL_CloseGamepad(controller.pGamepad);
				s_controllers[i].pGamepad = nullptr;
			}

			s_controllers[i].instanceId = Controller::kInvalidInstanceId;
		}
	}

	SDL_CaptureMouse(false);
}

void Input::FrameStart()
{
	for(unsigned int i = 0; i < SDL_SCANCODE_COUNT; ++i)
	{
		s_keyDownThisFrame[i] = false;
		s_keyReleasedThisFrame[i] = false;
	}

	for(unsigned int i = 0; i < kMaxControllers; ++i)
	{
		Controller& controller = s_controllers[i];

		for (unsigned int j = 0; j < SDL_GAMEPAD_BUTTON_COUNT; ++j)
		{
			controller.buttonDownThisFrame[j] = false;
		}

		for (unsigned int j = 0; j < COUNTOF_ARRAY(controller.axisValue); ++j)
		{
			controller.axisValuePrevFrame[j] = controller.axisValue[j];
		}
	}

	for (unsigned int i = 0; i < COUNTOF_ARRAY(s_mouseButtonState); i++)
	{
		s_mouseButtonDownThisFrame[i] = false;
	}
}

bool Input::GetKeyState(unsigned int scancode)
{
	return s_keyState[scancode];
}

bool Input::IsKeyDownThisFrame(unsigned int scancode)
{
	return s_keyDownThisFrame[scancode];
}

bool Input::IsKeyReleasedThisFrame(unsigned int scancode)
{
	return s_keyReleasedThisFrame[scancode];
}

const bool* Input::GetKeyStateArray()
{
	return s_keyState;
}

bool Input::IsControllerConnected(unsigned int index)
{
	HP_ASSERT(index < kMaxControllers);
	return s_controllers[index].instanceId != Controller::kInvalidInstanceId;
}

const char* Input::GetControllerName(unsigned int index)
{
	HP_ASSERT(index < kMaxControllers);

	if(!IsControllerConnected(index))
		return "<not connected>";

	const Controller& controller = s_controllers[index];
	if(controller.pGamepad)
		return SDL_GetGamepadName(controller.pGamepad);
	else
		return SDL_GetJoystickName(controller.pJoystick);
}

bool Input::GetButtonState(unsigned int controllerIndex, unsigned int buttonId)
{
	HP_ASSERT(controllerIndex < kMaxControllers);
	HP_ASSERT(buttonId < SDL_GAMEPAD_BUTTON_COUNT);
	return s_controllers[controllerIndex].buttonState[buttonId];
}

bool Input::IsButtonDownThisFrame(unsigned int controllerIndex, unsigned int buttonId)
{
	HP_ASSERT(controllerIndex < kMaxControllers);
	HP_ASSERT(buttonId < SDL_GAMEPAD_BUTTON_COUNT);
	return s_controllers[controllerIndex].buttonDownThisFrame[buttonId];
}

static float applyDeadZone(Sint16 iValue)
{
	static float s_analogueStickDeadZone = 0.15f;

	float value = (float)iValue / (float)INT16_MAX; // [-1.0f, 1.0f]   
	float magnitude = fabsf(value);
	magnitude = Max(0.0f, (magnitude - s_analogueStickDeadZone) / (1.0f - s_analogueStickDeadZone));
	float direction = value > 0.0f ? 1.0f : -1.0f;
	value = magnitude * direction;
	return value;
}

float Input::GetAxisValue(unsigned int controllerIndex, unsigned int axisId)
{
	HP_ASSERT(controllerIndex < kMaxControllers);
	HP_ASSERT(axisId < SDL_GAMEPAD_AXIS_COUNT);
	const Sint16 iValue = s_controllers[controllerIndex].axisValue[axisId];
	float value = applyDeadZone(iValue);
	return value;
}

float Input::GetAxisValuePrevFrame(unsigned int controllerIndex, unsigned int axisId)
{
	HP_ASSERT(controllerIndex < kMaxControllers);
	HP_ASSERT(axisId < SDL_GAMEPAD_AXIS_COUNT);
	const Sint16 iValue = s_controllers[controllerIndex].axisValuePrevFrame[axisId];
	float value = applyDeadZone(iValue);
	return value;
}

bool Input::IsMouseDown(MouseButton button)
{
	return s_mouseButtonState[(int)button];
}

bool Input::IsMouseDownThisFrame(MouseButton button)
{
	return s_mouseButtonDownThisFrame[(int)button];
}

void Input::SetInputCallback(InputCallback callback, void* userdata)
{
	s_inputCallback = callback;
	s_inputCallbackUserData = userdata;
}

//
// returns null if controller is not recognised, which can happen when pad cable is pulled.
//
// #TODO: Rename this to getGamepadFromInstanceId?
//
static Controller* getControllerFromInstanceId(SDL_JoystickID instanceId)
{
	for(unsigned int i = 0; i < kMaxControllers; ++i)
	{
		if(s_controllers[i].instanceId == instanceId)
			return &s_controllers[i];
	}

	return nullptr;
}

#pragma region "SDL Event Handlers"

static void onJoyDeviceAdded(const SDL_JoyDeviceEvent& jdevice)
{
	SDL_JoystickID joystickID = jdevice.which;
	INPUT_TRACE("SDL_JOYDEVICEADDED, JoystickID: %d\n", joystickID);

	// A SDL_JOYDEVICEADDED event is generated even if a device classed as an SDL Game Controller is added...
	if(SDL_IsGamepad(joystickID))
		return;

	addJoystick(joystickID);
}

static void onJoyDeviceRemoved(const SDL_JoyDeviceEvent& jdevice)
{
	INPUT_TRACE("SDL_JOYDEVICEREMOVED, oystickID: %d\n", jdevice.which);

	SDL_JoystickID instanceId = jdevice.which; // jdevice.which is the instance id for the REMOVED event

	for(unsigned int i = 0; i < kMaxControllers; ++i)
	{
		if(s_controllers[i].instanceId == instanceId)
		{
			Controller& controller = s_controllers[i];

			// A SDL_JOYDEVICEREMOVED event is generated even if a device classed as an SDL Game Controller is added...
			if(controller.pGamepad)
				return;

			INPUT_LOG("Removed controller %i instance ID %i\n", i, instanceId);

			SDL_CloseJoystick(controller.pJoystick);

			// invalidate
			controller.pJoystick = nullptr;
			controller.instanceId = Controller::kInvalidInstanceId;
			return;
		}
	}

//	HP_FATAL_ERROR("Unknown joystick removed"); // Can't error here, because if remove a SDL Controller SDL_JOYDEVICEREMOVED fires after SDL_CONTROLLERDEVICEREMOVED  
}

static void onJoyButtonDown(const SDL_JoyButtonEvent& jbutton)
{
	SDL_JoystickID instanceId = jbutton.which;
	INPUT_TRACE("SDL_JOYBUTTONDOWN  JoystickID: %u  Button: %u  Down: %u\n", instanceId, jbutton.button, jbutton.down);

	Controller* pController = getControllerFromInstanceId(instanceId);
	if(!pController)
		return; // controller may have been pulled while events still in queue

	if(pController->pGamepad)
		return; // handled by OnGamepadButtonDown

	pController->buttonDownThisFrame[jbutton.button] = true;
	pController->buttonState[jbutton.button] = true;

	callCallback();
}

static void onJoyButtonUp(const SDL_JoyButtonEvent& jbutton)
{
	SDL_JoystickID instanceId = jbutton.which;
	INPUT_TRACE("SDL_JOYBUTTONUP  JoystickID: %u  Button: %u  Down: %u\n", jbutton.which, jbutton.button, jbutton.down);

	Controller* pController = getControllerFromInstanceId(instanceId);
	if(!pController)
		return; // controller may have been pulled while events still in queue

	if(pController->pGamepad)
		return; // handled by OnGamepadButtonUp

	pController->buttonState[jbutton.button] = false;

	callCallback();
}

static void onJoyAxisMotion(const SDL_JoyAxisEvent& jaxis)
{
	INPUT_TRACE("SDL_JOYAXISMOTION, instanceId: %d  axis: %u  value: %d\n", jaxis.which, jaxis.axis, jaxis.value);

	Controller* pController = getControllerFromInstanceId(jaxis.which);
	if(!pController)
		return; // controller may have been pulled while events still in queue

	// Both SDL_JOYAXISMOTION and SDL_CONTROLLERAXISMOTION fire for game controllers
	if(pController->pGamepad)
		return;

	if(jaxis.axis == 0)
	{ 
		// horizontal

		if(jaxis.value > 0)
		{
			pController->buttonDownThisFrame[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = false;
		}
		else if(jaxis.value < 0)
		{
			pController->buttonDownThisFrame[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = false;
		}
		else // jaxis.value == 0
		{
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = false;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = false;
		}
	}
	else if(jaxis.axis == 1)
	{
		// vertical

		if(jaxis.value > 0)
		{
			pController->buttonDownThisFrame[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_UP] = false;
		}
		else if(jaxis.value < 0)
		{
			pController->buttonDownThisFrame[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = false;
		}
		else // jaxis.value == 0
		{
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_UP] = false;
			pController->buttonState[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = false;
		}
	}

	callCallback();
}

// this shouldn't be needed: joy axes provide all the directional info
static void onJoyHatMotion(const SDL_JoyHatEvent& jhat)
{
	INPUT_TRACE("JoyHatMotion, JoystickID: %d  hat index: %u  value: %s %s %s %s\n", jhat.which, jhat.hat,
		(jhat.value & SDL_HAT_UP) ? "SDL_HAT_UP" : "",
		(jhat.value & SDL_HAT_RIGHT) ? "SDL_HAT_RIGHT" : "",
		(jhat.value & SDL_HAT_DOWN) ? "SDL_HAT_DOWN" : "",
		(jhat.value & SDL_HAT_LEFT) ? "SDL_HAT_LEFT" : "");

	callCallback();
}

static void onGamepadAdded(const SDL_GamepadDeviceEvent& gdevice)
{
	// gdevice.which is the the joystick device index for the SDL_EVENT_GAMEPAD_ADDED event
	addGamepad(gdevice.which);
}

static void onGamepadRemoved(const SDL_GamepadDeviceEvent& gdevice)
{
	// gdevice.which is the instance id for the SDL_EVENT_GAMEPAD_REMOVED event
	const SDL_JoystickID instanceId = gdevice.which;

	for(unsigned int i = 0; i < kMaxControllers; ++i)
	{
		if(s_controllers[i].instanceId == instanceId)
		{
			INPUT_LOG("Removed controller %i instance ID %i\n", i, gdevice.which);

			SDL_CloseGamepad(s_controllers[i].pGamepad);

			// invalidate the controller
			s_controllers[i].pGamepad = nullptr;
			s_controllers[i].instanceId = Controller::kInvalidInstanceId;
			return;
		}
	}

	HP_FATAL_ERROR("Unknown controller removed");
}

static void onGamepadButtonDown(const SDL_GamepadButtonEvent& gbutton)
{
	INPUT_TRACE("SDL_EVENT_GAMEPAD_BUTTON_DOWN  JoystickID: %u  Button: %u  Down: %u\n", gbutton.which, gbutton.button, gbutton.down);
	Controller* pController = getControllerFromInstanceId(gbutton.which);
	if(!pController)
		return; // controller may have been pulled while events still in queue

	pController->buttonDownThisFrame[gbutton.button] = true;
	pController->buttonState[gbutton.button] = true;

	callCallback();
}

static void onGamepadButtonUp(const SDL_GamepadButtonEvent& gbutton)
{
	INPUT_TRACE("SDL_EVENT_GAMEPAD_BUTTON_UP  JoystickID: %u  Button: %u  Down: %u\n", gbutton.which, gbutton.button, gbutton.down);
	Controller* pController = getControllerFromInstanceId(gbutton.which);
	if(!pController)
		return; // controller may have been pulled while events still in queue
	pController->buttonState[gbutton.button] = false;

	callCallback();
}

static void onGamepadAxisMotion(const SDL_GamepadAxisEvent& gaxis)
{
	static bool printAxisMotionEvent = false;
	if(printAxisMotionEvent)
	{
		if(abs(gaxis.value) > 10000)
			INPUT_TRACE("SDL_CONTROLLERAXISMOTION  JoystickID: %u  Axis: %u  Value: %d\n", gaxis.which, gaxis.axis, gaxis.value);
	}

	Controller* pController = getControllerFromInstanceId(gaxis.which);
	if(!pController)
		return; // controller may have been pulled while events still in queue

	pController->axisValue[gaxis.axis] = gaxis.value;

	callCallback();
}

static void onKeyDown(const SDL_KeyboardEvent& key)
{
	INPUT_TRACE("SDL_KEYDOWN Scancode: %i Keycode: %i repeat: %u\n", key.scancode, key.key, key.repeat);

	if (key.repeat == 0)
	{
		s_keyState[key.scancode] = true;
		s_keyDownThisFrame[key.scancode] = true;

		callCallback();
	}
}

static void onKeyUp(const SDL_KeyboardEvent& key)
{
	INPUT_TRACE("SDL_KEYUP Scancode: %i Keycode: %i \n", key.scancode, key.key);
	s_keyState[key.scancode] = false;
	s_keyReleasedThisFrame[key.scancode] = true;

	callCallback();
}

static void onMouseButtonDown(const SDL_MouseButtonEvent& buttonEvent)
{
	MouseButton mouseButton;
	if (buttonEvent.button == SDL_BUTTON_LEFT)
		mouseButton = MouseButton::Left;
	else if (buttonEvent.button == SDL_BUTTON_RIGHT)
		mouseButton = MouseButton::Right;
	else if (buttonEvent.button == SDL_BUTTON_MIDDLE)
		mouseButton = MouseButton::Middle;
	else
		return;

	// Don't record left mouse button while disabled
	if (mouseButton == MouseButton::Left && s_disableLeftMouseButtonUntilRelease)
		return;
	
	s_mouseButtonState[(int)mouseButton] = true;
	s_mouseButtonDownThisFrame[(int)mouseButton] = true;
}

static void onMouseButtonUp(const SDL_MouseButtonEvent& button)
{
	MouseButton mouseButton;
	if (button.button == SDL_BUTTON_LEFT)
		mouseButton = MouseButton::Left;
	else if (button.button == SDL_BUTTON_RIGHT)
		mouseButton = MouseButton::Right;
	else if (button.button == SDL_BUTTON_MIDDLE)
		mouseButton = MouseButton::Middle;
	else
		return;

	if (mouseButton == MouseButton::Left && s_disableLeftMouseButtonUntilRelease)
		s_disableLeftMouseButtonUntilRelease = false; // start accepting left mouse clicks now

	s_mouseButtonState[(int)mouseButton] = false;
}

static void onWindowFocusGained()
{

}

static void onWindowFocusLost()
{

}

void Input::HandleSDLEvent(const SDL_Event& event)
{
	if (event.type == SDL_EVENT_KEY_DOWN)
		onKeyDown(event.key);
	else if (event.type == SDL_EVENT_KEY_UP)
		onKeyUp(event.key);
	else if (event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION)
		onJoyAxisMotion(event.jaxis);
	else if (event.type == SDL_EVENT_JOYSTICK_HAT_MOTION)
		onJoyHatMotion(event.jhat);
	else if (event.type == SDL_EVENT_JOYSTICK_ADDED)
		onJoyDeviceAdded(event.jdevice);
	else if (event.type == SDL_EVENT_JOYSTICK_REMOVED)
		onJoyDeviceRemoved(event.jdevice);
	else if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
		onJoyButtonDown(event.jbutton);
	else if (event.type == SDL_EVENT_JOYSTICK_BUTTON_UP)
		onJoyButtonUp(event.jbutton);
	else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
		onGamepadButtonDown(event.gbutton);
	else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
		onGamepadButtonUp(event.gbutton);
	else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)
		onGamepadAxisMotion(event.gaxis);
	else if (event.type == SDL_EVENT_GAMEPAD_ADDED)
		onGamepadAdded(event.gdevice);
	else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
		onGamepadRemoved(event.gdevice);
	else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		onMouseButtonDown(event.button);
	else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
		onMouseButtonUp(event.button);
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
		onWindowFocusGained();
	else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
		onWindowFocusLost();
}

#pragma endregion "SDL Event Handlers"
