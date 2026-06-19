#include "Controller.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/Helpers.h" // HP_UNUSED
#include "core/ArrayHelpers.h"

//
// https://psx-spx.consoledev.net/controllersandmemorycards/#controller-id-halfword-number-0
//
static const u16 kControllerIDs[] =
{
	0x5A41, // Digital Pad, or analogue pad/stick in digital mode with LED off
	0x5A73, // DualShock analogue mode i.e. LED lit red
};
static_assert(COUNTOF_ARRAY(kControllerIDs) == ENUM_COUNT(Controller::Type));


static const char* kModeNames[] =
{
	"Normal",
	"Configuration",
};
static_assert(COUNTOF_ARRAY(kModeNames) == ENUM_COUNT(Controller::Mode));

static bool s_logController = false;

void Controller::Reset()
{
	m_mode = Mode::Normal;
	m_nextMode = Mode::Normal;
	m_command = 0x00;
	m_state = State::idlo;
	m_digitalSwitchBits = 0xffff; // active low
	m_rightJoyX = 0x80;
	m_rightJoyY = 0x80;
	m_leftJoyX = 0x80;
	m_leftJoyY = 0x80;
	m_led = false;
	m_ii = 0;
	m_motor1 = false;
	m_motor2 = false;
	m_motorYYXX = 0;
}

void Controller::Select()
{
	if (s_logController)
		LOG_INFO("[Controller] Controller %u selected. Resetting state machine.\n", m_portIndex);

	// Reset state machine
	m_mode = Mode::Normal;
	m_nextMode = Mode::Normal;
	m_command = 0;
	m_state = State::idlo;
}

void Controller::Deselect()
{
	if (s_logController)
		LOG_INFO("[Controller] Controller %u deselected\n", m_portIndex);
}

void Controller::DigitalSwitchDown(DigitalSwitch digitalSwitch)
{
	// Reset bit
	m_digitalSwitchBits &= ~(1u << (unsigned int)digitalSwitch);
}

void Controller::DigitalSwitchUp(DigitalSwitch digitalSwitch)
{
	// Set bit
	m_digitalSwitchBits |= (1u << (unsigned int)digitalSwitch);
}

bool Controller::Write8(u8 val, u8& response)
{
	bool ack = false;
	switch (m_mode)
	{
		case Mode::Normal:
			ack = updateNormalMode(val, response);
			break;

		case Mode::Configuration:
			ack = updateConfigurationMode(val, response);
			break;
	}

	if (s_logController)
		LOG_INFO("[Controller] Controller %u %s received %02X response %02X\n", m_portIndex, kModeNames[(int)m_mode], val, response);

	return ack;
}

