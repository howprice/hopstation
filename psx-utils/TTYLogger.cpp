#include "TTYLogger.h"

#include "psx/R3000.h"

#include "core/ArrayHelpers.h"
#include "core/Log.h"

void TTYLogger::Reset()
{
	m_buffer[0] = '\0';
	m_count = 0;
	m_prevEOL = false;
}

void TTYLogger::Update(const R3000& r3000, FlushCallback pCallback)
{
	u32 pc = r3000.GetPC() & 0x1fffffff;
	if ((pc == 0xa0 && r3000.GetGPR(9) == 0x3C) || (pc == 0xb0 && r3000.GetGPR(9) == 0x3D))
	{
		char c = (char)r3000.GetGPR(4);
		m_buffer[m_count++] = c;

		bool flush = false;
		if ((c == '\n' || c == '\r') && !m_prevEOL) // flush on newline or carriage return (Amidog uses \n, PeterLemon uses \r)
		{
			m_buffer[m_count] = '\0';
			flush = true;
			m_prevEOL = true;
		}
		else
			m_prevEOL = false;

		if (!flush && m_count + 2 == COUNTOF_ARRAY(m_buffer)) // flush if buffer full
		{
			m_buffer[m_count++] = '\0';
			m_buffer[m_count] = '\n';
			flush = true;
		}

		if (flush)
		{
			if (pCallback)
				pCallback(m_buffer);
			else
				LOG_INFO("[TTY] %s", m_buffer);
			m_count = 0;
		}
	}
}
