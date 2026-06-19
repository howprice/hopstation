#include "CD.h"

#include "core/Log.h"
#include "core/hp_assert.h"
#include "core/FileHelpers.h" // FilenameWithoutExtensionFromPath
#include "core/StringHelpers.h"
#include "core/ArrayHelpers.h"

#include <string.h> // memset, strrchr
#include <stdio.h>

unsigned int CD::MSFtoLBA(unsigned int minutes, unsigned int seconds, unsigned int fragments)
{
	constexpr unsigned int kSecondsPerMinute = 60;
	unsigned int lba = ((minutes * kSecondsPerMinute) + seconds) * CD::kSectorsPerSecond + fragments;
	return lba;
}

void CD::LBAtoMSF(unsigned int lba, unsigned int& minutes, unsigned int& seconds, unsigned int& fragments)
{
	constexpr unsigned int kSecondsPerMinute = 60;
	minutes = lba / (kSecondsPerMinute * kSectorsPerSecond);
	seconds = (lba / kSectorsPerSecond) % kSecondsPerMinute;
	fragments = lba % kSectorsPerSecond;
}

CD::CD()
{
	m_data = new u8[kMaxSizeBytes];
	memset(m_data, 0, kMaxSizeBytes);
}

CD::~CD()
{
	delete[] m_data;
}

bool CD::LoadFromFile(const char* path)
{
	const char* extension = strrchr(path, '.');
	if (strcmp(extension, ".bin") == 0)
	{
		if (!loadBin(path))
			return false;
		return true;
	}
	else if (strcmp(extension, ".cue") == 0)
		return loadCue(path);
	else
	{
		HP_ASSERT(strstr(path, ".bin"), "Unsupported file type. Only .bin and .cue files are currently supported.");
		return false;
	}
}

bool CD::loadBin(const char* path)
{
	FILE* fp = fopen(path, "rb");
	if (!fp)
	{
		LOG_ERROR("Failed to open file: %s\n", path);
		return false;
	}

	fseek(fp, 0, SEEK_END);
	unsigned int fileSizeBytes = (unsigned int)ftell(fp);
	fseek(fp, 0, SEEK_SET);
	LOG_INFO("Opened bin file \"%s\" size %u (0x%X) bytes\n", path, fileSizeBytes, fileSizeBytes);

	if (kPregapSizeBytes + fileSizeBytes > kMaxSizeBytes)
	{
		LOG_ERROR("Disc image exceeds maximum size\n");
		fclose(fp);
		return false;
	}

	// Zero the pregap.
	// Shouldn't need to do this each time, but just to be sure.
	memset(m_data, 0, kPregapSizeBytes);

	// Copy the file image data after the pregap
	if (fread(m_data + kPregapSizeBytes, 1, fileSizeBytes, fp) != fileSizeBytes)
	{
		LOG_ERROR("Failed to read file: %s\n", path);
		fclose(fp);
		return false;
	}
	m_sizeBytes = kPregapSizeBytes + fileSizeBytes;

	HP_ASSERT((m_sizeBytes % kSectorSizeBytes) == 0);
	m_sizeSectors = m_sizeBytes / kSectorSizeBytes;

	fclose(fp);
	fp = nullptr;

	m_numTracks = 1; // assume one track

	// Clear others to avoid confusing data in debugger
	m_trackIndex1LBA[0] = 0;
	HP_ASSERT(m_sizeSectors > 0);

	for (unsigned int i = 1; i < kMaxTracks; i++)
	{
		m_trackDataType[i] = TrackDataType::UNKNOWN;
		m_trackIndex0LBA[i] = 0;
		m_trackIndex1LBA[i] = 0;
	}

	setPathAndName(path);

	return true;
}

