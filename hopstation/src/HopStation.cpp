// This will become the windowed application

#include "GUI/InsertDiscDialog.h"
#include "GUI/MemoryCardFileDialog.h"
#include "GUI/SideloadDialog.h"
#include "GUI/SnapshotDialog.h"
#include "GUI/CDROMWindow.h"
#include "GUI/CDWindow.h"
#include "GUI/CPUWindow.h"
#include "GUI/DMAWindow.h"
#include "GUI/GPUWindow.h"
#include "GUI/HostWindow.h"
#include "GUI/MemoryCardWindow.h"
#include "GUI/SPUWindow.h"
#include "GUI/ImGuiDemoWindow.h"
#include "GUI/ImGuiWrap.h"

#include "Host.h"
#include "Renderer.h"
#include "Input.h"

#include "psx-utils/Sideload.h"

#include "psx/Bus.h"

#include "core/Parse.h"
#include "core/Log.h"
#include "core/StringHelpers.h"
#include "core/Helpers.h" // HP_UNUSED

#include "imgui.h"

#include <SDL3/SDL_main.h> // will #define main to something else
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h> // EXIT_FAILURE
#include <mutex>

struct CommandLineArgs
{
	const char* biosPath = nullptr;
	const char* discPath = nullptr;
	const char* sideloadExePath = nullptr;
	const char* memoryCardPath = nullptr;
	unsigned int windowWidth = 0;
	unsigned int windowHeight = 0;
	bool windowMaximised = false;
	bool respectDisplayDpiScale = false; // should be false for pixel perfect fonts
	bool amidogTestDebug = false;
};

struct HostControllerInput
{
	bool buttonSelect = false;  // PlayStation Select
	bool buttonL3 = false; // left stick press
	bool buttonR3 = false; // right stick press
	bool buttonStart = false; // PlayStation Start
	bool joypadUp = false;
	bool joypadRight = false;
	bool joypadDown = false;
	bool joypadLeft = false;
	bool buttonL2 = false;
	bool buttonR2 = false;
	bool buttonL1 = false;
	bool buttonR1 = false;
	bool buttonNorth = false; // PlayStation Triangle / Nintendo Y / Xbox Y
	bool buttonEast = false;  // PlayStation Circle / Nintendo A / Xbox B
	bool buttonSouth = false; // PlayStation Cross / Nintendo B / Xbox A
	bool buttonWest = false;  // PlayStation Square / Nintendo X / Xbox X

	// Analogue sticks
	float m_leftStickX = 0.0f;
	float m_leftStickY = 0.0f;
	float m_rightStickX = 0.0f;
	float m_rightStickY = 0.0f;
};

static const unsigned int kMaxControllers = 2;

struct HostInput
{
	HostControllerInput controllers[kMaxControllers];
};

static bool s_quitOnEscape = false;
static bool s_quit = false;
static bool s_mainMenuBarVisible = true;
static HostInput s_hostInput;
static bool s_hostLeftAnalogueStickToDpadInDigitalMode[kMaxControllers]{ true, true }; // Convert host left analogue stick to PSX digital DPAD input.

static void printUsage()
{
	puts("Usage: hopstation [options]\n\n");
	puts("Options:\n\n"
		"  --help                           Shows this message\n"
		"  --bios <path>                    Specify bios path. Default: bios/SCPH1001.bin\n"
		"  --disc <path>                    Insert disc (.cue file)\n"
		"  --exe <path>                     Sideload executable\n"
		"  --mcd <path>                     Load memory card from file\n"
		"  --amidog-debug                   Enable Amidog debug output for debugging test failures\n"
		"  -w --window-width                Window width. Default: 75%% display width\n"
		"  -h --window-height               Window height. Default: 75%% display height\n"
		"  -m --window-maximised            Maximise window. Default: false\n"
		"  -d --respect-display-dpi-scale   Default: false\n"
		"  --log-level <value>              Specify log level: 2 (trace), 1 (debug), 0 (info), -1 (warn), -2 (error) -3 (none)  Default: 0\n"
	);
}

