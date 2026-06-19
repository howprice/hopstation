#pragma once

class R3000;

class TTYLogger
{
public:

	typedef void (*FlushCallback)(const char* text);

	void Reset();

	// Intercepts putchar function calls which the BIOS uses for TTY
	// See https://jsgroth.dev/blog/posts/ps1-sideloading/#tty
	void Update(const R3000& r3000, FlushCallback pCallback);

private:
	char m_buffer[1024]{};
	unsigned int m_count = 0;
	bool m_prevEOL = false;
};
