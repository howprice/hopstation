#include "ControllerPort.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/ArrayHelpers.h"
#include "core/Helpers.h"

static const char* kPortNames[] = { "left", "right" };

static const char* kDeviceTypeNames[] =
{
	"None",
	"Standard Controller",
	"Memory Card",
};
static_assert(COUNTOF_ARRAY(kDeviceTypeNames) == ENUM_COUNT(ControllerPort::DeviceType));

extern bool g_logSIO;

ControllerPort::ControllerPort(unsigned int index)
	: m_index(index)
	, m_controller(index)
	, m_memoryCard(index)
{

}

void ControllerPort::Reset()
{
	m_selected = false;
	m_awaitingAddress = false;
	m_deviceType = DeviceType::None;
	m_controller.Reset();
	m_memoryCard.Reset();
}

/*

Device addressing

Each controller port and its respective memory card slot are wired in parallel.
The /CS signals select both the controller and the memory card when asserted.
This selection is narrowed down through a simple addressing scheme:
The first byte sent by the console after asserting /CSn is the address of the device that shall reply.
All devices must keep the DAT line idle before receiving this byte.
Once the address is sent, the device that was addressed shall pull /ACK low to signal its presence and start exchanging bytes.

The following addresses are known to be used:

    Device                                  Address
    Standard controller                     01h
    Yaroze Access Card                      21h
    PS2 multitap (incompatible with PS1)    21h
    PS2 DVD remote receiver                 61h
    Memory card                             81h (82h..84h are also used by Silent Hill)
*/
void ControllerPort::SetSelected(bool select)
{
	if (select && !m_selected)
	{
		m_awaitingAddress = true;
		m_selected = true;

		if (g_logSIO)
			LOG_INFO("[SIO] Port %u (%s) selected. Awaiting address.\n", m_index, kPortNames[m_index]);

		switch (m_deviceType)
		{
			case DeviceType::None:
				break;
			case DeviceType::StandardController:
				m_controller.Select();
				break;
			case DeviceType::MemoryCard:
				m_memoryCard.Select();
				break;
		}
	}
	else if (!select && m_selected)
	{
		m_selected = false;

		if (g_logSIO)
			LOG_INFO("[SIO] Port %u (%s) deselected\n", m_index, kPortNames[m_index]);

		switch (m_deviceType)
		{
			case DeviceType::None:
				break;
			case DeviceType::StandardController:
				m_controller.Deselect();
				break;
			case DeviceType::MemoryCard:
				m_memoryCard.Deselect();
				break;
		}
	}
}

bool ControllerPort::Write8(u8 val, u8& response)
{
	// Default response in the case of no selected device, or address byte is high impedance value.
	response = 0xff;

	if (!m_selected)
		return false;

	if (m_awaitingAddress)
	{
		if (g_logSIO)
			LOG_INFO("[SIO] Port %u (%s) received %02X while awaiting address.\n", m_index, kPortNames[m_index], val);

		response = 0xff; // explicit response to address byte is high-Z

		if (val == 0x00)
		{
			HP_FATAL_ERROR("TODO: Should this turn off device addressing?");
			return false;
		}
		else if (val == 0x01)
		{
			if (!m_controllerConnected)
				return false;

			// Standard controller addressed
			m_deviceType = DeviceType::StandardController;
			m_awaitingAddress = false;

			if (g_logSIO)
				LOG_INFO("[SIO] Port %u (%s) addressing %s\n", m_index, kPortNames[m_index], kDeviceTypeNames[(int)m_deviceType]);
			return true;
		}
		else if (val == 0x81 || val == 0x82 || val == 0x83 || val == 0x84) // 81h is documented address. Silent Hill uses 82h. Apparantly >= 0x81 will do.
		{
			if (!m_memoryCardInserted)
				return false;

			m_deviceType = DeviceType::MemoryCard;
			m_awaitingAddress = false;
			if (g_logSIO)
				LOG_INFO("[SIO] Port %u (%s) addressing %s\n", m_index, kPortNames[m_index], kDeviceTypeNames[(int)m_deviceType]);
			return true;
		}
		else if (val == 0x55)
		{
			// The redux cdrom test starts with
			// [TTY] Init Pads...
			// [TTY] Entered intro sequence state 10 on frame 60
			// [TTY]         Broadcast to MCPro/PS1D. Present=0,0
			// 
			// And tries to address a device with ID 0x55. Is this some sort of debugging thing?
			return false;
		}
		else
		{
			HP_FATAL_ERROR("Unsupported device");
			return false;
		}
	}

	switch (m_deviceType)
	{
		case DeviceType::None:
			return false;
		case DeviceType::StandardController:
		{
			if (m_controllerConnected)
				return m_controller.Write8(val, response);
			else
			{
				response = 0xff; // explicit response to data byte is high-Z when controller is disconnected
				return false;
			}
		}
		case DeviceType::MemoryCard:
		{
			if (m_memoryCardInserted)
				return m_memoryCard.Write8(val, response);
			else
			{
				response = 0xff; // explicit response to data byte is high-Z when memory card is disconnected
				return false;
			}
		}
	}
	return false;
}

void ControllerPort::SetMemoryCardInserted(bool val)
{
	if (val == m_memoryCardInserted)
		return;

	m_memoryCardInserted = val;

	if (val)
		m_memoryCard.OnInserted();
}
