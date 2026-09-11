// unicode_lite — minimal Unicode helpers covering exactly what this project's
// pipeline needs: UTF-8 <-> UTF-32 conversion, and case/class queries over
// ASCII + Latin-1 Supplement + Cyrillic (the scripts actually seen in
// en/ru/uz/kaa text). This is NOT a general Unicode library — e.g. IsDigit
// only recognizes ASCII 0-9 (Go's unicode.IsDigit is broader, but every
// number this pipeline ever sees is ASCII).
#pragma once

#include <string>

namespace sb::text {

using rune = char32_t;

// Decodes UTF-8 to UTF-32 codepoints. Invalid sequences are replaced with
// U+FFFD, matching Go's utf8.DecodeRune behavior closely enough for text this
// pipeline handles (it never sees hostile input here — audio transcripts and
// operator-supplied strings).
std::u32string Utf8Decode(const std::string &s);
std::string Utf8Encode(const std::u32string &s);
std::string Utf8Encode(rune r);

bool IsSpace(rune r);
bool IsDigit(rune r);  // ASCII 0-9 only — see header comment.
bool IsUpper(rune r);
bool IsLower(rune r);
bool IsLetter(rune r);
rune ToUpper(rune r);
rune ToLower(rune r);

std::string ToLower(const std::string &utf8);
std::string ToUpper(const std::string &utf8);
std::string TrimSpace(const std::string &utf8);
// TrimRight of a specific ASCII cutset, mirroring strings.TrimRight(s, cutset).
std::string TrimRightASCII(const std::string &utf8, const char *cutset);

}  // namespace sb::text
