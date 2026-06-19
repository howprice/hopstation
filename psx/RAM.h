#pragma once

#include "core/Types.h"

class RAM
{
public:

	RAM(unsigned int sizeBytes);
	~RAM();

	void Reset();

	u8 ReadU8(u32 offset) const;
	u16 ReadU16(u32 offset) const;
	u32 ReadU32(u32 offset) const;

	void WriteU8(u32 offset, u8 val);
	void WriteU16(u32 offset, u16 val);
	void WriteU32(u32 offset, u32 val);

	void Write(u32 offset, const u8* data, u32 sizeBytes);

	unsigned int GetSizeBytes() const { return m_sizeBytes; }

	u8* GetData() { return m_data; }

private:

	u8* m_data = nullptr;
	unsigned int m_sizeBytes = 0;
	u32 m_mask = 0; // Mask to apply to addresses to wrap around when accessing RAM, eg. for 2MB RAM this would be 0x1fffff
};
