#include <catch_amalgamated.hpp>

#include "../src/text/split.h"
#include "../src/text/translit.h"

using namespace sb::text;

TEST_CASE("SplitSentences", "[text]") {
  struct Case {
    std::string name, in;
    std::vector<std::string> want;
  };
  std::vector<Case> cases = {
      {"empty", "", {}},
      {"whitespace only", "   \n\t ", {}},
      {"single no terminator", "hello world", {"hello world"}},
      {"single with period", "Hello world.", {"Hello world."}},
      {"two english", "Hello world. How are you?", {"Hello world.", "How are you?"}},
      {"question then exclaim", "Really?! I can't believe it.", {"Really?!", "I can't believe it."}},
      {"ellipsis kept, then split", "Well... I guess so. Fine.", {"Well... I guess so.", "Fine."}},
      {"decimal not split", "Pi is 3.14 today. Yes.", {"Pi is 3.14 today.", "Yes."}},
      {"grouped number", "It costs 1.000 soʻm. Cheap.", {"It costs 1.000 soʻm.", "Cheap."}},
      {"english abbreviation", "Mr. Smith went home. He slept.", {"Mr. Smith went home.", "He slept."}},
      {"initials not split", "J. R. R. Tolkien wrote books. Many books.",
       {"J. R. R. Tolkien wrote books.", "Many books."}},
      {"russian i t.d.", "Купили хлеб, молоко и т.д. Потом ушли домой.",
       {"Купили хлеб, молоко и т.д.", "Потом ушли домой."}},
      {"russian two sentences", "Привет, как дела? Всё хорошо!", {"Привет, как дела?", "Всё хорошо!"}},
      {"uzbek latin", "Bugun havo issiq. Ertaga yomgʻir yogʻadi.",
       {"Bugun havo issiq.", "Ertaga yomgʻir yogʻadi."}},
      {"uzbek cyrillic", "Бугун ҳаво иссиқ. Эртага ёмғир ёғади.",
       {"Бугун ҳаво иссиқ.", "Эртага ёмғир ёғади."}},
      {"no split lowercase after dot (domain-ish)", "visit example.com now",
       {"visit example.com now"}},
      {"trailing fragment kept", "First sentence. And a fragment",
       {"First sentence.", "And a fragment"}},
      {"quote closes sentence", "He said \"go home.\" Then he left.",
       {"He said \"go home.\"", "Then he left."}},
      {"newline separated", "Line one.\nLine two.", {"Line one.", "Line two."}},
      {"multiple spaces after terminator", "Done.    Next up.", {"Done.", "Next up."}},
      {"number starts next sentence", "Count them. 5 apples remain.",
       {"Count them.", "5 apples remain."}},
  };
  for (auto &c : cases) {
    INFO("case: " << c.name << " input: " << c.in);
    CHECK(SplitSentences(c.in) == c.want);
  }
}

TEST_CASE("SplitSentences reassembly — no fabricated content", "[text]") {
  std::string in = "Salom! Qalaysiz? Men yaxshi... rahmat. Т.е. всё хорошо.";
  for (auto &s : SplitSentences(in)) {
    CHECK(in.find(s) != std::string::npos);
  }
}

TEST_CASE("LatinToCyrillic", "[translit]") {
  struct Case {
    std::string in, want;
  };
  std::vector<Case> cases = {
      {"salom", "салом"},
      {"Salom", "Салом"},
      {"SALOM", "САЛОМ"},
      {"oʻzbek", "ўзбек"},
      {"o'zbek", "ўзбек"},
      {"gʻalaba", "ғалаба"},
      {"shahar", "шаҳар"},
      {"choy", "чой"},
      {"singil", "сингил"},
      {"tong", "тонг"},
      {"maʼno", "маъно"},
      {"ma'no", "маъно"},
      {"yer", "ер"},
      {"yomgʻir", "ёмғир"},
      {"yulduz", "юлдуз"},
      {"yaxshi", "яхши"},
      {"Toshkent", "Тошкент"},
      {"echki", "эчки"},
      {"men", "мен"},
      {"universitet", "университет"},
  };
  for (auto &c : cases) {
    INFO("LatinToCyrillic(" << c.in << ")");
    CHECK(LatinToCyrillic(c.in) == c.want);
  }
}

TEST_CASE("CyrillicToLatin", "[translit]") {
  struct Case {
    std::string in, want;
  };
  std::vector<Case> cases = {
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
  };
  for (auto &c : cases) {
    INFO("CyrillicToLatin(" << c.in << ")");
    CHECK(CyrillicToLatin(c.in) == c.want);
  }
}

TEST_CASE("Round-trip Latin -> Cyrillic -> Latin", "[translit]") {
  std::vector<std::string> words = {
      "oʻzbekiston", "toshkent", "samarqand", "buxoro", "gʻijduvon",
      "shahar", "choyxona", "yer", "yomgʻir", "yulduz", "yaxshi",
      "maʼno", "singil", "tong", "kitob", "maktab", "universitet",
      "salom", "rahmat", "xayr",
  };
  for (auto &w : words) {
    std::string rt = CyrillicToLatin(LatinToCyrillic(w));
    CHECK(rt == w);
  }
}

TEST_CASE("Translit preserves non-letters", "[translit]") {
  std::string in = "Salom, dunyo! 123 — oʻq.";
  std::string want = "Салом, дунё! 123 — ўқ.";
  CHECK(LatinToCyrillic(in) == want);
}
