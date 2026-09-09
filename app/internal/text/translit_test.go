package text

import "testing"

func TestLatinToCyrillic(t *testing.T) {
	cases := []struct{ in, want string }{
		{"salom", "салом"},
		{"Salom", "Салом"},
		{"SALOM", "САЛОМ"},
		{"oʻzbek", "ўзбек"},           // oʻ digraph
		{"o'zbek", "ўзбек"},           // ASCII apostrophe accepted
		{"gʻalaba", "ғалаба"},         // gʻ digraph
		{"shahar", "шаҳар"},           // sh
		{"choy", "чой"},               // ch
		{"singil", "сингил"},          // ng -> нг (no special letter)
		{"tong", "тонг"},              // ng at end
		{"maʼno", "маъно"},            // tutuq belgisi -> ъ
		{"ma'no", "маъно"},            // ASCII apostrophe as tutuq belgisi
		{"yer", "ер"},                 // ye -> е
		{"yomgʻir", "ёмғир"},          // yo + gʻ
		{"yulduz", "юлдуз"},           // yu
		{"yaxshi", "яхши"},            // ya
		{"Toshkent", "Тошкент"},       // title case with digraph
		{"echki", "эчки"},             // word-initial e -> э
		{"men", "мен"},                // medial e -> е
		{"universitet", "университет"},
	}
	for _, c := range cases {
		if got := LatinToCyrillic(c.in); got != c.want {
			t.Errorf("LatinToCyrillic(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestCyrillicToLatin(t *testing.T) {
	cases := []struct{ in, want string }{
		{"салом", "salom"},
		{"Салом", "Salom"},
		{"САЛОМ", "SALOM"},
		{"ўзбек", "oʻzbek"},
		{"ғалаба", "gʻalaba"},
		{"шаҳар", "shahar"},
		{"чой", "choy"},
		{"сингил", "singil"},
		{"тонг", "tong"},
		{"маъно", "maʼno"},
		{"ёмғир", "yomgʻir"},
		{"юлдуз", "yulduz"},
		{"яхши", "yaxshi"},
		{"Тошкент", "Toshkent"},
		{"эчки", "echki"},
		{"мен", "men"},
	}
	for _, c := range cases {
		if got := CyrillicToLatin(c.in); got != c.want {
			t.Errorf("CyrillicToLatin(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// Round-trip on a corpus of ordinary words: Latin -> Cyrillic -> Latin must be
// stable for the canonical Latin spellings.
func TestRoundTripLatin(t *testing.T) {
	words := []string{
		"oʻzbekiston", "toshkent", "samarqand", "buxoro", "gʻijduvon",
		"shahar", "choyxona", "yer", "yomgʻir", "yulduz", "yaxshi",
		"maʼno", "singil", "tong", "kitob", "maktab", "universitet",
		"salom", "rahmat", "xayr",
	}
	for _, w := range words {
		rt := CyrillicToLatin(LatinToCyrillic(w))
		if rt != w {
			t.Errorf("round-trip %q -> %q -> %q", w, LatinToCyrillic(w), rt)
		}
	}
}

func TestTranslitPreservesNonLetters(t *testing.T) {
	in := "Salom, dunyo! 123 — oʻq."
	want := "Салом, дунё! 123 — ўқ."
	if got := LatinToCyrillic(in); got != want {
		t.Errorf("LatinToCyrillic(%q) = %q, want %q", in, got, want)
	}
}
