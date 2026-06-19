#pragma once

#include "core/Types.h"

static constexpr unsigned int kBiosSizeBytes = 512 * 1024;

class BIOS
{
public:

	BIOS();
	~BIOS();

	bool Load(const char* path);

	u8 ReadU8(u32 offset) const;
	u16 ReadU16(u32 offset) const;
	u32 ReadU32(u32 offset) const;

private:
	u8* m_data = nullptr;
};