// Transfer length in Normal Mode is 5 bytes (Digital mode), or 9 bytes (Analog mode)
bool Controller::updateNormalMode(u8 val, u8& response)
{
	response = 0;

	switch (m_state)
	{
		case State::idlo: // return controller ID LSB
		{
			m_command = val;
			if (m_command == 0x42) // In normal mode, command 42h 'B' is Read Buttons (and analog inputs when in analog mode)  https://psx-spx.consoledev.net/controllersandmemorycards/#normal-mode-command-42h-b-read-buttons-and-analog-inputs-when-enabled
			{
				// Send  01h 42h 00h xx  yy  (00h 00h 00h 00h) (...)
				// Reply HiZ id  5Ah buttons ( analog-inputs ) (dualshock2 buttons...)

				// xx and yy are used to control the rumble motor (actuator). To switch the motor on:
				//   xx --> must be 40h..7Fh            (ie. bit7=0, bit6=1)
				//   yy --> must be 01h,03h,...,FDh,FFh (ie. bit0=1)
				// The motor control is digital on/off (no analog slow/fast), recommended values would be yyxx=0140h=on, and yyxx=0000h=off.
				// https://psx-spx.consoledev.net/controllersandmemorycards/#old-method-one-motor-no-config-commands-scph-1150-scph-1200-scph-110

				response = kControllerIDs[(int)m_type] & 0xff; // id = 41h for digital; 53h for analogue
				m_state = State::idhi;
			}
			else if (m_command == 0x43) // In normal mode, command 43h 'C' Enter/Exit Configuration Mode.  https://psx-spx.consoledev.net/controllersandmemorycards/#normal-mode-command-43h-c-enterexit-configuration-mode
			{
				// Send  01h 43h 00h xx  00h (zero padded...)   (...)
				// Reply HiZ id  5Ah buttons (analog inputs...) (dualshock2 buttons...)

				// "Some controllers can be switched from Normal Mode to Config Mode.
				// The Config Mode was invented for activating the 2nd rumble motor in SCPH-1200 analog joypads.
				// Additionally, the Config commands can switch between analog/digital inputs (without needing to manually press the Analog button),
				// activate more analog inputs (on Dualshock2), and read some type/status bytes."
				// - https://psx-spx.consoledev.net/controllersandmemorycards/#controllers-configuration-commands
				
				switch (m_type)
				{
					case Type::Digital:
					{
#if 0
						ret = kControllerIDs[(int)m_type] & 0xff; // id = 41h for digital; 53h for analogue
						m_state = State::idhi;
#else
						// #TODO: What does a digital controller do if it receives the config command, which is not supported?
						// TEST: Return Hi-Z and see how the software responde
						response = 0xff;
						return false;
#endif
						break;
					}
					case Type::Analogue:
					{
						response = kControllerIDs[(int)m_type] & 0xff; // id = 41h for digital; 53h for analogue
						m_state = State::idhi;
						break;
					}
				}
			}
			else
			{
				HP_FATAL_ERROR("Unexpected normal mode command: %02X", val); // https://psx-spx.consoledev.net/controllersandmemorycards/#normal-mode
			}
			break;
		}
		case State::idhi:  // return controller ID MSB
		{
			response = kControllerIDs[(int)m_type] >> 8; // 5Ah for both digital and analogue controllers
			m_state = State::swlo;
			break;
		}
		case State::swlo: // digital switches LSB
		{
			if (m_command == 0x42)
			{
				u8 xx = val; // rumble motor control
				m_motorYYXX = (m_motorYYXX & 0xff00) | xx;
			}
			else if (m_command == 0x43)
			{
				switch (m_type)
				{
					case Type::Digital:
						// Digital pads cannot switch to configuration mode.
						// Do nothing.
						break;
					case Type::Analogue:
					{
						if (val == 0x00)
						{
							// Stay in normal mode
							m_nextMode = Mode::Normal;
						}
						else if (val == 0x01)
						{
							// Switch to configuration mode 
							m_nextMode = Mode::Configuration;
						}
						else
						{
							HP_FATAL_ERROR("Unexpected normal mode command 43h Enter/Exit Configuration Mode param: %02Xh");
						}
						break;
					}
				}
			}

			response = m_digitalSwitchBits & 0xff;
			m_state = State::swhi;
			break;
		}
		case State::swhi: // digital switches MSB
		{
			if (m_command == 0x42)
			{
				u8 yy = val; // rumble motor control
				m_motorYYXX = ((u16)yy << 8) | (m_motorYYXX & 0x00ff);
			}

			response = m_digitalSwitchBits >> 8;

			// In normal mode, when reading buttons next state depends on controller type
			switch (m_type)
			{
				case Type::Digital:
					m_state = State::idlo;
					m_mode = m_nextMode;
					break;
				case Type::Analogue:
					m_state = State::adc0;
					break;
			}
			break;
		}
		case State::adc0:
		{
			response = m_rightJoyX;
			m_state = State::adc1;
			break;
		}
		case State::adc1:
		{
			response = m_rightJoyY;
			m_state = State::adc2;
			break;
		}
		case State::adc2:
		{
			response = m_leftJoyX;
			m_state = State::adc3;
			break;
		}
		case State::adc3:
		{
			response = m_leftJoyY;
			m_state = State::idlo;
			m_mode = m_nextMode;
			break;
		}
	}

	return true;
}

