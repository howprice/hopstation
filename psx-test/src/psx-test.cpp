#include "psx-utils/Snapshot.h"

#include "psx-utils/TTYLogger.h"
#include "psx-utils/Sideload.h"
#include "psx-utils/R3000Utils.h"

#include "psx/R3000Dasm.h"
#include "psx/Bus.h"

#include "core/Parse.h"
#include "core/ArrayHelpers.h"
#include "core/hp_assert.h"
#include "core/Log.h"
#include "core/Types.h"
#include "core/Helpers.h" // HP_UNUSED

#include <stdlib.h> // EXIT_SUCCESS, EXIT_FAILURE
#include <string.h> // strcmp

struct CommandLineArgs
{
	const char* biosPath = nullptr;
	const char* dscPath = nullptr;
	const char* sideloadExePath = nullptr;
	bool amidogCpuTestMode = false;
	bool amidogTestDebug = false;
	bool saveCpuTestVRAM = false;
	u32 saveVramPC = 0xffffffff;
	bool quitOnSave = false;

	// Two cycles per instruction (CPI) is apparantly a good approximation and will allow many games to run.
	// However, Jakub's timers test assumes one cycle per instruction.
	unsigned int cyclesPerInstruction = 2;
};

static bool s_logCpuState = false;
static bool s_logDisassembly = false;

// Stats
static unsigned int s_testErrorCount = 0;

static void printUsage()
{
	puts("Usage: psx [options]\n\n");
	puts("e.g. psx -exe test/amidog/psxtest_cpu.exe\n");
	puts("e.g. psx -disc \"Mortal Kombat II(Japan) (Track 1).bin\"\n\n");
	puts("Options:\n"
		"  --help                         Shows this message\n"
		"  --bios <path>                  Specify bios path. Default: bios/SCPH1001.bin\n"
		"  --disc <path>                  Insert disc (.bin)\n"
		"  --exe <path>                   Sideload executable\n"
		"  --amidog-cpu-test              Exit when Amidog CPU test completes\n"
		"  --amidog-debug                 Enable Amidog debug output for debugging test failures\n"
		"  --save-vram-pc <0x-------->    Save VRAM first time PC reaches specified value\n"
		"  --quit-on-save                 Quit after saving VRAM to file\n"
		"  --log-level <value>            Specify log level: 2 (trace), 1 (debug), 0 (info), -1 (warn), -2 (error) -3 (none)  Default: 0\n"
		"  --log-gp0                      Log GPU GP0 port writes\n"
		"  --log-gp1                      Log GPU GP1 port writes\n"
		"  --log-gpuread                  Log GPU GPUREAD port reads\n"
		"  --log-gpustat                  Log GPU GPUSTAT port reads\n"
		"  --log-irq                      Log IRQs\n"
		"  --log-dma                      Log DMA\n"
		"  --log-dma-reg                  Log DMA register access\n"
		"  --log-timers                   Log Timers\n"
		"  --log-timer-reads              Log Timer value reads\n"
		"  --log-cdrom                    Log CDROM\n"
		"  --log-spu                      Log SPU\n"
		"  --cycles-per-instruction <val> Set CPU cycles per instruction (2 for compat, 1 for Jakub timers test)\n"
		"  --cpi                          Alias for --cycles-per-instruction\n"
	);
}

static void parseCommandLine(int argc, char** argv, CommandLineArgs& commandLineArgs)
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
				fprintf(stderr, "ERROR: Invalid log-level value\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
			SetLogLevel(logLevel);
		}
		else if (strcmp(arg, "--log-gp0") == 0)
		{
			s_logGP0 = true;
		}
		else if (strcmp(arg, "--log-gp1") == 0)
		{
			s_logGP1 = true;
		}
		else if (strcmp(arg, "--log-gpuread") == 0)
		{
			s_logGPUREAD = true;
		}
		else if (strcmp(arg, "--log-gpustat") == 0)
		{
			s_logGPUSTAT = true;
		}
		else if (strcmp(arg, "--log-irq") == 0)
		{
			s_logInterrupts = true;
		}
		else if (strcmp(arg, "--log-dma") == 0)
		{
			s_logDMA = true;
		}
		else if (strcmp(arg, "--log-dma-reg") == 0)
		{
			s_logDMARegisterAccess = true;
		}
		else if (strcmp(arg, "--log-timers") == 0)
		{
			s_logTimers = true;
		}
		else if (strcmp(arg, "--log-timer-reads") == 0)
		{
			s_logTimerReads = true;
		}
		else if (strcmp(arg, "--log-cdrom") == 0)
		{
			s_logCDROM = true;
		}
		else if (strcmp(arg, "--log-spu") == 0)
		{
			g_logSPU = true;
		}
		else if (strcmp(arg, "--amidog-cpu-test") == 0)
		{
			commandLineArgs.amidogCpuTestMode = true;
		}
		else if (strcmp(arg, "--amidog-debug") == 0)
		{
			commandLineArgs.amidogTestDebug = true;
		}
		else if (strcmp(arg, "--save-vram-pc") == 0)
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

			u32 pc;
			if (!ParseHexUnsignedInt(arg, pc))
			{
				fprintf(stderr, "ERROR: Invalid save-vram-pc value\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
			commandLineArgs.saveVramPC = pc;

		}
		else if (strcmp(arg, "--cycles-per-instruction") == 0 || strcmp(arg, "--cpi") == 0)
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

			if (!ParseUnsignedInt(arg, commandLineArgs.cyclesPerInstruction))
			{
				fprintf(stderr, "ERROR: Invalid --cycles-per-instruction / --cpi value\n");
				printUsage();
				exit(EXIT_FAILURE);
			}
		}
		else if (strcmp(arg, "--quit-on-save") == 0)
		{
			commandLineArgs.quitOnSave = true;
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
			if (i + 1 == argc)
			{
				printUsage();
				exit(EXIT_FAILURE);
			}
			arg = argv[++i];
			commandLineArgs.dscPath = arg;
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
		else
		{
			LOG_ERROR("Unrecognised command line arg: %s\n", arg);
			printUsage();
			exit(EXIT_FAILURE);
		}
	}
}