static void parseCommandLineArgs(int argc, char** argv, CommandLineArgs& commandLineArgs)
{
	for (int i = 1; i < argc; i++)
	{
		const char* arg = argv[i];

		if (strcmp(arg, "--help") == 0)
		{
			printUsage();
			exit(EXIT_SUCCESS);
		}
		else if (strcmp(arg, "--log-level") == 0)
		{
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}

			arg = argv[++i];
			if (arg[0] == '-')
			{
				printUsage();
				exit(EXIT_FAILURE);
			}

			int logLevel;
			if (!ParseInt(arg, logLevel) || logLevel < LOG_LEVEL_MIN || logLevel > LOG_LEVEL_MAX)
			{
				LOG_ERROR("Invalid log-level value\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
			SetLogLevel(logLevel);
		}
		else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--window-width") == 0)
		{
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}

			arg = argv[++i];
			if (!ParseUnsignedInt(arg, commandLineArgs.windowWidth))
			{
				LOG_ERROR("Specified window width is invalid\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
		}
		else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--window-height") == 0)
		{
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}

			arg = argv[++i];
			if (!ParseUnsignedInt(arg, commandLineArgs.windowHeight))
			{
				LOG_ERROR("Specified window height is invalid\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
		}
		else if (strcmp(arg, "--maximised") == 0 || strcmp(arg, "-m") == 0)
		{
			commandLineArgs.windowMaximised = true;
		}
		else if (strcmp(arg, "--respect-display-dpi-scale") == 0 || strcmp(arg, "-d") == 0)
		{
			commandLineArgs.respectDisplayDpiScale = true;
		}
		else if (strcmp(arg, "--bios") == 0)
		{
			if (i + 1 == argc || commandLineArgs.biosPath)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}
			arg = argv[++i];
			commandLineArgs.biosPath = arg;
		}
		else if (strcmp(arg, "--disc") == 0)
		{
			if (i + 1 == argc || commandLineArgs.discPath)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}
			arg = argv[++i];
			commandLineArgs.discPath = arg;
		}
		else if (strcmp(arg, "--exe") == 0)
		{
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}
			arg = argv[++i];
			commandLineArgs.sideloadExePath = arg;
		}
		else if (strcmp(arg, "--mcd") == 0)
		{
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}
			arg = argv[++i];
			commandLineArgs.memoryCardPath = arg;
		}
		else if (strcmp(arg, "--amidog-debug") == 0)
		{
			commandLineArgs.amidogTestDebug = true;
		}
		else
		{
			LOG_ERROR("Unrecognised command line arg: %s\n", arg);
			printUsage();
			exit(EXIT_FAILURE);
		}
	}
}

// Maps [-1.0f,+1.0f] to [0,255]
static inline u8 stickFloatToU8(float val)
{
	if (val < -1.0f)
		val = -1.0f;
	if (val > 1.0f)
		val = 1.0f;
	u8 ret = (u8)((val + 1.0f) * 127.5f);
	return ret;
}

