// PSX Controller

#pragma once

#include "core/Types.h"

class Controller
{
public:

	enum class Type
	{
		Digital,  // Original PlayStation digital controller with no analogue sticks or rumble
		Analogue, // Revised PlayStation controller with analogue sticks and rumble. Dual Analogue Controller or, more commonly, DualShock

		Max = Analogue
	};

	// Ordered by switch bits
	// https://psx-spx.consoledev.net/controllersandmemorycards/#standard-controllers
	enum class DigitalSwitch
	{
		SelectButton,    // bit 0   
		L3,              // bit 1  (analogue controller only)   
		R3,              // bit 2  (analogue controller only)   
		StartButton,     // bit 3   
		JoypadUp,        // bit 4   
		JoypadRight,     // bit 5   
		JoypadDown,      // bit 6   
		JoypadLeft,      // bit 7   
		L2Button,        // bit 8   
		R2Button,        // bit 9   
		L1Button,        // bit 10  
		R1Button,        // bit 11  
		TriangleButton,  // bit 12  
		CircleButton,    // bit 13  
		CrossButton,     // bit 14  
		SquareButton,   // bit 15  
	};

	enum Mode
	{
		Normal,         // Command 42h
		Configuration,  // Command 43h

		Max = Configuration
	};

	/*

	Controller Communication Sequence

	  Send Reply Comment
	  01h  Hi-Z  Controller address
	  42h  idlo  Receive ID bit0..7 (variable) and Send Read Command (ASCII "B")
	  TAP  idhi  Receive ID bit8..15 (usually/always 5Ah)
	  MOT  swlo  Receive Digital Switches bit0..7
	  MOT  swhi  Receive Digital Switches bit8..15
	  --- transfer stops here for digital pad (or analog pad in digital mode) ---
	  00h  adc0  Receive Analog Input 0 (if any) (eg. analog joypad or mouse)
	  00h  adc1  Receive Analog Input 1 (if any) (eg. analog joypad or mouse)
	  00h  adc2  Receive Analog Input 2 (if any) (eg. analog joypad)
	  00h  adc3  Receive Analog Input 3 (if any) (eg. analog joypad)

	  https://psx-spx.consoledev.net/controllersandmemorycards/#controllers-communication-sequence
	*/
	enum class State
	{
		// Don't need a high-impedance state because controller is not accessed by port in that state by definition.
		idlo,
		idhi,
		swlo,
		swhi,
		adc0,
		adc1,
		adc2,
		adc3,
	};

	Controller(unsigned int portIndex) : m_portIndex(portIndex) {}

	void Reset();

	void Select();
	void Deselect();

	// Digital input: buttons + DPAD
	void DigitalSwitchDown(DigitalSwitch digitalSwitch);
	void DigitalSwitchUp(DigitalSwitch digitalSwitch);

	// Analogue input
	void SetRightJoyX(u8 val) { m_rightJoyX = val; }
	void SetRightJoyY(u8 val) { m_rightJoyY = val; }
	void SetLeftJoyX(u8 val) { m_leftJoyX = val; }
	void SetLeftJoyY(u8 val) { m_leftJoyY = val; }

	// Synchronous I/O
	// The data is transferred in units of bytes, via separate input and output lines.
	// So, when sending byte, the hardware does simultaneously receive a response byte.
	// 
	// https://psx-spx.consoledev.net/controllersandmemorycards/#synchronous-io
	//
	// Returns true if the byte is recevied by the selected peripheral i.e. /ACK is asserted
	bool Write8(u8 val, u8& response);

	Type GetType() const { return m_type; }
	void SetType(Type type) { m_type = type; }

private:

	bool updateNormalMode(u8 val, u8& response);
	bool updateConfigurationMode(u8 val, u8& response);

	[[maybe_unused]]
	unsigned int m_portIndex = 0; // 0=left port, 1=right port. Debugging only.
	
	Type m_type = Type::Digital;
	Mode m_mode = Mode::Normal;
	Mode m_nextMode = Mode::Normal;
	u8 m_command = 0;
	State m_state = State::idlo; // #TODO: Get rid of this. It is too specific to button reading. Replace with unsigned int m_step

	u16 m_digitalSwitchBits = 0xffff; // Active low. See https://psx-spx.consoledev.net/controllersandmemorycards/#standard-controllers

	// Analogue sticks
	u8 m_rightJoyX = 0x80; // adc0 RightJoyX (00h=Left, 80h=Center, FFh=Right)
	u8 m_rightJoyY = 0x80; // adc1 RightJoyY (00h=Up,   80h=Center, FFh=Down)
	u8 m_leftJoyX = 0x80;  // adc2 LeftJoyX (00h=Left, 80h=Center, FFh=Right)
	u8 m_leftJoyY = 0x80;  // adc3 LeftJoyY (00h=Up,   80h=Center, FFh=Down)

	bool m_led = false; // Analogue controller LED state, used to switch between digital and analogue
	u8 m_ii = 0; // Config command param

	bool m_motor1 = false; // left/large motor
	bool m_motor2 = false; // right/small motor

	// Original one-motor rumble, set via Normal mode ReadButton command 42h.
	// See https://psx-spx.consoledev.net/controllersandmemorycards/#old-method-one-motor-no-config-commands-scph-1150-scph-1200-scph-110
	// #TODO: Use this to control host controller rumble
	u16 m_motorYYXX = 0;
};