// Load a Cue sheet
// https://en.wikipedia.org/wiki/Cue_sheet_(computing)
// https://psx-spx.consoledev.net/cdromfileformats/#cuebin-cdrwin
// Spec can be found in CDRWIN User's Guide https://web.archive.org/web/20070614044112/http://www.goldenhawk.com/download/cdrwin.pdf
// 
bool CD::loadCue(const char* cuePath)
{
	FILE* pCueFile = fopen(cuePath, "r");
	if (!pCueFile)
	{
		LOG_ERROR("Failed to open file: %s\n", cuePath);
		return false;
	}

	LOG_INFO("Opened cue file: %s\n", cuePath);

	m_sizeBytes = 0;
	m_sizeSectors = 0;
	m_numTracks = 0;
	for (unsigned int i = 0; i < kMaxTracks; i++)
	{
		m_trackDataType[i] = TrackDataType::UNKNOWN;
		m_trackIndex0LBA[i] = 0;
		m_trackIndex1LBA[i] = 0;
	}

	FILE* pBinFile = nullptr;

	char line[256];
	bool error = false;

	// CUE file does not contain track lengths, so need to keep track of how much to read before inserting a gap is encountered.
	unsigned int pregapSectorsToInsert = 0;

	// Current bin file
	char binPath[512];
	binPath[0] = '\0';
	unsigned int fileSizeBytes = 0;

	while (fgets(line, sizeof(line), pCueFile))
	{
		LOG_INFO("%s", line); // line contains newline

		// skip leading whitespace
		char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		if (strncmp(p, "FILE", 4) == 0)
		{
			// FILE filename filetype
			// Filename may or may not be quoted
			// e.g. FILE "Mortal Kombat II (Japan) (Track 1).bin" BINARY

			if (pBinFile)
			{
				// Read remainder of previous bin file
				unsigned int fileOffsetBytes = (unsigned int)ftell(pBinFile);
				HP_ASSERT(fileOffsetBytes < fileSizeBytes);
				unsigned int bytesToRead = fileSizeBytes - fileOffsetBytes;
				if (m_sizeBytes + bytesToRead <= kMaxSizeBytes)
				{
					if (fread(m_data + m_sizeBytes, 1, bytesToRead, pBinFile) != bytesToRead)
					{
						LOG_ERROR("Failed to read file: %s\n", binPath);
						error = true;
						break;
					}
					m_sizeBytes += bytesToRead;
				}
				else
				{
					LOG_ERROR("Disc image exceeds maximum size\n");
					error = true;
					break;
				}

				fclose(pBinFile);
				pBinFile = nullptr;
			}

			char filename[256]{};
			char fileType[32]{};
			// #TODO: Test cue sheet with unquoted filename e.g. FILE test.bin BINARY
			if (sscanf(p, "FILE \"%255[^\"]\" %31s", filename, fileType) != 2)
			{
				if (sscanf(p, "FILE %255s %31s", filename, fileType) != 2)
				{
					LOG_ERROR("Malformed FILE entry in cue file: %s\n", p);
					error = true;
					break;
				}
			}

			LOG_INFO("Found FILE entry: filename=\"%s\", type=%s\n", filename, fileType);

			if (strcmp(fileType, "BINARY") != 0)
			{
				LOG_ERROR("Unsuppored file type\n");
				error = true;
				break;
			}

			// Construct full path to .bin file
			const char* lastSlash = strrchr(cuePath, '/');
			if (!lastSlash)
				lastSlash = strrchr(cuePath, '\\');
			if (lastSlash)
			{
				size_t dirLength = (size_t)(lastSlash - cuePath) + 1; // include slash
				strncpy(binPath, cuePath, dirLength);
				binPath[dirLength] = '\0';
				strcat(binPath, filename);
			}
			else
			{
				// No directory component in cuePath
				SafeStrcpy(binPath, sizeof(binPath), filename);
			}

			pBinFile = fopen(binPath, "rb");
			if (!pBinFile)
			{
				LOG_ERROR("Failed to open bin file: %s\n", binPath);
				error = true;
				break;
			}

			fseek(pBinFile, 0, SEEK_END);
			fileSizeBytes = (unsigned int)ftell(pBinFile);
			fseek(pBinFile, 0, SEEK_SET);
			LOG_INFO("Opened bin file \"%s\" size %u (0x%X) bytes\n", binPath, fileSizeBytes, fileSizeBytes);

			// Each bin file has an implicit 2 second pregap that needs to be generated.
			if (m_sizeBytes + kPregapSizeBytes > kMaxSizeBytes)
			{
				LOG_ERROR("Disc image exceeds maximum size\n");
				error = true;
				break;
			}
			memset(m_data + m_sizeBytes, 0, kPregapSizeBytes);
			m_sizeBytes += kPregapSizeBytes;
		}
		else if (strncmp(p, "TRACK", 5) == 0)
		{
			// TRACK number datatype
			// e.g. TRACK 01 MODE2/2352

			unsigned int trackNumber;
			char trackType[32]{};
			if (sscanf(p, "TRACK %u %31s", &trackNumber, trackType) != 2)
			{
				LOG_ERROR("Malformed TRACK entry in cue file: %s\n", p);
				error = true;
				break;
			}

			LOG_INFO("Found TRACK entry: trackNumber=%u, trackType=%s\n", trackNumber, trackType);

			if (trackNumber != m_numTracks + 1)
			{
				LOG_ERROR("Unexpected track number %u, expected %u\n", trackNumber, m_numTracks + 1);
				error = true;
				break;
			}

			HP_ASSERT(m_numTracks < kMaxTracks, "Exceeded maximum number of supported tracks");

			// Determine track data type
			unsigned int trackDataTypeIndex;
			for (trackDataTypeIndex = 0; trackDataTypeIndex < ENUM_COUNT(CD::TrackDataType); trackDataTypeIndex++)
			{
				if (strcmp(trackType, kCDTrackDataTypeNames[trackDataTypeIndex]) == 0)
				{
					m_trackDataType[m_numTracks] = (CD::TrackDataType)trackDataTypeIndex;
					break;
				}
			}
			if (trackDataTypeIndex == ENUM_COUNT(CD::TrackDataType))
			{
				LOG_ERROR("Unsupported track data type: %s\n", trackType);
				error = true;
				break;
			}
			m_trackDataType[m_numTracks] = (CD::TrackDataType)trackDataTypeIndex;

			m_numTracks++;

			// Track LBA cannot be set until first INDEX is read.

			HP_ASSERT(pregapSectorsToInsert == 0, "Did previous TRACK have PREGAP but no INDEX commands?");
		}
		else if (strncmp(p, "PREGAP", 6) == 0)
		{
			// PREGAP mm:ss:ff
			// e.g. PREGAP 00:02:00
			// The PREGAP command must appear after a TRACK command, but before any INDEX commands.
			// Only one PREGAP command is allowed per track.

			unsigned int minutes, seconds, fragments;
			if (sscanf(p, "PREGAP %u:%u:%u", &minutes, &seconds, &fragments) != 3)
			{
				LOG_ERROR("Malformed PREGAP entry in cue file: %s\n", p);
				error = true;
				break;
			}

			HP_ASSERT(pregapSectorsToInsert == 0);
			pregapSectorsToInsert = CD::MSFtoLBA(minutes, seconds, fragments);

			// We can't insert the pregap until we know the index (pos) at which is will be inserted, which will
			// come from the next INDEX command!
		}
		else if (strncmp(p, "INDEX", 5) == 0)
		{
			// INDEX number mm:ss:ff
			// e.g. INDEX 01 00:00:00
			// "index" means position within the current FILE.
			// INDEX 00 is optional specifies the starting time of the track pregap. #TODO: I think this is stored in the .bin file.
			// INDEX 01 is required and specifies the starting time of the track data.

			unsigned int index, minutes, seconds, fragments;
			if (sscanf(p, "INDEX %u %u:%u:%u", &index, &minutes, &seconds, &fragments) != 4)
			{
				LOG_ERROR("Malformed INDEX entry in cue file: %s\n", p);
				error = true;
				break;
			}

			unsigned int indexSectors = CD::MSFtoLBA(minutes, seconds, fragments);
			unsigned int indexBytes = indexSectors * CD::kSectorSizeBytes;

			// Read up to this INDEX position
			if (!pBinFile)
			{
				LOG_ERROR("INDEX encountered outside of FILE\n");
				error = true;
				break;
			}
			unsigned int fileOffsetBytes = (unsigned int)ftell(pBinFile);
			if (indexBytes > fileOffsetBytes) // will be zero for TRACK 01
			{
				unsigned int bytesToRead = indexBytes - fileOffsetBytes;

				if (m_sizeBytes + bytesToRead > kMaxSizeBytes)
				{
					LOG_ERROR("Disc image exceeds maximum size\n");
					error = true;
					break;
				}

				if (fread(m_data + m_sizeBytes, 1, bytesToRead, pBinFile) != bytesToRead)
				{
					LOG_ERROR("Failed to read file: %s\n", binPath);
					error = true;
					break;
				}
				m_sizeBytes += bytesToRead;
			}

			// Generate any PREGAP for this track (which has not yet been read)
			if (pregapSectorsToInsert > 0)
			{
				unsigned int pregapSizeBytes = pregapSectorsToInsert * CD::kSectorSizeBytes;
				if (m_sizeBytes + pregapSizeBytes > kMaxSizeBytes)
				{
					LOG_ERROR("Disc image exceeds maximum size\n");
					error = true;
					break;
				}
				pregapSectorsToInsert = 0;

				memset(m_data + m_sizeBytes, 0, pregapSizeBytes);
				m_sizeBytes += pregapSizeBytes;
			}

			// Track LBA can be calculated when the first INDEX is encountered
			if (m_numTracks == 0)
			{
				LOG_ERROR("INDEX encountered outside of TRACK\n");
				error = true;
				break;
			}
			unsigned int trackIndex = m_numTracks - 1;
			if (index == 0 && m_trackIndex0LBA[trackIndex] == 0)
			{
				HP_ASSERT(m_sizeBytes % CD::kSectorSizeBytes == 0, "Track LBAs must be sector-aligned");
				m_trackIndex0LBA[trackIndex] = m_sizeBytes / CD::kSectorSizeBytes;
			}
			else if (index == 1 && m_trackIndex1LBA[trackIndex] == 0)
			{
				HP_ASSERT(m_sizeBytes % CD::kSectorSizeBytes == 0, "Track LBAs must be sector-aligned");
				m_trackIndex1LBA[trackIndex] = m_sizeBytes / CD::kSectorSizeBytes;
			}
		}
		else if (strncmp(p, "FLAGS", 5) == 0)
		{
			// e.g. FLAGS DCP
			// Ignore
		}
		else
		{
			HP_FATAL_ERROR("Unsupported CUE sheet command: %s", p);
		}
	}

	if (!error)
	{
		HP_ASSERT(pregapSectorsToInsert == 0);

		if (pBinFile)
		{
			// read remainder of bin file
			unsigned int fileOffsetBytes = (unsigned int)ftell(pBinFile);
			HP_ASSERT(fileOffsetBytes < fileSizeBytes);
			unsigned int bytesToRead = fileSizeBytes - fileOffsetBytes;
			if (m_sizeBytes + bytesToRead <= kMaxSizeBytes)
			{
				if (fread(m_data + m_sizeBytes, 1, bytesToRead, pBinFile) != bytesToRead)
				{
					LOG_ERROR("Failed to read file: %s\n", binPath);
					error = true;
				}
				m_sizeBytes += bytesToRead;
			}
			else
			{
				LOG_ERROR("Disc image exceeds maximum size\n");
				error = true;
			}
		}
	}

	if (pBinFile)
	{
		fclose(pBinFile);
		pBinFile = nullptr;
	}
	fclose(pCueFile);
	pCueFile = nullptr;

	if (error)
		return false;

	setPathAndName(cuePath);

	HP_ASSERT((m_sizeBytes% kSectorSizeBytes) == 0);
	m_sizeSectors = m_sizeBytes / kSectorSizeBytes;

	return true;
}

