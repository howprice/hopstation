#include "RAM.h"

#include "core/MathsHelpers.h" // IsPowerOfTwo
#include "core/hp_assert.h"

#include <string.h> // memset

RAM::RAM(unsigned int sizeBytes)
	: m_sizeBytes(sizeBytes)
{
	HP_ASSERT(IsPowerOfTwo(sizeBytes));
	m_mask = sizeBytes - 1;

	m_data = new u8[sizeBytes];
	Reset();
}

RAM::~RAM()
{
	delete[] m_data;
}

void RAM::Reset()
{
	memset(m_data, 0, m_sizeBytes);
}

u8 RAM::ReadU8(u32 offset) const
{
	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);

	return m_data[offset];
}

u16 RAM::ReadU16(u32 offset) const
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 1) == 0, "Unaligned read");

	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);

	// R3000 is little-endian. Assume host is too.
	u16 val = *(u16*)(m_data + offset);
	return val;
}

u32 RAM::ReadU32(u32 offset) const
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 3) == 0, "Unaligned read");

#if 0
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);
#else
	// Apply mask to avoid reading outside of range.
	// This is required for Sexy Parodius.
	// #TODO: Is this the correct behaviour?
	offset &= m_mask;
#endif

	// R3000 is little-endian. Assume host is too.
	u32 val = *(u32*)(m_data + offset);
	return val;
}

void RAM::WriteU8(u32 offset, u8 val)
{
	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);

	m_data[offset] = val;
}

void RAM::WriteU16(u32 offset, u16 val)
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 1) == 0, "Unaligned 16-bit write");

	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);

	u16* pDst = (u16*)(m_data + offset);

	// R3000 is little-endian. Assume host is too.
	*pDst = val;
}

void RAM::WriteU32(u32 offset, u32 val)
{
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_ASSERT((offset & 3) == 0, "Unaligned 32-bit write");

	// #TODO: Apply mask to avoid reading outside of range?
	// #TODO: Remove this assert - this is likely to be performance critical code.
	HP_DEBUG_ASSERT(offset < m_sizeBytes);

	u32* pDst = (u32*)(m_data + offset);

	// R3000 is little-endian. Assume host is too.
	*pDst = val;
}

void RAM::Write(u32 offset, const u8* data, u32 sizeBytes)
{
	HP_DEBUG_ASSERT(offset + sizeBytes <= m_sizeBytes);
	memcpy(m_data + offset, data, sizeBytes);
}