static void amidogCpuTestTTTFlushCallback(const char* text)
{
	// If a test fails the line contains "error" e.g. "bgezal_b value error @ 0,1: got 00000000 wanted ffffffff"
	if (strstr(text, "error") != nullptr)
		s_testErrorCount++;

	// On success, prints "Result: 00000101"
	// On failure, prints "Result: 00000X0Y" where either X or Y is not 1.
	if (strncmp(text, "Result: ", 8) == 0)
	{
		if (strcmp(text + 8, "00000101\n") == 0)
		{
			LOG_INFO("Amidog CPU test passed\n");
			HP_ASSERT(s_testErrorCount == 0);
			exit(EXIT_SUCCESS);
		}
		else
		{
			LOG_ERROR("Amidog CPU test failed: %u errors\n", s_testErrorCount);
			HP_ASSERT(s_testErrorCount > 0);
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char** argv)
{
	CommandLineArgs commandLineArgs;
	parseCommandLine(argc, argv, commandLineArgs);

	if (commandLineArgs.biosPath == nullptr)
		commandLineArgs.biosPath = "bios/SCPH1001.bin";

	CD* pCD = nullptr;
	if (commandLineArgs.dscPath)
	{
		pCD = new CD();
		if (!pCD->LoadFromFile(commandLineArgs.dscPath))
		{
			LOG_ERROR("Failed to load disc image: %s\n", commandLineArgs.dscPath);
			return EXIT_FAILURE;
		}
	}

	Bus bus;
	if (!bus.GetBIOS().Load(commandLineArgs.biosPath))
	{
		LOG_ERROR("Failed to load BIOS\n");
		return EXIT_FAILURE;
	}

	bus.SetCpuCyclesPerInstruction(commandLineArgs.cyclesPerInstruction);

	TTYLogger ttyLogger;

	if (commandLineArgs.sideloadExePath)
	{
		if (!Sideload::SideloadExecutable(commandLineArgs.sideloadExePath, bus, &ttyLogger))
		{
			LOG_ERROR("Failed to sideload executable: %s\n", commandLineArgs.sideloadExePath);
			return EXIT_FAILURE;
		}

		if (commandLineArgs.amidogTestDebug)
			Sideload::EnableAmidogTestDebugOutput(bus);
	}

	if (pCD)
		bus.GetCDROM().InsertDisc(*pCD);

	R3000& r3000 = bus.GetCPU();

	while (true)
	{
		if (s_logCpuState)
			R3000Utils::LogState(r3000);

		if (s_logDisassembly)
		{
			// Disassemble instruction
			u32 pc = r3000.GetPC();
			u32 opcode = bus.ReadWord(pc);
			char disasmBuffer[64]{};
			if (R3000Dasm::Disassemble(opcode, pc, disasmBuffer, sizeof(disasmBuffer)) == 0)
			{
				LOG_ERROR("Failed to disassemble opcode: 0x%08X at address 0x%08X\n", opcode, pc);
				return EXIT_FAILURE;
			}

			LOG_INFO("%08X: %08X %s\n", pc, opcode, disasmBuffer);
		}

		bus.StepInstruction();

		ttyLogger.Update(r3000, commandLineArgs.amidogCpuTestMode ? amidogCpuTestTTTFlushCallback : nullptr);

		if (commandLineArgs.saveVramPC != 0xffffffff)
		{
			if (r3000.GetPC() == commandLineArgs.saveVramPC)
			{
				const GPU& gpu = bus.GetGPU();
				const u8* vram = gpu.GetVRAM();

				// Save all of VRAM as 16 bpp and 24 bpp
				Snapshot::SaveVramRectAsPPM(vram, "vram_16bpp.ppm", 0, 0, kVRAMWidth16bpp, kVRAMHeightLines, DisplayFormat::A1B5G5R5);

				static constexpr unsigned int kVRAMWidth24bpp = kVRAMWidthBytes / 3; // 682.66 rounded down to 682
				Snapshot::SaveVramRectAsPPM(vram, "vram_24bpp.ppm", 0, 0, kVRAMWidth24bpp, kVRAMHeightLines, DisplayFormat::B8G8R8);

				// Save display area in configured format
				Snapshot::SaveVramRectAsPPM(vram, "display.ppm", gpu.GetDisplayStartX(), gpu.GetDisplayStartY(), gpu.GetHorizontalResolution(), gpu.GetVerticalResolution(), gpu.GetDisplayFormat());

				// Save 320x224 display area as 16 bpp to match PeterLemon reference images
				// #TODO: Consider putting this on a command line option
				Snapshot::SaveVramRectAsPPM(vram, "display_320x224.ppm", 0, 0, 320, 224, gpu.GetDisplayFormat());

				commandLineArgs.saveVramPC = 0xffffffff; // invalidate to avoid repeated saves
				if (commandLineArgs.quitOnSave)
					break;
			}
		}
	}

	delete pCD;
	pCD = nullptr;

	return EXIT_SUCCESS;
}