static void inputCallback(void* /*userdata*/)
{
	const HostControllerInput hostController0_prev = s_hostInput.controllers[0];

	HostControllerInput& hostController0 = s_hostInput.controllers[0];
	hostController0.buttonSelect = Input::GetKeyState(SDL_SCANCODE_BACKSPACE) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_BACK);
	hostController0.buttonL3 = Input::GetKeyState(SDL_SCANCODE_LCTRL) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_LEFT_STICK);
	hostController0.buttonR3 = Input::GetKeyState(SDL_SCANCODE_RCTRL) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
	hostController0.buttonStart = Input::GetKeyState(SDL_SCANCODE_RETURN) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_START);
	hostController0.joypadUp = Input::GetKeyState(SDL_SCANCODE_UP) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_DPAD_UP);
	hostController0.joypadRight = Input::GetKeyState(SDL_SCANCODE_RIGHT) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	hostController0.joypadDown = Input::GetKeyState(SDL_SCANCODE_DOWN) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	hostController0.joypadLeft = Input::GetKeyState(SDL_SCANCODE_LEFT) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
	if (s_hostLeftAnalogueStickToDpadInDigitalMode[0] && Host::GetBus().GetSIO().GetPort(0).GetController().GetType() == Controller::Type::Digital)
	{
		hostController0.joypadUp |= Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTY) < -0.5f;;
		hostController0.joypadRight |= Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTX) > 0.5f;
		hostController0.joypadDown |= Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTY) > 0.5f;
		hostController0.joypadLeft |= Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTX) < -0.5f;
	}
	hostController0.buttonL2 = Input::GetKeyState(SDL_SCANCODE_2) || Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f;
	hostController0.buttonR2 = Input::GetKeyState(SDL_SCANCODE_4) || Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f;
	hostController0.buttonL1 = Input::GetKeyState(SDL_SCANCODE_1) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
	hostController0.buttonR1 = Input::GetKeyState(SDL_SCANCODE_3) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	hostController0.buttonNorth = Input::GetKeyState(SDL_SCANCODE_E) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_NORTH); // PlayStation Triangle / Nintendo Y / Xbox Y
	hostController0.buttonEast = Input::GetKeyState(SDL_SCANCODE_D) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_EAST);  // PlayStation Circle / Nintendo A / Xbox B
	hostController0.buttonSouth = Input::GetKeyState(SDL_SCANCODE_X) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_SOUTH); // PlayStation Cross / Nintendo B / Xbox A
	hostController0.buttonWest = Input::GetKeyState(SDL_SCANCODE_S) || Input::GetButtonState(0, SDL_GAMEPAD_BUTTON_WEST);  // PlayStation Square / Nintendo X / Xbox X
	hostController0.m_leftStickX = Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTX);
	hostController0.m_leftStickY = Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_LEFTY);
	hostController0.m_rightStickX = Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_RIGHTX);
	hostController0.m_rightStickY = Input::GetAxisValue(0, SDL_GAMEPAD_AXIS_RIGHTY);

	// #TODO: Implement second controller input
	const HostControllerInput hostController1_prev = s_hostInput.controllers[1];
	HostControllerInput& hostController1 = s_hostInput.controllers[1];
	hostController1.buttonSelect = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_BACK);
	hostController1.buttonL3 = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_LEFT_STICK);
	hostController1.buttonR3 = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
	hostController1.buttonStart = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_START);
	hostController1.joypadUp = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_DPAD_UP);
	hostController1.joypadRight = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
	hostController1.joypadDown = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
	hostController1.joypadLeft = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
	if (s_hostLeftAnalogueStickToDpadInDigitalMode[1] && Host::GetBus().GetSIO().GetPort(1).GetController().GetType() == Controller::Type::Digital)
	{
		hostController1.joypadUp |= Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTY) < -0.5f;;
		hostController1.joypadRight |= Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTX) > 0.5f;
		hostController1.joypadDown |= Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTY) > 0.5f;
		hostController1.joypadLeft |= Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTX) < -0.5f;
	}
	hostController1.buttonL2 = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f;
	hostController1.buttonR2 = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f;
	hostController1.buttonL1 = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
	hostController1.buttonR1 = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	hostController1.buttonNorth = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_NORTH); // PlayStation Triangle / Nintendo Y / Xbox Y
	hostController1.buttonEast = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_EAST);  // PlayStation Circle / Nintendo A / Xbox B
	hostController1.buttonSouth = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_SOUTH); // PlayStation Cross / Nintendo B / Xbox A
	hostController1.buttonWest = Input::GetButtonState(1, SDL_GAMEPAD_BUTTON_WEST);  // PlayStation Square / Nintendo X / Xbox X
	hostController1.m_leftStickX = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTX);
	hostController1.m_leftStickY = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_LEFTY);
	hostController1.m_rightStickX = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_RIGHTX);
	hostController1.m_rightStickY = Input::GetAxisValue(1, SDL_GAMEPAD_AXIS_RIGHTY);

	// Pass input state changes to emulator
	SIO& sio = Host::GetBus().GetSIO();

	Controller& controller0 = sio.GetPort(0).GetController();
	if (hostController0.buttonSelect != hostController0_prev.buttonSelect)
		hostController0.buttonSelect ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::SelectButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::SelectButton);
	if (hostController0.buttonL3 != hostController0_prev.buttonL3)
		hostController0.buttonL3 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::L3) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::L3);
	if (hostController0.buttonR3 != hostController0_prev.buttonR3)
		hostController0.buttonR3 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::R3) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::R3);
	if (hostController0.buttonStart != hostController0_prev.buttonStart)
		hostController0.buttonStart ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::StartButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::StartButton);
	if (hostController0.joypadUp != hostController0_prev.joypadUp)
		hostController0.joypadUp ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::JoypadUp) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::JoypadUp);
	if (hostController0.joypadRight != hostController0_prev.joypadRight)
		hostController0.joypadRight ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::JoypadRight) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::JoypadRight);
	if (hostController0.joypadDown != hostController0_prev.joypadDown)
		hostController0.joypadDown ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::JoypadDown) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::JoypadDown);
	if (hostController0.joypadLeft != hostController0_prev.joypadLeft)
		hostController0.joypadLeft ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::JoypadLeft) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::JoypadLeft);
	if (hostController0.buttonL2 != hostController0_prev.buttonL2)
		hostController0.buttonL2 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::L2Button) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::L2Button);
	if (hostController0.buttonR2 != hostController0_prev.buttonR2)
		hostController0.buttonR2 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::R2Button) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::R2Button);
	if (hostController0.buttonL1 != hostController0_prev.buttonL1)
		hostController0.buttonL1 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::L1Button) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::L1Button);
	if (hostController0.buttonR1 != hostController0_prev.buttonR1)
		hostController0.buttonR1 ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::R1Button) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::R1Button);
	if (hostController0.buttonNorth != hostController0_prev.buttonNorth)
		hostController0.buttonNorth ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::TriangleButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::TriangleButton);
	if (hostController0.buttonEast != hostController0_prev.buttonEast)
		hostController0.buttonEast ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::CircleButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::CircleButton);
	if (hostController0.buttonSouth != hostController0_prev.buttonSouth)
		hostController0.buttonSouth ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::CrossButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::CrossButton);
	if (hostController0.buttonWest != hostController0_prev.buttonWest)
		hostController0.buttonWest ? controller0.DigitalSwitchDown(Controller::DigitalSwitch::SquareButton) : controller0.DigitalSwitchUp(Controller::DigitalSwitch::SquareButton);
	if (hostController0.m_leftStickX != hostController0_prev.m_leftStickX)
		controller0.SetLeftJoyX(stickFloatToU8(hostController0.m_leftStickX));
	if (hostController0.m_leftStickY != hostController0_prev.m_leftStickY)
		controller0.SetLeftJoyY(stickFloatToU8(hostController0.m_leftStickY));
	if (hostController0.m_rightStickX != hostController0_prev.m_rightStickX)
		controller0.SetRightJoyX(stickFloatToU8(hostController0.m_rightStickX));
	if (hostController0.m_rightStickY != hostController0_prev.m_rightStickY)
		controller0.SetRightJoyY(stickFloatToU8(hostController0.m_rightStickY));

	Controller& controller1 = sio.GetPort(1).GetController();
	if (hostController1.buttonSelect != hostController1_prev.buttonSelect)
		hostController1.buttonSelect ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::SelectButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::SelectButton);
	if (hostController1.buttonL3 != hostController1_prev.buttonL3)
		hostController1.buttonL3 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::L3) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::L3);
	if (hostController1.buttonR3 != hostController1_prev.buttonR3)
		hostController1.buttonR3 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::R3) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::R3);
	if (hostController1.buttonStart != hostController1_prev.buttonStart)
		hostController1.buttonStart ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::StartButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::StartButton);
	if (hostController1.joypadUp != hostController1_prev.joypadUp)
		hostController1.joypadUp ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::JoypadUp) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::JoypadUp);
	if (hostController1.joypadRight != hostController1_prev.joypadRight)
		hostController1.joypadRight ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::JoypadRight) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::JoypadRight);
	if (hostController1.joypadDown != hostController1_prev.joypadDown)
		hostController1.joypadDown ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::JoypadDown) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::JoypadDown);
	if (hostController1.joypadLeft != hostController1_prev.joypadLeft)
		hostController1.joypadLeft ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::JoypadLeft) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::JoypadLeft);
	if (hostController1.buttonL2 != hostController1_prev.buttonL2)
		hostController1.buttonL2 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::L2Button) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::L2Button);
	if (hostController1.buttonR2 != hostController1_prev.buttonR2)
		hostController1.buttonR2 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::R2Button) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::R2Button);
	if (hostController1.buttonL1 != hostController1_prev.buttonL1)
		hostController1.buttonL1 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::L1Button) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::L1Button);
	if (hostController1.buttonR1 != hostController1_prev.buttonR1)
		hostController1.buttonR1 ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::R1Button) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::R1Button);
	if (hostController1.buttonNorth != hostController1_prev.buttonNorth)
		hostController1.buttonNorth ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::TriangleButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::TriangleButton);
	if (hostController1.buttonEast != hostController1_prev.buttonEast)
		hostController1.buttonEast ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::CircleButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::CircleButton);
	if (hostController1.buttonSouth != hostController1_prev.buttonSouth)
		hostController1.buttonSouth ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::CrossButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::CrossButton);
	if (hostController1.buttonWest != hostController1_prev.buttonWest)
		hostController1.buttonWest ? controller1.DigitalSwitchDown(Controller::DigitalSwitch::SquareButton) : controller1.DigitalSwitchUp(Controller::DigitalSwitch::SquareButton);
	if (hostController1.m_leftStickX != hostController1_prev.m_leftStickX)
		controller1.SetLeftJoyX(stickFloatToU8(hostController1.m_leftStickX));
	if (hostController1.m_leftStickY != hostController1_prev.m_leftStickY)
		controller1.SetLeftJoyY(stickFloatToU8(hostController1.m_leftStickY));
	if (hostController1.m_rightStickX != hostController1_prev.m_rightStickX)
		controller1.SetRightJoyX(stickFloatToU8(hostController1.m_rightStickX));
	if (hostController1.m_rightStickY != hostController1_prev.m_rightStickY)
		controller1.SetRightJoyY(stickFloatToU8(hostController1.m_rightStickY));
}

