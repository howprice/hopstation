// PSX controller and memory card port

#pragma once

#include "core/Types.h"

#include "Controller.h"
#include "MemoryCard.h"

class ControllerPort
{
public:

	enum class DeviceType
	{
		None,
		StandardController,
		MemoryCard,

		Max = MemoryCard
	};

	ControllerPort(unsigned int index);

	void Reset();

	void SetSelected(bool val); // Chip select

	// Synchronous I/O
	// The data is transferred in units of bytes, via separate input and output lines.
	// So, when sending byte, the hardware does simultaneously receive a response byte.
	// 
	// https://psx-spx.consoledev.net/controllersandmemorycards/#synchronous-io
	//
	// Returns true if the byte is recevied by the selected peripheral i.e. /ACK is asserted
	bool Write8(u8 val, u8& response);

	bool IsControllerConnected() const { return m_controllerConnected; }
	void SetControllerConnected(bool connected) { m_controllerConnected = connected; }
	Controller& GetController() { return m_controller; }

	bool IsMemoryCardInserted() const { return m_memoryCardInserted; }
	void SetMemoryCardInserted(bool val);
	MemoryCard& GetMemoryCard() { return m_memoryCard; }

private:
	unsigned int m_index = 0; // 0=left port, 1=right port
	bool m_selected = false; // Chip Select status
	bool m_awaitingAddress = false;
	DeviceType m_deviceType = DeviceType::None;
	bool m_controllerConnected = false;
	Controller m_controller;
	bool m_memoryCardInserted = false;
	MemoryCard m_memoryCard;
};
