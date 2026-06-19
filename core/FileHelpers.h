#pragma once

#ifdef __GNUC__
#include <stddef.h> // size_t
#endif

bool FileExists(const char* path);
void FileNameWithoutExtension(const char* fileName, char fileNameWithoutExtention[], size_t fileNameWithoutExtentionSize);
void FilenameWithExtensionFromPath(const char* path, char* fileNameBuffer, size_t bufferSize);
void FilenameWithoutExtensionFromPath(const char* path, char* fileNameBuffer, size_t bufferSize);