//
// Returns menu bar height, which can be used to position items underneath the menu so they are not partially obscured.
//
static unsigned int showMainMenuBar(SDL_Window* pWindow)
{
	if (!ImGui::BeginMainMenuBar())
		return 0;

	Bus& bus = Host::GetBus();

	// File
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Insert disc..."))
		{
			// This call returns immediately
			InsertDiscDialog::ShowOpenFileDialog(pWindow);
		}
		if (ImGui::MenuItem("Eject disc", /*shortcut*/nullptr, /*selected*/false, /*enabled*/bus.GetCDROM().IsDiscInserted()))
		{
			bus.GetCDROM().EjectDisc();
		}
		if (ImGui::MenuItem("Sideload executable..."))
		{
			// This call returns immediately
			SideloadDialog::ShowOpenFileDialog(pWindow);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Save display..."))
		{
			const GPU& gpu = bus.GetGPU();
			SnapshotDialog::ShowSaveFileDialog(pWindow, gpu.GetDisplayStartX(), gpu.GetDisplayStartY(), gpu.GetHorizontalResolution(), gpu.GetVerticalResolution(), gpu.GetDisplayFormat());
		}
		if (ImGui::MenuItem("Save VRAM (16 bpp)..."))
		{
			SnapshotDialog::ShowSaveFileDialog(pWindow, 0, 0, kVRAMWidth16bpp, kVRAMHeightLines, DisplayFormat::A1B5G5R5);
		}
		if (ImGui::MenuItem("Save VRAM (24 bpp)..."))
		{
			static constexpr unsigned int kVRAMWidth24bpp = kVRAMWidthBytes / 3; // 682.66 rounded down to 682
			SnapshotDialog::ShowSaveFileDialog(pWindow, 0, 0, kVRAMWidth24bpp, kVRAMHeightLines, DisplayFormat::B8G8R8);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Exit"))
			s_quit = true;
		ImGui::EndMenu();
	}

	// Emulator
	if (ImGui::BeginMenu("Emulator"))
	{
		if (ImGui::MenuItem("Reset", "Ctrl+Shift+F5")) // #TODO: Appropriate shortcut
			Host::ResetEmulator();

		ImGui::MenuItem("Pause", "F5", &Host::s_paused);

		ImGui::Separator();

		ImGui::MenuItem("Draw display", nullptr, &Host::s_drawDisplay);
		if (ImGui::BeginMenu("Display scale"))
		{
			if (ImGui::MenuItem("x1", /*shortcut*/nullptr, /*selected*/Host::s_displayScale == 1))
				Host::s_displayScale = 1;
			if (ImGui::MenuItem("x2", /*shortcut*/nullptr, /*selected*/Host::s_displayScale == 2))
				Host::s_displayScale = 2;
			if (ImGui::MenuItem("x3", /*shortcut*/nullptr, /*selected*/Host::s_displayScale == 3))
				Host::s_displayScale = 3;
			ImGui::EndMenu();
		}
		ImGui::MenuItem("Draw overscan", nullptr, &Host::s_drawOverscan);
		ImGui::MenuItem("Draw VRAM", nullptr, &Host::s_drawVRAM);
		ImGui::EndMenu();
	}

	// Audio
	if (ImGui::BeginMenu("Audio"))
	{
		ImGui::MenuItem("Test tone", nullptr, &Host::s_playTestTone);
		ImGui::EndMenu();
	}

	// Controllers
	if (ImGui::BeginMenu("Controllers"))
	{
		SIO& sio = bus.GetSIO();
		for (unsigned int portIndex = 0; portIndex < SIO::kNumPorts; portIndex++)
		{
			char label[32];
			unsigned int portNumber = 1 + portIndex; // 1-based for user friendliness
			SafeSnprintf(label, sizeof(label), "Controller %u", portNumber);
			if (ImGui::BeginMenu(label))
			{
				ControllerPort& port = sio.GetPort(portIndex);
				bool controllerConnected = port.IsControllerConnected();
				Controller& controller = port.GetController();
				Controller::Type controllerType = controller.GetType();
				if (ImGui::MenuItem("Digital", nullptr, /*selected*/controllerConnected && controllerType == Controller::Type::Digital))
				{
					if (controllerType != Controller::Type::Digital)
						controller.SetType(Controller::Type::Digital);
					if (!controllerConnected)
						port.SetControllerConnected(true);
				}
				if (ImGui::MenuItem("Analogue", nullptr, /*selected*/controllerConnected && controller.GetType() == Controller::Type::Analogue))
				{
					if (controllerType != Controller::Type::Analogue)
						controller.SetType(Controller::Type::Analogue);
					if (!controllerConnected)
						port.SetControllerConnected(true);
				}
				if (ImGui::MenuItem("None", nullptr, /*selected*/!controllerConnected))
				{
					port.SetControllerConnected(false);
				}

				ImGui::Separator();

				if (ImGui::MenuItem("Left stick = DPAD in digital mode", nullptr, s_hostLeftAnalogueStickToDpadInDigitalMode[portIndex]))
					s_hostLeftAnalogueStickToDpadInDigitalMode[portIndex] = !s_hostLeftAnalogueStickToDpadInDigitalMode[portIndex];

				ImGui::EndMenu();
			}
		}

		ImGui::EndMenu();
	}

	// Memory Cards
	if (ImGui::BeginMenu("Memory Cards"))
	{
		SIO& sio = bus.GetSIO();
		for (unsigned int portIndex = 0; portIndex < SIO::kNumPorts; portIndex++)
		{
			char label[32];
			unsigned int memCardNumber = 1 + portIndex; // 1-based for user friendliness
			SafeSnprintf(label, sizeof(label), "Memory Card %u", memCardNumber);
			if (ImGui::BeginMenu(label))
			{
				ControllerPort& port = sio.GetPort(portIndex);
				MemoryCard& card = port.GetMemoryCard();
				bool cardInserted = port.IsMemoryCardInserted();
				if (ImGui::MenuItem("Load..."))
				{
					// This call returns immediately
					MemoryCardFileDialog::ShowOpenFileDialog(portIndex, pWindow);
				}
				if (ImGui::MenuItem("Save...", nullptr, false, cardInserted))
				{
					// This call returns immediately
					MemoryCardFileDialog::ShowSaveFileDialog(portIndex, pWindow);
				}
				if (ImGui::MenuItem("Insert", nullptr, false, !cardInserted))
				{
					port.SetMemoryCardInserted(true);
				}
				if (ImGui::MenuItem("Eject", nullptr, false, cardInserted))
				{
					port.SetMemoryCardInserted(false);
				}
				if (ImGui::MenuItem("Format", nullptr, false, cardInserted))
				{
					card.Format();
				}
				ImGui::EndMenu();
			}
		}
		ImGui::EndMenu();
	}

	// Logging
	if (ImGui::BeginMenu("Logging"))
	{
		ImGui::MenuItem("Log GP0", nullptr, &s_logGP0);
		ImGui::MenuItem("Log GP1", nullptr, &s_logGP1);
		ImGui::MenuItem("Log GPUREAD", nullptr, &s_logGPUREAD);
		ImGui::MenuItem("Log GPUSTAT", nullptr, &s_logGPUSTAT);
		ImGui::MenuItem("Log Unimplemented GPU Features", nullptr, &s_logUnimplementedGpuFeatures);
		ImGui::Separator();
		ImGui::MenuItem("Log DMA Registers", nullptr, &s_logDMARegisterAccess);
		ImGui::MenuItem("Log DMA", nullptr, &s_logDMA);
		ImGui::MenuItem("Log Interrupt Registers", nullptr, &s_logInterruptRegisterAccess);
		ImGui::MenuItem("Log Interrupts", nullptr, &s_logInterrupts);
		ImGui::MenuItem("Log Timers", nullptr, &s_logTimers);
		ImGui::MenuItem("Log Timer Reads", nullptr, &s_logTimerReads);
		ImGui::MenuItem("Log CDROM", nullptr, &s_logCDROM);
		ImGui::MenuItem("Log SPU", nullptr, &g_logSPU);
		ImGui::MenuItem("Log SIO", nullptr, &g_logSIO);
		ImGui::MenuItem("Log Memory Card", nullptr, &g_logMemoryCard);
		ImGui::Separator();
		ImGui::MenuItem("Log Memory Control Registers", nullptr, &s_logMemoryControlRegisterAccess);
		ImGui::MenuItem("Log Cache Control Registers", nullptr, &s_logCacheControlRegisterAccess);
		ImGui::MenuItem("Log Expansion 2 Registers", nullptr, &s_logExpansion2RegisterAccess);
		ImGui::EndMenu();
	}

	// Window
	if (ImGui::BeginMenu("Window"))
	{
		ImGui::MenuItem("CPU Window", nullptr, &CPUWindow::s_visible);
		ImGui::MenuItem("DMA Window", nullptr, &DMAWindow::s_visible);
		ImGui::MenuItem("GPU Window", nullptr, &GPUWindow::s_visible);
		ImGui::MenuItem("SPU Window", nullptr, &SPUWindow::s_visible);
		ImGui::MenuItem("CD Window", nullptr, &CDWindow::s_visible);
		ImGui::MenuItem("CDROM Window", nullptr, &CDROMWindow::s_visible);
		ImGui::MenuItem("Memory Card Window", nullptr, &MemoryCardWindow::s_visible);
		ImGui::MenuItem("Host Window", nullptr, &HostWindow::s_visible);
		ImGui::EndMenu();
	}

	// Help
	if (ImGui::BeginMenu("Help"))
	{
		ImGui::MenuItem("Show ImGui Demo Window", nullptr, &ImGuiDemoWindow::s_showDemoWindow);
		ImGui::MenuItem("Show ImGui About Window", nullptr, &ImGuiDemoWindow::s_showAboutWindow);
		ImGui::EndMenu();
	}

	// Show CDROM head position to right of main menu bar for convenience.
	// Hacky right align. See https://github.com/ocornut/imgui/issues/5875
	ImVec2 textSize = ImGui::CalcTextSize("Head MSF: 00:00:00 LBA: 0x00000000___"); // few extra chars
	if (textSize.x <= ImGui::GetContentRegionAvail().x) // Don't show if not enough space.
	{
		ImGui::SameLine(ImGui::GetWindowWidth() - textSize.x - ImGui::GetStyle().FramePadding.x);

		unsigned int m, s, f;
		u32 headLBA = Host::GetBus().GetCDROM().GetHeadLBA();
		CD::LBAtoMSF(headLBA, m, s, f);
		ImGui::BeginDisabled(true);
		ImGui::Text("Head MSF: %02u:%02u:%02u LBA: 0x%08X\n", m, s, f, headLBA); // MSF conventionally printed in decimal
		ImGui::EndDisabled();
	}

	float menuBarHeight = ImGui::GetWindowSize().y;
	ImGui::EndMainMenuBar();
	return (unsigned int)menuBarHeight;
}

static void updateGUI()
{
	Bus& bus = Host::GetBus();

	InsertDiscDialog::Update();
	MemoryCardFileDialog::Update();
	SnapshotDialog::Update(bus.GetGPU());
	SideloadDialog::Update();
	ImGuiDemoWindow::Update();
	CDROMWindow::Update(bus.GetCDROM());
	CDWindow::Update(bus.GetCDROM().GetCD());
	CPUWindow::Update(bus.GetCPU());
	GPUWindow::Update(bus.GetGPU());
	SPUWindow::Update(bus.GetSPU());
	DMAWindow::Update(bus.GetDMAC());
	MemoryCardWindow::Update(bus.GetSIO());
	HostWindow::Update();
}

int main(int argc, char** argv)
{
	CommandLineArgs commandLineArgs;
	parseCommandLineArgs(argc, argv, commandLineArgs);

	// Initialise critical sub-systems
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
	{
		LOG_ERROR("SDL_Init() failed: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	const int compiled = SDL_VERSION;  /* hardcoded number from SDL headers */
	const int linked = SDL_GetVersion();  /* reported by linked SDL library */

	LOG_INFO("Compiled against SDL version %d.%d.%d\n",
		SDL_VERSIONNUM_MAJOR(compiled),
		SDL_VERSIONNUM_MINOR(compiled),
		SDL_VERSIONNUM_MICRO(compiled));

	LOG_INFO("Linked against SDL version %d.%d.%d\n",
		SDL_VERSIONNUM_MAJOR(linked),
		SDL_VERSIONNUM_MINOR(linked),
		SDL_VERSIONNUM_MICRO(linked));

	// Initialise audio sub-system separately to support platforms without an audio device i.e. WSL
	const bool audioSubSystemInitialised = SDL_InitSubSystem(SDL_INIT_AUDIO);
	if (audioSubSystemInitialised)
		LOG_INFO("[Host] SDL audio subsystem initialised\n");
	else
		LOG_INFO("[Host] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\nAudio will not be available.\n", SDL_GetError());

	const SDL_DisplayID displayID = SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* pDisplayMode = SDL_GetCurrentDisplayMode(displayID); // const pointer owned by SDL and does not need to be freed
	if (!pDisplayMode)
	{
		LOG_ERROR("Failed to get current display mode: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	LOG_INFO("[Host] Primary display: \"%s\" %dx%d %.2f%% @ %f Hz (%d/%d)\n",
		SDL_GetDisplayName(displayID),
		pDisplayMode->w, pDisplayMode->h, pDisplayMode->pixel_density * 100.0f,
		pDisplayMode->refresh_rate, pDisplayMode->refresh_rate_numerator, pDisplayMode->refresh_rate_denominator);

	const double displayFramePeriodSeconds = (double)pDisplayMode->refresh_rate_denominator / (double)pDisplayMode->refresh_rate_numerator;

	SDL_Rect displayBounds;
	if (!SDL_GetDisplayBounds(displayID, &displayBounds))
	{
		LOG_ERROR("SDL_GetDisplayBounds failed: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	// Size window to show display above VRAM + ImGui main menu
	static const unsigned int kDefaultWindowWidth = kVRAMWidth16bpp + 512 + 128; // extra for ImGui windows
	static const unsigned int kDefaultWindowHeight = 20 + 64 + GPU::kMaxDisplayHeightPixels + kVRAMHeightLines + 32; // + some extra for main menu + overscan

	// Default window size
	int windowWidth = (int)kDefaultWindowWidth;
	int windowHeight = (int)kDefaultWindowHeight;
	if (commandLineArgs.windowWidth > 0)
		windowWidth = commandLineArgs.windowWidth;
	if (commandLineArgs.windowHeight > 0)
		windowHeight = commandLineArgs.windowHeight;

	// Create hidden window and show it later when fully configured.
	// #TODO: Pass SDL_WINDOW_VULKAN if decide to use Vulkan?
	// #TODO: Pass SDL_WINDOW_METAL on macOS
	Uint64 windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (commandLineArgs.windowMaximised)
		windowFlags |= SDL_WINDOW_MAXIMIZED;

	SDL_Window* pWindow = SDL_CreateWindow("HopStation", windowWidth, windowHeight, windowFlags);
	if (!pWindow)
	{
		LOG_ERROR("SDL_CreateWindow() failed: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	// Default position is centred on the primary display
	// SDL_WINDOWPOS_CENTERED is no good because it doesn't take display index into account.
	int windowX = displayBounds.x + (displayBounds.w - windowWidth) / 2; // centre
	int windowY = displayBounds.y + (displayBounds.h - windowHeight) / 2;
	SDL_SetWindowPosition(pWindow, windowX, windowY);

	// Small delay before showing window can help with X11 stability in WSL
	SDL_Delay(50);

	if (!SDL_ShowWindow(pWindow))
	{
		LOG_ERROR("SDL_ShowWindow() failed: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}

	Input::Init();

	Input::SetInputCallback(inputCallback, &s_hostInput);

	if (!Host::Init(pWindow, audioSubSystemInitialised, commandLineArgs.biosPath))
	{
		LOG_ERROR("Failed to initialise host\n");
		return EXIT_FAILURE;
	}

	const float displayScale = SDL_GetDisplayContentScale(displayID);
	if (!ImGuiWrap::Init(pWindow, Renderer::GetDevice(), commandLineArgs.respectDisplayDpiScale ? displayScale : 1.0f))
	{
		LOG_ERROR("ImGuiWrap::Init() failed\n");
		return EXIT_FAILURE;
	}

	// Connect a controller to port 0
	// #TODO: Make this a command line option or load from settings
	ControllerPort& port0 = Host::GetBus().GetSIO().GetPort(0);
	port0.SetControllerConnected(true);

	if (commandLineArgs.memoryCardPath)
	{
		MemoryCard& memoryCard = port0.GetMemoryCard();
		if (!memoryCard.LoadFromFile(commandLineArgs.memoryCardPath))
		{
			LOG_ERROR("Failed to load memory card image: %s\n", commandLineArgs.memoryCardPath);
			return EXIT_FAILURE;
		}
		port0.SetMemoryCardInserted(true);
	}

	if (commandLineArgs.discPath)
	{
		CD& cd = Host::GetCD();
		if (!cd.LoadFromFile(commandLineArgs.discPath))
		{
			LOG_ERROR("Failed to load disc image: %s\n", commandLineArgs.discPath);
			return EXIT_FAILURE;
		}
		Host::GetBus().GetCDROM().InsertDisc(cd);
	}

	if (commandLineArgs.sideloadExePath)
	{
		if (!Sideload::SideloadExecutable(commandLineArgs.sideloadExePath, Host::GetBus(), /*pTTYLogger*/nullptr))
		{
			LOG_ERROR("Failed to sideload executable: %s\n", commandLineArgs.sideloadExePath);
			return EXIT_FAILURE;
		}

		if (commandLineArgs.amidogTestDebug)
			Sideload::EnableAmidogTestDebugOutput(Host::GetBus());
	}

	// Main loop
	s_quit = false;
	const Uint64 kCountsPerSecond = SDL_GetPerformanceFrequency();
	Uint64 prevTime = SDL_GetPerformanceCounter();

	while (!s_quit)
	{
		Input::FrameStart();

		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			Input::HandleSDLEvent(event);
			ImGuiWrap::ProcessEvent(event);

			if (event.type == SDL_EVENT_QUIT)
			{
				s_quit = true;
			}
			else if (event.type == SDL_EVENT_KEY_DOWN)
			{
				const bool ctrl = event.key.mod & SDL_KMOD_CTRL;
				const bool shift = event.key.mod & SDL_KMOD_SHIFT;
				const bool alt = event.key.mod & SDL_KMOD_ALT;
				const bool gui = event.key.mod & SDL_KMOD_GUI;

				if (s_quitOnEscape && event.key.key == SDLK_ESCAPE && !ctrl && !shift && !alt && !gui)
				{
					LOG_INFO("Escape pressed, quitting\n");
					s_quit = true;
				}

				if (event.key.key == SDLK_TAB && !ctrl && !shift && !alt && !gui)
					s_mainMenuBarVisible = !s_mainMenuBarVisible;

				if (event.key.key == SDLK_F5)
				{
					if (!ctrl && !shift && !alt && !gui) // F5 alone
						Host::s_paused = !Host::s_paused;
					else if (ctrl && shift && !alt && !gui) // Ctrl+Shift+F5
						Host::ResetEmulator();
				}
			}
			else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(pWindow))
			{
				s_quit = true;
			}
		}

		if (SDL_GetWindowFlags(pWindow) & SDL_WINDOW_MINIMIZED)
		{
			SDL_Delay(10);
			continue;
		}

		ImGuiWrap::NewFrame();

		Uint64 currentTime = SDL_GetPerformanceCounter();
		Uint64 deltaTime = currentTime - prevTime;
		double frameTimeSeconds = (double)deltaTime / kCountsPerSecond;
		prevTime = currentTime;
		if (frameTimeSeconds > 0.5)
			frameTimeSeconds = 1.0f / pDisplayMode->refresh_rate; // Probably debugging.

		unsigned int menuBarHeight = 0;
		if (s_mainMenuBarVisible)
			menuBarHeight = showMainMenuBar(pWindow);

		Host::Update(displayFramePeriodSeconds, frameTimeSeconds);
		updateGUI();
		Host::Render(menuBarHeight);
	}

	ImGuiWrap::Shutdown();
	Host::Shutdown();

	Input::Shutdown();

	SDL_DestroyWindow(pWindow);
	pWindow = nullptr;

	SDL_Quit();

	return EXIT_SUCCESS;
}
