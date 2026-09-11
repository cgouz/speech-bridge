// Sentence splitter for the ru/uz/kaa/en pipeline. Direct port of
// app/internal/text/split.go — see that file's doc comment for the rules.
#pragma once

#include <string>
#include <vector>

namespace sb::text {

std::vector<std::string> SplitSentences(const std::string &text);

}  // namespace sb::text