bool Controller::updateConfigurationMode(u8 val, u8& response)
{
	response = 0;

	switch (m_state)
	{
		case State::idlo:
		{
			m_command = val;
			if (m_command == 0x42) // Configuration mode command 42h 'B' is Read Buttons AND analog inputs too. See https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-42h-b-read-buttons-and-analog-inputs
			{
				//   Send  01h 42h 00h M2  M1  00h 00h 00h 00h
				//   Reply HiZ F3h 5Ah buttons  analog-inputs
				
				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x43) // Configuration mode command 43h 'C' Enter/Exit Configuration Mode. See https://psx-spx.consoledev.net/controllersandmemorycards/#normal-mode-command-43h-c-enterexit-configuration-mode
			{
				// Send  01h 43h 00h xx  00h 00h 00h 00h 00h
				// Reply HiZ F3h 5Ah 00h 00h 00h 00h 00h 00h

				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x44) // Configuration mode command 44h "D" - Set LED State (analog mode on/off) https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-44h-d-set-led-state-analog-mode-onoff
			{
				//  Send  01h 44h 00h Led Key 00h 00h 00h 00h
				//  Reply HiZ F3h 5Ah 00h 00h Err 00h 00h 00h
				// 
				// The Led byte can be:
				//   When Led=00h      --> Digital mode, with LED=Off
				//   When Led=01h      --> Analog mode, with LED=On/red
				//   When Led=02h..FFh --> Ignored (and, in case of dualshock2: set Err=FFh)
				// 
				// The Key byte can be:
				//   When Key=00h..02h --> Unlock (allow user to push Analog button)
				//   When Key=03h      --> Lock (stay in current mode, ignore Analog button)
				//   When Key=04h..FFh --> Acts same as (Key AND 03h)
				// 
				// The Err byte is usually 00h (except, Dualshock2 sets Err=FFh upon Led=02h..FFh; older PSX/PSone controllers don't do that).

				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x45) // Configuration mode command 45h 'E' Get LED State (and Type/constants). See https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-45h-e-get-led-state-and-typeconstants
			{
				// Send  01h 45h 00h 00h 00h 00h 00h 00h 00h
				// Reply HiZ F3h 5Ah Typ 02h Led 02h 01h 00h

				// Where
				//  Led: Current LED State (00h=Off, 01h=On/red)
				//  Typ: Controller Type (01h=PSX/Analog Pad, 03h=PS2/Dualshock2)

				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x46) // Configuration mode command 46h "F" - Get Variable Response A https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-46h-f-get-variable-response-a
			{
				// Send  01h 46h 00h ii  00h 00h 00h 00h 00h
				// Reply Hiz F3h 5Ah 00h 00h cc  dd  ee  ff
				//
				// When ii=00h --> returns cc,dd,ee,ff = 01h,02h,00h,0ah
				// When ii=01h --> returns cc,dd,ee,ff = 01h,01h,01h,14h
				// Otherwise --> returns cc,dd,ee,ff = all zeroes
				// 
				// #TODO: This may have something to do with the actuator(s) i.e. rumble

				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x47) // Configuration mode command 47h "G" - Get whatever values https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-47h-g-get-whatever-values
			{
				// Purpose unknown
				// Send  01h 47h 00h 00h 00h 00h 00h 00h 00h
				// Reply HiZ F3h 5Ah 00h 00h 02h 00h 01h 00h
				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x4C) // Configuration mode command 4Ch 'L' Get Variable Response B https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-4ch-l-get-variable-response-b
			{
				// Send  01h 4Ch 00h ii  00h 00h 00h 00h 00h
				// Reply Hiz F3h 5Ah 00h 00h 00h dd  00h 00h
				//
				// When ii=00h --> returns dd=04h.
				// When ii=01h --> returns dd=07h.
				// Otherwise --> returns dd=00h.
				response = 0xf3;
				m_state = State::idhi;
			}
			else if (m_command == 0x4D) // Config mode command 4Dh "M" - Get/Set RumbleProtocol https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-4dh-m-getset-rumbleprotocol_1
			{
				// Send  01h 4Dh 00h aa  bb  cc  dd  ee  ff     ;<-- set NEW aa..ff values
				// Reply Hiz F3h 5Ah aa  bb  cc  dd  ee  ff     ;<-- returns OLD aa..ff values

				// #TODO: Implement rumble.
				// #TEMP: Just return initial value FFh for all values for now.

				response = 0xf3;
				m_state = State::idhi;
			}
			else
			{
				HP_FATAL_ERROR("Unhandled configuration mode command: %02X", val); // See https://psx-spx.consoledev.net/controllersandmemorycards/#configuration-mode
			}
			break;
		}
		case State::idhi:  // return controller ID MSB
		{
			response = kControllerIDs[(int)m_type] >> 8; // 5Ah for both digital and analogue controllers
			m_state = State::swlo;
			break;
		}
		case State::swlo:
		{
			if (m_command == 0x42)
			{
				response = m_digitalSwitchBits & 0xff;
			}
			else if (m_command == 0x43) // https://psx-spx.consoledev.net/controllersandmemorycards/#config-mode-command-43h-c-enterexit-configuration-mode
			{
				if (val == 0x00)
				{
					// Switch to normal mode
					m_nextMode = Mode::Normal;
				}
				else if (val == 0x01)
				{
					// Stay in configuration mode
					m_nextMode = Mode::Configuration;
				}
				else
				{
					HP_FATAL_ERROR("Unexpected normal mode command 43h Enter/Exit Configuration Mode param: %02Xh");
				}

				response = 0;
			}
			else if (m_command == 0x44)
			{
				if (val == 0)
					m_led = 0; // digital mode
				else if (val == 1)
					m_led = 1; // analogue mode
				else
				{
					// #TODO: For DualShock2, set Err=FFh
				}
				response = 0;
			}
			else if (m_command == 0x45)
			{
				response = 0x01; // Controller Type (01h=PSX/Analog Pad, 03h=PS2/Dualshock2)
			}
			else if (m_command == 0x46)
			{
				m_ii = val;
				response = 0;
			}
			else if (m_command == 0x47)
				response = 0;
			else if (m_command == 0x4C)
			{
				m_ii = val;
				response = 0;
			}
			else if (m_command == 0x4D)
				response = 0xff;

			m_state = State::swhi;
			break;
		}
		case State::swhi: // digital switches MSB
		{
			if (m_command == 0x42)
				response = m_digitalSwitchBits >> 8;
			else if (m_command == 0x43)
				response = 0;
			else if (m_command == 0x44)
			{
				u8 key = val & 3;
				if (key <= 2)
				{
					// #TODO: Unlock LED (allow user to push Analog button)
				}
				else if (key == 3)
				{
					// #TODO: Lock LED (ignore analogue button presses)
				}
				response = 0;
			}
			else if (m_command == 0x45)
				response = 0x02;
			else if (m_command == 0x46)
				response = 0;
			else if (m_command == 0x47)
				response = 0;
			else if (m_command == 0x4C)
				response = 0;
			else if (m_command == 0x4D)
				response = 0xff;

			// In configuration mode, analogue input is *always* read, so progress to adc0
			m_state = State::adc0;
			break;
		}
		case State::adc0:
		{
			if (m_command == 0x42)
				response = m_rightJoyX;
			else if (m_command == 0x43)
				response = 0;
			else if (m_command == 0x44)
				response = 0; // #TODO: For DualShock2, return error code if LED param was invalid
			else if (m_command == 0x45)
				response = m_led ? 1 : 0;
			else if (m_command == 0x46)
			{
				if (m_ii == 0 || m_ii == 1)
					response = 1;
				else
					response = 0;
			}
			else if (m_command == 0x47)
				response = 2;
			else if (m_command == 0x4C)
				response = 0;
			else if (m_command == 0x4D)
				response = 0xff;

			m_state = State::adc1;
			break;
		}
		case State::adc1:
		{
			if (m_command == 0x42)
				response = m_rightJoyY;
			else if (m_command == 0x43)
				response = 0;
			else if (m_command == 0x44)
				response = 0;
			else if (m_command == 0x45)
				response = 2;
			else if (m_command == 0x46)
			{
				if (m_ii == 0)
					response = 2;
				else if (m_ii == 1)
					response = 1;
				else
					response = 0;
			}
			else if (m_command == 0x47)
				response = 0;
			else if (m_command == 0x4C)
			{
				if (m_ii == 0)
					response = 0x04;
				else if (m_ii == 1)
					response = 0x07;
				else
					response = 0;
			}
			else if (m_command == 0x4D)
				response = 0xff;

			m_state = State::adc2;
			break;
		}
		case State::adc2:
		{
			if (m_command == 0x42)
				response = m_leftJoyX;
			else if (m_command == 0x43)
				response = 0;
			else if (m_command == 0x44)
				response = 0;
			else if (m_command == 0x45)
				response = 1;
			else if (m_command == 0x46)
			{
				if (m_ii == 0)
					response = 0;
				else if (m_ii == 1)
					response = 1;
				else
					response = 0;
			}
			else if (m_command == 0x47)
				response = 1;
			else if (m_command == 0x4C)
				response = 0;
			else if (m_command == 0x4D)
				response = 0xff;

			m_state = State::adc3;
			break;
		}
		case State::adc3:
		{
			if (m_command == 0x42)
				response = m_leftJoyY;
			else if (m_command == 0x43)
				response = 0;
			else if (m_command == 0x44)
				response = 0;
			else if (m_command == 0x45)
				response = 0;
			else if (m_command == 0x46)
			{
				if (m_ii == 0)
					response = 0x0a;
				else if (m_ii == 1)
					response = 0x14;
				else
					response = 0;
			}
			else if (m_command == 0x47)
				response = 0;
			else if (m_command == 0x4C)
				response = 0;
			else if (m_command == 0x4D)
				response = 0xff;

			m_state = State::idlo;
			m_mode = m_nextMode;
			break;
		}
	}

	return true;
}
