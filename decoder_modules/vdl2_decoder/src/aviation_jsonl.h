#pragma once
#include <string>

// Append one validated object, compacted to one newline-terminated record.
// A tailing reader must buffer bytes until it sees a newline.
bool appendAviationJSONL(const std::string& path, const std::string& object);
