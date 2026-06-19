#include "HostAudioBuffer.h"

#include "core/hp_assert.h"

#include <string.h> // memset

HostAudioBuffer::HostAudioBuffer(unsigned int bufferSize)
{
	m_pBuffer = new int16_t[bufferSize];
	m_bufferCapacity = bufferSize;

	Clear();
}

HostAudioBuffer::~HostAudioBuffer()
{
	delete[] m_pBuffer;
	m_pBuffer = nullptr;
}

void HostAudioBuffer::Clear()
{
	memset(m_pBuffer, 0, m_bufferCapacity);
	m_pos = 0;
}

void HostAudioBuffer::WriteSample(int16_t value)
{
	HP_ASSERT(m_pos < m_bufferCapacity, "Buffer overflow");
	m_pBuffer[m_pos] = value;
	m_pos++;
	if (m_pos >= m_bufferCapacity)
		m_pos = 0;
}

void HostAudioBuffer::Reset()
{
	m_pos = 0;
}
