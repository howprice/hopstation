#pragma once

#include <stdint.h>

class RingBuffer
{
public:
	RingBuffer(uint8_t* storage, unsigned int capacityBytes);

	// Does not modify storage buffer. Client can do so if required.
	void Reset();

	// Adds data to the ring buffer
	void Write(const uint8_t* src, unsigned int numBytes);

	// Removes data from the buffer and copies into provided buffer
	void Read(uint8_t* dst, unsigned int numBytes);

	unsigned int GetUsedSpaceBytes() const;

	unsigned int GetFreeSpaceBytes() const;

	bool IsFull() const { return m_full; }
	bool IsEmpty() const { return m_empty; }

private:

	const unsigned int m_capacityBytes;
	uint8_t* m_data;
	unsigned int m_writeIndex = 0;
	unsigned int m_readIndex = 0;
	bool m_empty = true;
	bool m_full = false;
};
