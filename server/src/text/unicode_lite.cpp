#include "unicode_lite.h"

namespace sb::text {

std::u32string Utf8Decode(const std::string &s) {
  std::u32string out;
  out.reserve(s.size());
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    rune r;
    size_t len;
    if (c < 0x80) {
      r = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      r = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      r = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      r = c & 0x07;
      len = 4;
    } else {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    if (i + len > n) {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    bool ok = true;
    rune acc = r;
    for (size_t k = 1; k < len; k++) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      acc = (acc << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    out.push_back(acc);
    i += len;
  }
  return out;
}

std::string Utf8Encode(rune r) {
  std::string out;
  if (r < 0x80) {
    out.push_back(static_cast<char>(r));
  } else if (r < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (r >> 6)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  } else if (r < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (r >> 12)));
    out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (r >> 18)));
    out.push_back(static_cast<char>(0x80 | ((r >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
  }
  return out;
}

std::string Utf8Encode(const std::u32string &s) {
  std::string out;
  out.reserve(s.size());
  for (rune r : s) out += Utf8Encode(r);
  return out;
}

bool IsSpace(rune r) {
  switch (r) {
    case 0x20:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
      return true;
    default:
      return r >= 0x2000 && r <= 0x200A;
  }
}

bool IsDigit(rune r) { return r >= '0' && r <= '9'; }

bool IsUpper(rune r) {
  if (r >= 'A' && r <= 'Z') return true;
  if (r >= 0xC0 && r <= 0xDE && r != 0xD7) return true;             // Latin-1 upper
  if (r >= 0x0410 && r <= 0x042F) return true;                       // А-Я
  if (r >= 0x0400 && r <= 0x040F) return true;                       // Ѐ-Џ
  if (r == 0x0492 || r == 0x049A || r == 0x04B2) return true;        // Ғ Қ Ҳ (Uzbek Cyrillic)
  return false;
}

bool IsLower(rune r) {
  if (r >= 'a' && r <= 'z') return true;
  if (r >= 0xDF && r <= 0xFF && r != 0xF7) return true;              // Latin-1 lower
  if (r >= 0x0430 && r <= 0x044F) return true;                       // а-я
  if (r >= 0x0450 && r <= 0x045F) return true;                       // ѐ-џ
  if (r == 0x0493 || r == 0x049B || r == 0x04B3) return true;        // ғ қ ҳ (Uzbek Cyrillic)
  return false;
}

bool IsLetter(rune r) {
  if (IsUpper(r) || IsLower(r)) return true;
  if (r == 0x02BB || r == 0x02BC) return true;  // ʻ ʼ (Uzbek okina / tutuq)
  return false;
}

rune ToUpper(rune r) {
  if (r >= 'a' && r <= 'z') return r - 32;
  if (r >= 0xDF && r <= 0xFE && r != 0xF7) return r - 32;
  if (r >= 0x0430 && r <= 0x044F) return r - 0x20;
  if (r >= 0x0450 && r <= 0x045F) return r - 0x50;
  if (r == 0x0493) return 0x0492;  // ғ -> Ғ
  if (r == 0x049B) return 0x049A;  // қ -> Қ
  if (r == 0x04B3) return 0x04B2;  // ҳ -> Ҳ
  return r;
}

rune ToLower(rune r) {
  if (r >= 'A' && r <= 'Z') return r + 32;
  if (r >= 0xC0 && r <= 0xDE && r != 0xD7) return r + 32;
  if (r >= 0x0410 && r <= 0x042F) return r + 0x20;
  if (r >= 0x0400 && r <= 0x040F) return r + 0x50;
  if (r == 0x0492) return 0x0493;  // Ғ -> ғ
  if (r == 0x049A) return 0x049B;  // Қ -> қ
  if (r == 0x04B2) return 0x04B3;  // Ҳ -> ҳ
  return r;
}

std::string ToLower(const std::string &utf8) {
  std::u32string r = Utf8Decode(utf8);
  for (auto &c : r) c = ToLower(c);
  return Utf8Encode(r);
}

std::string ToUpper(const std::string &utf8) {
  std::u32string r = Utf8Decode(utf8);
  for (auto &c : r) c = ToUpper(c);
  return Utf8Encode(r);
}

std::string TrimSpace(const std::string &utf8) {
  std::u32string r = Utf8Decode(utf8);
  size_t start = 0, end = r.size();
  while (start < end && IsSpace(r[start])) start++;
  while (end > start && IsSpace(r[end - 1])) end--;
  return Utf8Encode(r.substr(start, end - start));
}

std::string TrimRightASCII(const std::string &utf8, const char *cutset) {
  std::u32string r = Utf8Decode(utf8);
  size_t end = r.size();
  auto inCutset = [&](rune c) {
    for (const char *p = cutset; *p; p++)
      if (static_cast<rune>(static_cast<unsigned char>(*p)) == c) return true;
    return false;
  };
  while (end > 0 && r[end - 1] < 0x80 && inCutset(r[end - 1])) end--;
  return Utf8Encode(r.substr(0, end));
}

}  // namespace sb::text