unsigned int CD::GetTrackIndex0LBA(unsigned int trackIndex) const
{
	HP_ASSERT(trackIndex < m_numTracks, "Track index out of range");
	return m_trackIndex0LBA[trackIndex];
}

unsigned int CD::GetTrackStartLBA(unsigned int trackIndex) const
{
	HP_ASSERT(trackIndex < m_numTracks, "Track index out of range");

	// The track starts at INDEX 1. All tracks have INDEX 1, but not all have INDEX 0.
	// Index 0 is the start of the pregap.
	return m_trackIndex1LBA[trackIndex];
}

CD::TrackDataType CD::GetTrackDataType(unsigned int trackIndex) const
{
	HP_ASSERT(trackIndex < m_numTracks);
	return m_trackDataType[trackIndex];
}

unsigned int CD::GetTrackFinalLBA(unsigned int trackIndex) const
{
	HP_ASSERT(trackIndex < m_numTracks);
	if (trackIndex + 1 < m_numTracks) // not final track
	{
		return m_trackIndex0LBA[trackIndex + 1];
	}
	else // final track
	{
		return m_sizeSectors - 1;
	}
}

void CD::setPathAndName(const char* path)
{
	SafeStrcpy(m_path, sizeof(m_path), path);

	FilenameWithoutExtensionFromPath(m_path, m_name, sizeof(m_name));

	// Remove any prefix ending with -
	char* firstDash = strchr(m_name, '-');
	if (firstDash)
	{
		size_t prefixLength = (size_t)(firstDash - m_name) + 1; // include dash
		size_t nameLength = strlen(m_name);
		size_t newNameLength = nameLength - prefixLength;
		memmove(m_name, m_name + prefixLength, newNameLength + 1); // include null terminator (memory in memmove can overlap, unlike memcpy)
	}
}
