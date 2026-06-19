#pragma once

#include <stdint.h>

class HostAudioBuffer
{
public:

	HostAudioBuffer(unsigned int bufferSize);
	~HostAudioBuffer();

	void Clear();
	void WriteSample(int16_t value);
	void Reset();
	const int16_t* GetBuffer() const { return m_pBuffer; }
	unsigned int GetCapacity() const { return m_bufferCapacity; }

	// Returns the number samples stored in the buffer
	unsigned int GetNumSamples() const { return m_pos; }

private:

	int16_t* m_pBuffer = nullptr;
	unsigned int m_bufferCapacity = 0;
	unsigned int m_pos = 0;
};
