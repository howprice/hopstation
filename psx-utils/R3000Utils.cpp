#include "R3000Utils.h"

#include "psx/R3000.h"

#include "core/Log.h"

void R3000Utils::LogState(const R3000& c)
{
	LOG_INFO("R0:  %08X  R1:  %08X  R2:  %08X  R3:  %08X  R4:  %08X  R5:  %08X  R6:  %08X  R7:  %08X\n",
		c.GetGPR(0), c.GetGPR(1), c.GetGPR(2), c.GetGPR(3), c.GetGPR(4), c.GetGPR(5), c.GetGPR(6), c.GetGPR(7));
	LOG_INFO("R8:  %08X  R9:  %08X  R10: %08X  R11: %08X  R12: %08X  R13: %08X  R14: %08X  R15: %08X\n",
		c.GetGPR(8), c.GetGPR(9), c.GetGPR(10), c.GetGPR(11), c.GetGPR(12), c.GetGPR(13), c.GetGPR(14), c.GetGPR(15));
	LOG_INFO("R16: %08X  R17: %08X  R18: %08X  R19: %08X  R20: %08X  R21: %08X  R22: %08X  R23: %08X\n",
		c.GetGPR(16), c.GetGPR(17), c.GetGPR(18), c.GetGPR(19), c.GetGPR(20), c.GetGPR(21), c.GetGPR(22), c.GetGPR(23));
	LOG_INFO("R24: %08X  R25: %08X  R26: %08X  R27: %08X  R28: %08X  R29: %08X  R30: %08X  R31: %08X\n",
		c.GetGPR(24), c.GetGPR(25), c.GetGPR(26), c.GetGPR(27), c.GetGPR(28), c.GetGPR(29), c.GetGPR(30), c.GetGPR(31));
	LOG_INFO("PC: %08X  HI: %08X  LO: %08X\n", c.GetPC(), c.GetHI(), c.GetLO());
}
