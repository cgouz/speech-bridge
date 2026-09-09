package text

import (
	"reflect"
	"testing"
)

func TestSplitSentences(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want []string
	}{
		{"empty", "", nil},
		{"whitespace only", "   \n\t ", nil},
		{"single no terminator", "hello world", []string{"hello world"}},
		{"single with period", "Hello world.", []string{"Hello world."}},
		{
			"two english",
			"Hello world. How are you?",
			[]string{"Hello world.", "How are you?"},
		},
		{
			"question then exclaim",
			"Really?! I can't believe it.",
			[]string{"Really?!", "I can't believe it."},
		},
		{
			"ellipsis kept, then split",
			"Well... I guess so. Fine.",
			[]string{"Well... I guess so.", "Fine."},
		},
		{
			"decimal not split",
			"Pi is 3.14 today. Yes.",
			[]string{"Pi is 3.14 today.", "Yes."},
		},
		{
			"grouped number",
			"It costs 1.000 soʻm. Cheap.",
			[]string{"It costs 1.000 soʻm.", "Cheap."},
		},
		{
			"english abbreviation",
			"Mr. Smith went home. He slept.",
			[]string{"Mr. Smith went home.", "He slept."},
		},
		{
			"initials not split",
			"J. R. R. Tolkien wrote books. Many books.",
			[]string{"J. R. R. Tolkien wrote books.", "Many books."},
		},
		{
			"russian i t.d.",
			"Купили хлеб, молоко и т.д. Потом ушли домой.",
			[]string{"Купили хлеб, молоко и т.д.", "Потом ушли домой."},
		},
		{
			"russian two sentences",
			"Привет, как дела? Всё хорошо!",
			[]string{"Привет, как дела?", "Всё хорошо!"},
		},
		{
			"uzbek latin",
			"Bugun havo issiq. Ertaga yomgʻir yogʻadi.",
			[]string{"Bugun havo issiq.", "Ertaga yomgʻir yogʻadi."},
		},
		{
			"uzbek cyrillic",
			"Бугун ҳаво иссиқ. Эртага ёмғир ёғади.",
			[]string{"Бугун ҳаво иссиқ.", "Эртага ёмғир ёғади."},
		},
		{
			"no split lowercase after dot (domain-ish)",
			"visit example.com now",
			[]string{"visit example.com now"},
		},
		{
			"trailing fragment kept",
			"First sentence. And a fragment",
			[]string{"First sentence.", "And a fragment"},
		},
		{
			"quote closes sentence",
			`He said "go home." Then he left.`,
			[]string{`He said "go home."`, "Then he left."},
		},
		{
			"newline separated",
			"Line one.\nLine two.",
			[]string{"Line one.", "Line two."},
		},
		{
			"multiple spaces after terminator",
			"Done.    Next up.",
			[]string{"Done.", "Next up."},
		},
		{
			"number starts next sentence",
			"Count them. 5 apples remain.",
			[]string{"Count them.", "5 apples remain."},
		},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := SplitSentences(c.in)
			if !reflect.DeepEqual(got, c.want) {
				t.Errorf("SplitSentences(%q)\n got  %#v\n want %#v", c.in, got, c.want)
			}
		})
	}
}

func TestSplitSentencesReassembly(t *testing.T) {
	// Every sentence must be a substring of the input (no fabricated content).
	in := "Salom! Qalaysiz? Men yaxshi... rahmat. Т.е. всё хорошо."
	for _, s := range SplitSentences(in) {
		if !contains(in, s) {
			t.Errorf("sentence %q is not a substring of input", s)
		}
	}
}

func contains(hay, needle string) bool {
	return len(needle) > 0 && len(hay) >= len(needle) && indexOf(hay, needle) >= 0
}

func indexOf(hay, needle string) int {
	for i := 0; i+len(needle) <= len(hay); i++ {
		if hay[i:i+len(needle)] == needle {
			return i
		}
	}
	return -1
}
