package text

import (
	"strings"
	"unicode"
)

// Uzbek Latin <-> Cyrillic transliteration.
//
// MADLAD-400 emits Uzbek in Cyrillic. For display when target=uz the pipeline
// converts to Latin (the everyday script in Uzbekistan since 1995); the MMS uz
// TTS voice is fed Cyrillic directly (see docs/models.md), so synthesis does
// not use these functions.
//
// The digraphs the mission calls out — oʻ, gʻ, sh, ch, ng — and the tutuq
// belgisi (ъ / ʼ) are handled explicitly. Natural-language transliteration is
// never perfectly bijective; the common cases round-trip (see translit_test.go).
//
// Apostrophe forms accepted on input for oʻ/gʻ and the tutuq belgisi:
// ' ` ‘ ’ ʻ ʼ ´ . Canonical output: ʻ (U+02BB) in oʻ/gʻ, ʼ (U+02BC) for ъ.

const (
	turnedComma = 'ʻ' // U+02BB — the "okina" used in oʻ / gʻ
	modApos     = 'ʼ' // U+02BC — tutuq belgisi (glottal stop)
)

func isApostrophe(r rune) bool {
	switch r {
	case '\'', '`', '‘', '’', 'ʻ', 'ʼ', '´', 'ʹ', '‛':
		return true
	}
	return false
}

func isVowelLatin(r rune) bool {
	switch unicode.ToLower(r) {
	case 'a', 'e', 'i', 'o', 'u':
		return true
	}
	return false
}

// keepCase copies the case pattern of src onto the ASCII/Cyrillic string dst.
// If src's first rune is upper and it has >1 rune with the second also upper,
// dst is upper-cased entirely; if only the first is upper, dst is title-cased;
// otherwise dst is returned unchanged.
func keepCase(src string, dst string) string {
	sr := []rune(src)
	if len(sr) == 0 || !unicode.IsUpper(sr[0]) {
		return dst
	}
	if len(sr) > 1 && unicode.IsUpper(sr[1]) {
		return strings.ToUpper(dst)
	}
	dr := []rune(dst)
	if len(dr) == 0 {
		return dst
	}
	dr[0] = unicode.ToUpper(dr[0])
	return string(dr)
}

// LatinToCyrillic transliterates Uzbek Latin text to Cyrillic.
func LatinToCyrillic(s string) string {
	r := []rune(s)
	var b strings.Builder
	atWordStart := true

	emit := func(src, dst string) {
		b.WriteString(keepCase(src, dst))
	}

	for i := 0; i < len(r); i++ {
		c := r[i]
		lc := unicode.ToLower(c)

		// two-rune digraphs with an apostrophe: oʻ, gʻ
		if (lc == 'o' || lc == 'g') && i+1 < len(r) && isApostrophe(r[i+1]) {
			if lc == 'o' {
				emit(string(r[i:i+2]), "ў")
			} else {
				emit(string(r[i:i+2]), "ғ")
			}
			i++
			atWordStart = false
			continue
		}

		// two-rune Latin digraphs: sh, ch, yo, yu, ya, ye, ts
		if i+1 < len(r) {
			pair := strings.ToLower(string(r[i : i+2]))
			switch pair {
			case "sh":
				emit(string(r[i:i+2]), "ш")
				i++
				atWordStart = false
				continue
			case "ch":
				emit(string(r[i:i+2]), "ч")
				i++
				atWordStart = false
				continue
			case "yo":
				emit(string(r[i:i+2]), "ё")
				i++
				atWordStart = false
				continue
			case "yu":
				emit(string(r[i:i+2]), "ю")
				i++
				atWordStart = false
				continue
			case "ya":
				emit(string(r[i:i+2]), "я")
				i++
				atWordStart = false
				continue
			case "ye":
				emit(string(r[i:i+2]), "е")
				i++
				atWordStart = false
				continue
			case "ts":
				emit(string(r[i:i+2]), "ц")
				i++
				atWordStart = false
				continue
			}
		}

		if isApostrophe(c) {
			b.WriteRune('ъ') // standalone apostrophe = tutuq belgisi
			atWordStart = false
			continue
		}

		var mapped string
		switch lc {
		case 'a':
			mapped = "а"
		case 'b':
			mapped = "б"
		case 'd':
			mapped = "д"
		case 'e':
			if atWordStart {
				mapped = "э"
			} else {
				mapped = "е"
			}
		case 'f':
			mapped = "ф"
		case 'g':
			mapped = "г"
		case 'h':
			mapped = "ҳ"
		case 'i':
			mapped = "и"
		case 'j':
			mapped = "ж"
		case 'k':
			mapped = "к"
		case 'l':
			mapped = "л"
		case 'm':
			mapped = "м"
		case 'n':
			mapped = "н"
		case 'o':
			mapped = "о"
		case 'p':
			mapped = "п"
		case 'q':
			mapped = "қ"
		case 'r':
			mapped = "р"
		case 's':
			mapped = "с"
		case 't':
			mapped = "т"
		case 'u':
			mapped = "у"
		case 'v':
			mapped = "в"
		case 'x':
			mapped = "х"
		case 'y':
			mapped = "й"
		case 'z':
			mapped = "з"
		case 'c':
			mapped = "к" // bare 'c' is rare in Uzbek Latin; approximate
		case 'w':
			mapped = "в"
		default:
			b.WriteRune(c)
			if unicode.IsSpace(c) || isWordBreak(c) {
				atWordStart = true
			}
			continue
		}
		emit(string(c), mapped)
		atWordStart = false
	}
	return b.String()
}

