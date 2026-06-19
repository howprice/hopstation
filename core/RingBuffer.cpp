#include "RingBuffer.h"

#include "hp_assert.h"
#include "core/Types.h"

#include <string.h> // memcpy

RingBuffer::RingBuffer(u8* storage, unsigned int capacityBytes)
: m_capacityBytes(capacityBytes)
, m_data(storage)
{

}

void RingBuffer::Reset()
{
	m_writeIndex = 0;
	m_readIndex = 0;
	m_empty = true;
	m_full = false;
}

void RingBuffer::Write(const u8* src, unsigned int numBytes)
{
	if (numBytes == 0)
		return;

	// #TODO: What should the behaviour be here?
	HP_DEBUG_ASSERT(numBytes <= GetFreeSpaceBytes(), "Please check there is enough free space before writing");

	if (m_readIndex <= m_writeIndex)
	{
		// One or two free ranges: between start and read index, and/or between write index and end.
		unsigned int endSpaceBytes = m_capacityBytes - m_writeIndex;
		if (numBytes <= endSpaceBytes)
		{
			memcpy(m_data + m_writeIndex, src, numBytes);
			m_writeIndex = (m_writeIndex + numBytes);
			if (m_writeIndex == m_capacityBytes)
			{
				m_writeIndex = 0;
				if (m_writeIndex == m_readIndex)
					m_full = true;
			}
		}
		else
		{
			memcpy(m_data + m_writeIndex, src, endSpaceBytes);
			unsigned int secondSliceSizeBytes = numBytes - endSpaceBytes;
			memcpy(m_data, src + endSpaceBytes, secondSliceSizeBytes);
			m_writeIndex = secondSliceSizeBytes;
			if (m_writeIndex == m_readIndex)
				m_full = true;
		}
	}
	else // m_writeIndex < m_readIndex
	{
		// Single free range between write index and read index
		memcpy(m_data + m_writeIndex, src, numBytes);
		m_writeIndex += numBytes;
		if (m_writeIndex == m_readIndex)
			m_full = true;
	}

	if (m_empty)
		m_empty = false;
}

void RingBuffer::Read(uint8_t* dst, unsigned int numBytes)
{
	if (numBytes == 0)
		return;

	HP_DEBUG_ASSERT(numBytes <= GetUsedSpaceBytes(), "Please check there is enough data available before reading");

	if (m_readIndex <= m_writeIndex)
	{
		// Single used range
		memcpy(dst, m_data + m_readIndex, numBytes);
		m_readIndex += numBytes;
		if (m_readIndex == m_writeIndex)
			m_empty = true;
	}
	else // m_writeIndex < m_readIndex
	{
		// One or two used ranges: between read index and end and/or between start and write index
		unsigned int endSizeBytes = m_capacityBytes - m_readIndex;
		if (numBytes <= endSizeBytes)
		{
			// Read end range only
			memcpy(dst, m_data + m_readIndex, numBytes);
			m_readIndex += numBytes;
			if (m_readIndex == m_capacityBytes)
			{
				m_readIndex = 0;
				if (m_readIndex == m_writeIndex)
					m_empty = true;
			}
		}
		else
		{
			// Read end range and start range
			memcpy(dst, m_data + m_readIndex, endSizeBytes);
			unsigned int secondSliceSizeBytes = numBytes - endSizeBytes;
			memcpy(dst + endSizeBytes, m_data, secondSliceSizeBytes);
			m_readIndex = secondSliceSizeBytes;
			if (m_readIndex == m_writeIndex)
				m_empty = true;
		}
	}

	if (m_full)
		m_full = false;
}

unsigned int RingBuffer::GetUsedSpaceBytes() const
{
	if (m_empty)
		return 0;
	if (m_full)
		return m_capacityBytes;
	if (m_writeIndex > m_readIndex)
		return m_writeIndex - m_readIndex;
	else // if (m_readIndex > m_writeIndex)
		return m_capacityBytes - (m_readIndex - m_writeIndex);
}

unsigned int RingBuffer::GetFreeSpaceBytes() const
{
	if (m_empty)
		return m_capacityBytes;
	if (m_full)
		return 0;
	if (m_writeIndex > m_readIndex)
		return m_capacityBytes - (m_writeIndex - m_readIndex);
	else // if (m_readIndex > m_writeIndex)
		return m_readIndex - m_writeIndex;
}
