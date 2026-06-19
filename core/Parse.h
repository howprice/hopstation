#pragma once

bool ParseInt(const char* arg, int& val);

bool ParseUnsignedInt(const char* arg, unsigned int& val);

// arg should start with "0x" or "0X" for internal strtoul call, but seems to work without.
[[nodiscard]]
bool ParseHexUnsignedInt(const char* arg, unsigned int& val);

bool ParseFloat(const char* arg, float& val);
