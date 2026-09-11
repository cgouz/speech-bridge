#include "translit.h"

#include "unicode_lite.h"

namespace sb::text {

namespace {

constexpr rune kTurnedComma = 0x02BB;  // ʻ — the "okina" used in oʻ / gʻ
constexpr rune kModApos = 0x02BC;      // ʼ — tutuq belgisi (glottal stop)

bool IsApostrophe(rune r) {
  switch (r) {
    case '\'':
    case '`':
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x02BB:  // ʻ
    case 0x02BC:  // ʼ
    case 0x00B4:  // ´
    case 0x02B9:  // ʹ
    case 0x201B:  // ‛
      return true;
    default:
      return false;
  }
}

bool IsWordBreak(rune r) {
  switch (r) {
    case '-':
    case 0x2014:  // —
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case '"':
    case '(':
    case ')':
    case 0x00AB:  // «
    case 0x00BB:  // »
    case '\n':
    case '\t':
    case '/':
      return true;
    default:
      return false;
  }
}

// keepCase copies the case pattern of src onto dst (UTF-8). Mirrors
// translit.go's keepCase exactly.
std::string KeepCase(const std::u32string &src, const std::string &dstUtf8) {
  if (src.empty() || !IsUpper(src[0])) return dstUtf8;
  if (src.size() > 1 && IsUpper(src[1])) return ToUpper(dstUtf8);
  std::u32string dr = Utf8Decode(dstUtf8);
  if (dr.empty()) return dstUtf8;
  dr[0] = ToUpper(dr[0]);
  return Utf8Encode(dr);
}

// caseFold applies the case of a Cyrillic source rune (with lookahead for
// ALL-CAPS runs) to a multi-letter Latin output.
std::string CaseFold(rune src, size_t i, const std::u32string &all, const std::string &dstUtf8) {
  if (!IsUpper(src)) return dstUtf8;
  size_t next = i + 1;
  if (next < all.size() && IsUpper(all[next])) return ToUpper(dstUtf8);
  if (i > 0 && IsUpper(all[i - 1])) return ToUpper(dstUtf8);
  std::u32string dr = Utf8Decode(dstUtf8);
  if (dr.empty()) return dstUtf8;
  dr[0] = ToUpper(dr[0]);
  return Utf8Encode(dr);
}

}  // namespace

std::string LatinToCyrillic(const std::string &s) {
  std::u32string r = Utf8Decode(s);
  std::string b;
  bool atWordStart = true;

  auto emit = [&](const std::u32string &src, const std::string &dst) { b += KeepCase(src, dst); };

  for (size_t i = 0; i < r.size(); i++) {
    rune c = r[i];
    rune lc = ToLower(c);

    // two-rune digraphs with an apostrophe: oʻ, gʻ
    if ((lc == 'o' || lc == 'g') && i + 1 < r.size() && IsApostrophe(r[i + 1])) {
      emit(r.substr(i, 2), lc == 'o' ? "ў" : "ғ");
      i++;
      atWordStart = false;
      continue;
    }

    // two-rune Latin digraphs: sh, ch, yo, yu, ya, ye, ts
    if (i + 1 < r.size()) {
      rune c0 = ToLower(r[i]), c1 = ToLower(r[i + 1]);
      std::string pair2;
      if (c0 == 's' && c1 == 'h') pair2 = "ш";
      else if (c0 == 'c' && c1 == 'h') pair2 = "ч";
      else if (c0 == 'y' && c1 == 'o') pair2 = "ё";
      else if (c0 == 'y' && c1 == 'u') pair2 = "ю";
      else if (c0 == 'y' && c1 == 'a') pair2 = "я";
      else if (c0 == 'y' && c1 == 'e') pair2 = "е";
      else if (c0 == 't' && c1 == 's') pair2 = "ц";
      if (!pair2.empty()) {
        emit(r.substr(i, 2), pair2);
        i++;
        atWordStart = false;
        continue;
      }
    }

    if (IsApostrophe(c)) {
      b += Utf8Encode(static_cast<rune>(U'ъ'));
      atWordStart = false;
      continue;
    }

    std::string mapped;
    switch (lc) {
      case 'a': mapped = "а"; break;
      case 'b': mapped = "б"; break;
      case 'd': mapped = "д"; break;
      case 'e': mapped = atWordStart ? "э" : "е"; break;
      case 'f': mapped = "ф"; break;
      case 'g': mapped = "г"; break;
      case 'h': mapped = "ҳ"; break;
      case 'i': mapped = "и"; break;
      case 'j': mapped = "ж"; break;
      case 'k': mapped = "к"; break;
      case 'l': mapped = "л"; break;
      case 'm': mapped = "м"; break;
      case 'n': mapped = "н"; break;
      case 'o': mapped = "о"; break;
      case 'p': mapped = "п"; break;
      case 'q': mapped = "қ"; break;
      case 'r': mapped = "р"; break;
      case 's': mapped = "с"; break;
      case 't': mapped = "т"; break;
      case 'u': mapped = "у"; break;
      case 'v': mapped = "в"; break;
      case 'x': mapped = "х"; break;
      case 'y': mapped = "й"; break;
      case 'z': mapped = "з"; break;
      case 'c': mapped = "к"; break;  // bare 'c' is rare in Uzbek Latin; approximate
      case 'w': mapped = "в"; break;
      default:
        b += Utf8Encode(c);
        if (IsSpace(c) || IsWordBreak(c)) atWordStart = true;
        continue;
    }
    emit(std::u32string(1, c), mapped);
    atWordStart = false;
  }
  return b;
}

std::string CyrillicToLatin(const std::string &s) {
  std::u32string r = Utf8Decode(s);
  std::string b;
  bool atWordStart = true;

  auto emit = [&](rune src, const std::string &dst) { b += KeepCase(std::u32string(1, src), dst); };

  for (size_t i = 0; i < r.size(); i++) {
    rune c = r[i];
    rune lc = ToLower(c);

    std::string mapped;
    switch (lc) {
      case U'а': mapped = "a"; break;
      case U'б': mapped = "b"; break;
      case U'в': mapped = "v"; break;
      case U'г': mapped = "g"; break;
      case U'ғ': mapped = std::string("g") + Utf8Encode(kTurnedComma); break;
      case U'д': mapped = "d"; break;
      case U'е': mapped = atWordStart ? "ye" : "e"; break;
      case U'ё': mapped = "yo"; break;
      case U'ж': mapped = "j"; break;
      case U'з': mapped = "z"; break;
      case U'и': mapped = "i"; break;
      case U'й': mapped = "y"; break;
      case U'к': mapped = "k"; break;
      case U'қ': mapped = "q"; break;
      case U'л': mapped = "l"; break;
      case U'м': mapped = "m"; break;
      case U'н': mapped = "n"; break;
      case U'о': mapped = "o"; break;
      case U'п': mapped = "p"; break;
      case U'р': mapped = "r"; break;
      case U'с': mapped = "s"; break;
      case U'т': mapped = "t"; break;
      case U'у': mapped = "u"; break;
      case U'ў': mapped = std::string("o") + Utf8Encode(kTurnedComma); break;
      case U'ф': mapped = "f"; break;
      case U'х': mapped = "x"; break;
      case U'ҳ': mapped = "h"; break;
      case U'ц': mapped = "ts"; break;
      case U'ч': mapped = "ch"; break;
      case U'ш': mapped = "sh"; break;
      case U'щ': mapped = "sh"; break;
      case U'ъ': mapped = Utf8Encode(kModApos); break;
      case U'ь': mapped = ""; break;  // soft sign: dropped in Uzbek Latin
      case U'э': mapped = "e"; break;
      case U'ю': mapped = "yu"; break;
      case U'я': mapped = "ya"; break;
      default:
        b += Utf8Encode(c);
        if (IsSpace(c) || IsWordBreak(c)) atWordStart = true;
        continue;
    }

    if (!mapped.empty()) {
      // keep case for multi-rune outputs too (Sh, SH, sh) -- byte length,
      // matching Go's len(mapped) > 1 on the same ASCII/mixed strings.
      if (mapped.size() > 1) {
        b += CaseFold(c, i, r, mapped);
      } else {
        emit(c, mapped);
      }
    }
    atWordStart = false;
  }
  return b;
}

}  // namespace sb::text
