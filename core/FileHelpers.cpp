#include "FileHelpers.h"

#include "Log.h"
#include "ArrayHelpers.h"
#include "StringHelpers.h"
#include "hp_assert.h"

#include <filesystem>

#ifdef _MSC_VER
#include <Windows.h> // wchar
#else
#include <libgen.h> // basename
#endif

bool FileExists(const char* path)
{
	HP_ASSERT(path && path[0]);

	std::error_code ec;

#ifdef _MSC_VER
	// std::filesystem uses wchar on Windows.
	// SDL uses UTF-8, so we need to convert to wide char.
	WCHAR wpath[320];
	::MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, COUNTOF_ARRAY(wpath));
	bool exists = std::filesystem::exists(wpath, ec);
#else
	bool exists = std::filesystem::exists(path, ec);
#endif
	if (ec)
	{
		LOG_ERROR("std::filesystem::exists(%s) failed : %s", path, ec.message().c_str());
		return false;
	}

	return exists;
}

void FileNameWithoutExtension(const char* fileName, char fileNameWithoutExtention[], size_t fileNameWithoutExtentionSize)
{
	HP_ASSERT(fileName && fileName[0]);

	namespace fs = std::filesystem;

	const fs::path path(fileName);
	const fs::path stem = path.stem();
	const std::string strStem = stem.string();
	const char* szStem = strStem.c_str();
	SafeStrcpy(fileNameWithoutExtention, fileNameWithoutExtentionSize, szStem);
}

void FilenameWithExtensionFromPath(const char* path, char* fileNameBuffer, size_t bufferSize)
{
	HP_ASSERT(path && path[0]);
	HP_ASSERT(fileNameBuffer);

#ifdef WIN32
	//	char drive[256];
	//	char dir[256];
	char filename[256];
	char ext[256];
	_splitpath_s(
		path,
		/*drive*/NULL, /*sizeof(drive)*/0,        // e.g. "C:"
		/*dir*/NULL, /*sizeof(dir)*/0,            // e.g. "\dev\howprice\iragui\build\Debug\"
		filename, sizeof(filename),  // filename e.g. Calculator
		ext, sizeof(ext));           // e.g. ".cnf" or ".asm". Maybe even ".exe"

	_makepath_s(fileNameBuffer, bufferSize, /*drive*/NULL, /*dir*/NULL, filename, ext);
#else
	// basename may be destructive, so make a copy
	char pathCopy[256];
	SafeStrcpy(pathCopy, sizeof(pathCopy), path);
	const char* filename = basename(pathCopy);
	SafeStrcpy(fileNameBuffer, bufferSize, filename);
#endif
}

void FilenameWithoutExtensionFromPath(const char* path, char* fileNameBuffer, size_t bufferSize)
{
	HP_ASSERT(path && path[0]);
	HP_ASSERT(fileNameBuffer);

#ifdef WIN32
	//	char drive[256];
	//	char dir[256];
	char filename[256];
	char ext[256];
	_splitpath_s(
		path,
		/*drive*/NULL, /*sizeof(drive)*/0,        // e.g. "C:"
		/*dir*/NULL, /*sizeof(dir)*/0,            // e.g. "\dev\howprice\iragui\build\Debug\"
		filename, sizeof(filename),  // filename e.g. Calculator
		ext, sizeof(ext));           // e.g. ".cnf" or ".asm". Maybe even ".exe"

	_makepath_s(fileNameBuffer, bufferSize, /*drive*/NULL, /*dir*/NULL, filename, /*ext*/NULL);
#else
	// basename may be destructive, so make a copy
	char pathCopy[256];
	SafeStrcpy(pathCopy, sizeof(pathCopy), path);
	const char* filename = basename(pathCopy);

	namespace fs = std::filesystem;
	const fs::path filenamePath(filename);
	const fs::path stem = filenamePath.stem();
	const std::string strStem = stem.string();
	const char* szStem = strStem.c_str();
	SafeStrcpy(fileNameBuffer, bufferSize, szStem);
#endif
}
