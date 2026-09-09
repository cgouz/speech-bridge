// Package text provides the pure-Go text utilities on the Speech Bridge
// pipeline: a sentence splitter and Uzbek Latin<->Cyrillic transliteration.
// Zero cgo.
package text

import (
	"strings"
	"unicode"
)

// sentenceEnders are the runes that can terminate a sentence.
var sentenceEnders = map[rune]bool{
	'.': true, '!': true, '?': true,
	'…': true, // U+2026 HORIZONTAL ELLIPSIS
	'。': true, // pasted CJK full stop
	'؟': true, // Arabic question mark (kaa/uz texts occasionally)
}

// abbreviations that end in '.' and must NOT trigger a split. Compared
// case-insensitively against the whitespace-delimited token before the dot.
var abbreviations = map[string]bool{
	// English
	"mr": true, "mrs": true, "ms": true, "dr": true, "prof": true, "sr": true, "jr": true,
	"st": true, "vs": true, "etc": true, "no": true, "vol": true, "fig": true, "al": true,
	"inc": true, "ltd": true, "co": true, "e.g": true, "i.e": true,
	// Russian
	"т": true, "д": true, "п": true, "г": true, "гг": true, "др": true, "см": true,
	"стр": true, "рис": true, "табл": true, "им": true, "проф": true, "акад": true,
	"руб": true, "коп": true, "ул": true, "пр": true, "обл": true, "респ": true,
	// Uzbek (Latin + Cyrillic)
	"va": true, "h.k": true, "b": true, "sh": true, "hok": true,
	"ва": true, "ҳ.к": true,
}

// SplitSentences splits text into sentences for the ru/uz/kaa/en pipeline.
//
// Rules:
//   - split after . ! ? … (and runs like "?!", "...") when followed by
//     whitespace or end of input;
//   - never split inside a number ("3.14", "1 000.50");
//   - never split after a known abbreviation ("и т.д.", "Mr.", "va h.k.");
//   - never split after a lone initial ("J. R. R. Tolkien");
//   - collapse the trailing/leading whitespace of each returned sentence.
//
// Whitespace-only input yields nil. A trailing fragment with no terminator is
// returned as its own sentence.
func SplitSentences(text string) []string {
	runes := []rune(text)
	var out []string
	var buf []rune

	flush := func() {
		s := strings.TrimSpace(string(buf))
		if s != "" {
			out = append(out, s)
		}
		buf = buf[:0]
	}

	for i := 0; i < len(runes); i++ {
		r := runes[i]
		buf = append(buf, r)

		if !sentenceEnders[r] {
			continue
		}

		// Consume a run of terminators / closing quotes+brackets so "?!" and
		// "..." and `.")` stay attached to this sentence.
		j := i + 1
		for j < len(runes) && (sentenceEnders[runes[j]] || isClosing(runes[j])) {
			buf = append(buf, runes[j])
			j++
		}

		// What follows the terminator run?
		if j >= len(runes) {
			i = j - 1
			break // trailing terminator: flush happens after the loop
		}
		if !unicode.IsSpace(runes[j]) {
			i = j - 1
			continue // e.g. "3.14", "google.com" — not a boundary
		}

		// A bare ellipsis ("..." or "…") is a pause, not a sentence boundary.
		if (r == '.' && isEllipsisRun(runes, i)) || r == '…' {
			i = j - 1
			continue
		}

		// Only '.' needs the number / abbreviation / initial guards.
		if r == '.' {
			if insideNumber(runes, i) {
				i = j - 1
				continue
			}
			tok := lastToken(buf[:len(buf)-1]) // token before the '.' run
			if abbreviations[strings.ToLower(strings.TrimRight(tok, "."))] {
				i = j - 1
				continue
			}
			if isInitial(tok) {
				i = j - 1
				continue
			}
		}

		// Peek past the whitespace: a boundary needs a "sentence start" or EOS.
		k := j
		for k < len(runes) && unicode.IsSpace(runes[k]) {
			k++
		}
		if k < len(runes) && !sentenceStart(runes[k]) {
			i = j - 1
			continue
		}

		flush()
		i = j - 1
	}
	flush()
	return out
}

func isClosing(r rune) bool {
	switch r {
	case '"', '\'', '»', '”', '’', ')', ']', '}', '«':
		return true
	}
	return false
}

// sentenceStart reports whether r plausibly begins a new sentence.
func sentenceStart(r rune) bool {
	if unicode.IsUpper(r) || unicode.IsDigit(r) {
		return true
	}
	switch r {
	case '"', '\'', '«', '“', '‘', '(', '[', '—', '-', '…', '¿', '№':
		return true
	}
	// Scripts without case (rare here) — treat any letter as a possible start.
	return unicode.IsLetter(r) && !unicode.IsLower(r)
}

// insideNumber reports whether runes[dot] is a decimal/grouping point between
// digits, e.g. the '.' in "3.14" or "1.000".
func insideNumber(runes []rune, dot int) bool {
	var prev, next rune
	for p := dot - 1; p >= 0; p-- {
		if unicode.IsSpace(runes[p]) {
			continue
		}
		prev = runes[p]
		break
	}
	if dot+1 < len(runes) {
		next = runes[dot+1]
	}
	return unicode.IsDigit(prev) && unicode.IsDigit(next)
}

// isEllipsisRun reports whether the '.' at i is part of a "..." run.
func isEllipsisRun(runes []rune, i int) bool {
	n := 0
	for k := i; k < len(runes) && runes[k] == '.'; k++ {
		n++
	}
	for k := i - 1; k >= 0 && runes[k] == '.'; k-- {
		n++
	}
	return n >= 3
}

// lastToken returns the trailing non-space token of buf.
func lastToken(buf []rune) string {
	end := len(buf)
	for end > 0 && unicode.IsSpace(buf[end-1]) {
		end--
	}
	start := end
	for start > 0 && !unicode.IsSpace(buf[start-1]) {
		start--
	}
	return string(buf[start:end])
}

// isInitial reports whether tok is a single-letter initial like "J" or "Ф".
func isInitial(tok string) bool {
	tok = strings.TrimRight(tok, ".")
	r := []rune(tok)
	if len(r) != 1 {
		return false
	}
	return unicode.IsUpper(r[0])
}
