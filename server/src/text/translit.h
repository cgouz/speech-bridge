// Uzbek Latin <-> Cyrillic transliteration. Direct port of
// app/internal/text/translit.go — see that file's doc comment.
#pragma once

#include <string>

namespace sb::text {

std::string LatinToCyrillic(const std::string &s);
std::string CyrillicToLatin(const std::string &s);

}  // namespace sb::text
