#include "BIOS.h"

#include "core/hp_assert.h"
#include "core/Log.h"

#include <stdio.h>

BIOS::BIOS()
{
	m_data = new u8[kBiosSizeBytes];
}

BIOS::~BIOS()
{
	delete[] m_data;
}

bool BIOS::Load(const char* path)
{
	FILE* fp = fopen(path, "rb");
	if (!fp)
	{
		LOG_ERROR("Failed to open file: %s\n", path);
		return false;
	}

	if (fread(m_data, 1, kBiosSizeBytes, fp) != kBiosSizeBytes)
	{
		LOG_ERROR("Failed to read file: %s\n", path);
		fclose(fp);
		return false;
	}
	fclose(fp);
	return true;
}

u8 BIOS::ReadU8(u32 offset) const
{
	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT(offset < kBiosSizeBytes);

	u8 val = m_data[offset];
	return val;
}

u16 BIOS::ReadU16(u32 offset) const
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 1) == 0, "Unaligned read");

	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT(offset < kBiosSizeBytes);

	// R3000 is little-endian. Assume host is too.
	u16 val = *(u16*)(m_data + offset);
	return val;
}

u32 BIOS::ReadU32(u32 offset) const
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 3) == 0, "Unaligned read");

	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT(offset < kBiosSizeBytes);

	// R3000 is little-endian. Assume host is too.
	u32 val = *(u32*)(m_data + offset);
	return val;
}
