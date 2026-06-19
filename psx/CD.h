// Compact Disc image
// See:
// - https://en.wikipedia.org/wiki/CD-ROM#CD-ROM_XA_extension
// - https://psx-spx.consoledev.net/cdromformat/#cdrom-sector-encoding

#pragma once

#include "core/ArrayHelpers.h" // ARRAY_COUNT
#include "core/Helpers.h" // ENUM_COUNT
#include "core/Types.h"

class CD
{
public:

	static const unsigned int kMaxSizeBytes = 736 * 1024 * 1024; // 700 MB sounds about right, but FF7 is 713 MiB. redux cdrom test requires 736 MiB #TODO: Increase if required

	// Sectors are always 2352 bytes, regardless of type.
	static const unsigned int kSectorSizeBytes = 0x930; // 0x930 = 2352

	// CD ROM Sector Encoding

	// All sector formats start a 12 bytes sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
	static const unsigned int kSectorSyncSizeBytes = 12;

	// This is followed by a 4 byte header at offset 0xC: (Minute,Second,Sector,Mode)
	// Mode can be 00 for empty, 01 for Mode1 (original CDROM), 02 for Mode 2  (CD-XA)

	// CD-ROM XA Mode 2 sectors have an 8 byte "subheader" at offset 0x10
	static const unsigned int kMode2SectorSubHeaderOffset = 0x10; // 0x10 = 16
	static const unsigned int kMode2SectorSubHeaderSizeBytes = 8;

	// CD-ROM XA Mode 2 sectors have data at offset 0x18
	static const unsigned int kMode2SectorDataOffset = 24; // 0x18 = 24

	// CD-ROM XA Mode 2, Form 1 sectors have 2048 bytes of data
	static const unsigned int kXA_Mode2Form1SectorDataSizeBytes = 0x800; // 0x800 = 2048

	// CD-ROM XA Mode 2, Form 2 sectors have 2324 bytes of data
	static const unsigned int kXA_Mode2Form2SectorDataSizeBytes = 0x914; // 0x914 = 2324

	// The PSX hardware allows to read 800h (2048) byte or 924h (2340) byte sectors, indexed as [000h..7FFh] or [000h..923h]
	static constexpr unsigned int kSectorSizeExcludingSyncBytes = kSectorSizeBytes - kSectorSyncSizeBytes; // 0x924 = 2340

	// There are two CDROM (disc, head) addressing schemes:
	//   1. LBA stands for Logical Block Addressing (or Logical Block Address). 
	//   2. MSF (Minutes-Seconds-Frames) addressing
	// 
	// LBA is a simplified, linear addressing scheme used to locate data on a disc by assigning a unique integer index to each block, starting from 0. 
	// LBA is linear, unlike the older Cylinder-Head-Sector (CHS) method, LBA treats the entire disc as a linear sequence of blocks (0, 1, 2, ...), allowing
	// the drive controller to find specific data without knowing the exact physical head position, track, or cylinder.
	// Essentially, LBA is just disc sector index.
	// 
	// MSF is used for CDs addressing. It is stored in the sector header (in BCD)


	// Sometimes referred to as "fragments" or "frames", especially in the "MSF" minutes, seconds, frames/fragments format.
	// https://psx-spx.consoledev.net/cdromformat/#cdrom-disk-format
	static const unsigned int kSectorsPerSecond = 75;

	// Original PSX discs have 2 seconds (150 sectors) of empty space before the data starts.
	// At 75 sectors per second, this is equivalent to LBA 150
	// This gap is not present in ripped binary images and needs to be accounted for for accurate addressing.
	// 
	// There are two ways to implement this:
	// 1. Subtract 150 sectors from the address each time the disc is accessed
	// 2. Add 150 sectors gap at the start of the disc image data buffer.
	//
	// Option 2 is preferable because it simplifies address calculations, and I have been informed will be even more
	// convenient when track address data is parsed from CUE files.
	static const unsigned int kPregapSizeSeconds = 2;
	static constexpr unsigned int kPregapSizeBytes = kPregapSizeSeconds * kSectorsPerSecond * kSectorSizeBytes;

	// PSX subset of cue sheet track datatypes from CDRWIN User's Guide https://web.archive.org/web/20070614044112/http://www.goldenhawk.com/download/cdrwin.pdf
	enum class TrackDataType
	{
		UNKNOWN,
		AUDIO,     // Audio/Music (2352)
		MODE2_2336, // CD-ROM XA Mode2 Data
		MODE2_2352, // CD-ROM XA Mode2 Data

		Max = MODE2_2352
	};

	// m, s and f are in decimal, *not* binary coded decimal (they are stored on disc and sent to SetLoc command in BCD)
	static unsigned int MSFtoLBA(unsigned int minutes, unsigned int seconds, unsigned int fragments);

	static void LBAtoMSF(unsigned int lba, unsigned int& minutes, unsigned int& seconds, unsigned int& fragments);

	CD();
	~CD();

	bool LoadFromFile(const char* path);

	const u8* GetData() const { return m_data; }
	unsigned int GetSizeBytes() const { return m_sizeBytes; }
	unsigned int GetSizeSectors() const { return m_sizeSectors; }
	unsigned int GetNumTracks() const { return m_numTracks; }
	TrackDataType GetTrackDataType(unsigned int trackIndex) const;
	unsigned int GetTrackIndex0LBA(unsigned int trackIndex) const;
	unsigned int GetTrackStartLBA(unsigned int trackIndex) const;

	// Returns the LBA of the final sector in the track.
	// e.g. If track range is [0x1234,0x2345] inclusive then returns 0x2345.
	unsigned int GetTrackFinalLBA(unsigned int trackIndex) const;

	const char* GetPath() const { return m_path; }
	const char* GetName() const { return m_name; }

private:

	bool loadBin(const char* path);
	bool loadCue(const char* path);

	void setPathAndName(const char* path);

	u8* m_data{};
	unsigned int m_sizeBytes{}; // including any gaps e.g. 2 seconds pregap at start of .bin/.cue images
	unsigned int m_sizeSectors{};
	unsigned int m_numTracks{};
	char m_path[256]{};
	char m_name[256]{};

	static const unsigned int kMaxTracks = 99;
	TrackDataType m_trackDataType[kMaxTracks]{};
	unsigned int m_trackIndex0LBA[kMaxTracks]{}; // INDEX 0 n.b. These are stored zero-indexed so track[0] is track 01
	unsigned int m_trackIndex1LBA[kMaxTracks]{}; // INDEX 1 " "
};

// PSX subset of cue sheet track datatypes from CDRWIN User's Guide https://web.archive.org/web/20070614044112/http://www.goldenhawk.com/download/cdrwin.pdf
inline const char* kCDTrackDataTypeNames[] =
{
	"UNKNOWN",
	"AUDIO",
	"MODE2/2336",
	"MODE2/2352",
};
static_assert(COUNTOF_ARRAY(kCDTrackDataTypeNames) == ENUM_COUNT(CD::TrackDataType));
