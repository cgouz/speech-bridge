#include "split.h"

#include <unordered_set>

#include "unicode_lite.h"

namespace sb::text {

namespace {

bool IsSentenceEnder(rune r) {
  switch (r) {
    case U'.':
    case U'!':
    case U'?':
    case U'…':  // …
    case U'。':  // 。
    case U'؟':  // ؟
      return true;
    default:
      return false;
  }
}

bool IsClosing(rune r) {
  switch (r) {
    case U'"':
    case U'\'':
    case U'»':  // »
    case U'”':  // ”
    case U'’':  // ’
    case U')':
    case U']':
    case U'}':
    case U'«':  // «
      return true;
    default:
      return false;
  }
}

bool SentenceStart(rune r) {
  if (IsUpper(r) || IsDigit(r)) return true;
  switch (r) {
    case U'"':
    case U'\'':
    case U'«':  // «
    case U'“':  // “
    case U'‘':  // ‘
    case U'(':
    case U'[':
    case U'—':  // —
    case U'-':
    case U'…':  // …
    case U'¿':  // ¿
    case U'№':  // №
      return true;
    default:
      break;
  }
  return IsLetter(r) && !IsLower(r);
}

bool InsideNumber(const std::u32string &r, size_t dot) {
  rune prev = 0, next = 0;
  for (size_t p = dot; p-- > 0;) {
    if (IsSpace(r[p])) continue;
    prev = r[p];
    break;
  }
  if (dot + 1 < r.size()) next = r[dot + 1];
  return IsDigit(prev) && IsDigit(next);
}

bool IsEllipsisRun(const std::u32string &r, size_t i) {
  int n = 0;
  for (size_t k = i; k < r.size() && r[k] == U'.'; k++) n++;
  for (size_t k = i; k-- > 0 && r[k] == U'.';) n++;
  return n >= 3;
}

// Trailing non-space token of r[0:end) (end exclusive), as UTF-8.
std::string LastToken(const std::u32string &r, size_t end) {
  size_t e = end;
  while (e > 0 && IsSpace(r[e - 1])) e--;
  size_t start = e;
  while (start > 0 && !IsSpace(r[start - 1])) start--;
  return Utf8Encode(r.substr(start, e - start));
}

bool IsInitial(const std::string &tokUtf8) {
  std::string tok = TrimRightASCII(tokUtf8, ".");
  std::u32string r = Utf8Decode(tok);
  if (r.size() != 1) return false;
  return IsUpper(r[0]);
}

const std::unordered_set<std::string> &Abbreviations() {
  static const std::unordered_set<std::string> kAbbrev = {
      // English
      "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc", "no",
      "vol", "fig", "al", "inc", "ltd", "co", "e.g", "i.e",
      // Russian
      "т", "д", "п", "г", "гг", "др", "см", "стр", "рис", "табл", "им",
      "проф", "акад", "руб", "коп", "ул", "пр", "обл", "респ",
      // Uzbek (Latin + Cyrillic)
      "va", "h.k", "b", "sh", "hok", "ва", "ҳ.к",
  };
  return kAbbrev;
}

}  // namespace

std::vector<std::string> SplitSentences(const std::string &text) {
  std::u32string runes = Utf8Decode(text);
  std::vector<std::string> out;
  std::u32string buf;

  auto flush = [&]() {
    std::string s = TrimSpace(Utf8Encode(buf));
    if (!s.empty()) out.push_back(s);
    buf.clear();
  };

  for (size_t i = 0; i < runes.size(); i++) {
    rune r = runes[i];
    buf.push_back(r);

    if (!IsSentenceEnder(r)) continue;

    size_t j = i + 1;
    while (j < runes.size() && (IsSentenceEnder(runes[j]) || IsClosing(runes[j]))) {
      buf.push_back(runes[j]);
      j++;
    }

    if (j >= runes.size()) {
      i = j - 1;
      break;
    }
    if (!IsSpace(runes[j])) {
      i = j - 1;
      continue;
    }

    if ((r == U'.' && IsEllipsisRun(runes, i)) || r == U'…') {
      i = j - 1;
      continue;
    }

    if (r == U'.') {
      if (InsideNumber(runes, i)) {
        i = j - 1;
        continue;
      }
      std::string tok = LastToken(buf, buf.size() - 1);
      std::string key = ToLower(TrimRightASCII(tok, "."));
      if (Abbreviations().count(key)) {
        i = j - 1;
        continue;
      }
      if (IsInitial(tok)) {
        i = j - 1;
        continue;
      }
    }

    size_t k = j;
    while (k < runes.size() && IsSpace(runes[k])) k++;
    if (k < runes.size() && !SentenceStart(runes[k])) {
      i = j - 1;
      continue;
    }

    flush();
    i = j - 1;
  }
  flush();
  return out;
}

}  // namespace sb::text