// CyrillicToLatin transliterates Uzbek Cyrillic text to Latin.
func CyrillicToLatin(s string) string {
	r := []rune(s)
	var b strings.Builder
	atWordStart := true

	emit := func(src rune, dst string) {
		b.WriteString(keepCase(string(src), dst))
	}

	for i := 0; i < len(r); i++ {
		c := r[i]
		lc := unicode.ToLower(c)

		var mapped string
		switch lc {
		case 'а':
			mapped = "a"
		case 'б':
			mapped = "b"
		case 'в':
			mapped = "v"
		case 'г':
			mapped = "g"
		case 'ғ':
			mapped = "g" + string(turnedComma)
		case 'д':
			mapped = "d"
		case 'е':
			if atWordStart {
				mapped = "ye"
			} else {
				mapped = "e"
			}
		case 'ё':
			mapped = "yo"
		case 'ж':
			mapped = "j"
		case 'з':
			mapped = "z"
		case 'и':
			mapped = "i"
		case 'й':
			mapped = "y"
		case 'к':
			mapped = "k"
		case 'қ':
			mapped = "q"
		case 'л':
			mapped = "l"
		case 'м':
			mapped = "m"
		case 'н':
			mapped = "n"
		case 'о':
			mapped = "o"
		case 'п':
			mapped = "p"
		case 'р':
			mapped = "r"
		case 'с':
			mapped = "s"
		case 'т':
			mapped = "t"
		case 'у':
			mapped = "u"
		case 'ў':
			mapped = "o" + string(turnedComma)
		case 'ф':
			mapped = "f"
		case 'х':
			mapped = "x"
		case 'ҳ':
			mapped = "h"
		case 'ц':
			mapped = "ts"
		case 'ч':
			mapped = "ch"
		case 'ш':
			mapped = "sh"
		case 'щ':
			mapped = "sh"
		case 'ъ':
			mapped = string(modApos)
		case 'ь':
			mapped = "" // soft sign: dropped in Uzbek Latin
		case 'э':
			mapped = "e"
		case 'ю':
			mapped = "yu"
		case 'я':
			mapped = "ya"
		default:
			b.WriteRune(c)
			if unicode.IsSpace(c) || isWordBreak(c) {
				atWordStart = true
			}
			continue
		}

		if mapped != "" {
			// keep case for multi-rune outputs too (Sh, SH, sh)
			if len(mapped) > 1 {
				b.WriteString(caseFold(c, i, r, mapped))
			} else {
				emit(c, mapped)
			}
		}
		atWordStart = false
	}
	return b.String()
}

// caseFold applies the case of a Cyrillic source rune (with lookahead for
// ALL-CAPS runs) to a multi-letter Latin output.
func caseFold(src rune, i int, all []rune, dst string) string {
	if !unicode.IsUpper(src) {
		return dst
	}
	next := i + 1
	if next < len(all) && unicode.IsUpper(all[next]) {
		return strings.ToUpper(dst)
	}
	// also treat a preceding upper as part of a caps run
	if i > 0 && unicode.IsUpper(all[i-1]) {
		return strings.ToUpper(dst)
	}
	dr := []rune(dst)
	dr[0] = unicode.ToUpper(dr[0])
	return string(dr)
}

func isWordBreak(r rune) bool {
	switch r {
	case '-', '—', '.', ',', ';', ':', '!', '?', '"', '(', ')', '«', '»', '\n', '\t', '/':
		return true
	}
	return false
}
