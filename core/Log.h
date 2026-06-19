#pragma once

// Originally adapted from https://github.com/amigadev/modpack
//
// n.b. macOS Application Bundle output appears in vscode Terminal rather than Output Window.
//

#include <stdio.h> // FILE

#define LOG_LEVEL_NONE (-3)
#define LOG_LEVEL_ERROR (-2)
#define LOG_LEVEL_WARN (-1)
#define LOG_LEVEL_INFO (0)
#define LOG_LEVEL_DEBUG (1)
#define LOG_LEVEL_TRACE (2)

#define LOG_LEVEL_MIN LOG_LEVEL_NONE
#define LOG_LEVEL_MAX LOG_LEVEL_TRACE

void SetLogLevel(int logLevel);
int GetLogLevel();

// Always logs, regardless of level
// pStream should be stdout or stderr
void LogMsg(FILE* pStream, const char* format, ...);

void LogMsgV(int logLevel, FILE* pStream, const char* format, va_list argList);

// Log if logLevel > global log level (set with SetLogLevel)
void LogLevel(int logLevel, const char* format, ...);
void LogLevelV(int logLevel, const char* format, va_list argList);

typedef void (*LogCallback)(int logLevel, const char* format, va_list argList);
void SetLogCallback(LogCallback pCallback);

#define LOG_ERROR(...) LogLevel(LOG_LEVEL_ERROR, "ERROR: " __VA_ARGS__)
#define LOG_WARN(...) LogLevel(LOG_LEVEL_WARN, "WARN: " __VA_ARGS__)
#define LOG_INFO(...) LogLevel(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) LogLevel(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) LogLevel(LOG_LEVEL_TRACE, __VA_ARGS__)

// Helper function to LOG_INFO if verbose is true, otherwise LOG_TRACE
void Log(bool verbose, const char* format, ...);
