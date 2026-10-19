// Plan N1 Task 3: tokens (grammar.md §2), MINUS vs NEG, numbers in milli, errors 7015 / 7016 / 7010.
#include <string.h>
#include <unity.h>

#include "logo/lexer.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
void L(const char* s) { g_d.clear(); lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d); }
int K(Tok k) { return static_cast<int>(k); }
}  // namespace

void setUp() {}
void tearDown() {}

void test_lex_words_numbers_and_end() {
  L("FD 50 rt 2.5");
  TEST_ASSERT_EQUAL(5, g_t.n);
  TEST_ASSERT_EQUAL(K(Tok::kName), K(g_t.t[0].kind));
  TEST_ASSERT_EQUAL(50000, g_t.t[1].milli);
  TEST_ASSERT_TRUE(tokenIs("FD 50 rt 2.5", g_t.t[2], "RT"));
  TEST_ASSERT_EQUAL(2500, g_t.t[3].milli);
  TEST_ASSERT_EQUAL(K(Tok::kEnd), K(g_t.t[4].kind));
}

void test_lex_minus_and_neg() {
  L("10-5 10 - 5 10 -5 (-5 [-5 -:x");
  const Tok want[] = {Tok::kNumber, Tok::kMinus, Tok::kNumber, Tok::kNumber, Tok::kMinus, Tok::kNumber,
                      Tok::kNumber, Tok::kNeg,   Tok::kNumber, Tok::kLParen, Tok::kNeg,   Tok::kNumber,
                      Tok::kLBracket, Tok::kNeg, Tok::kNumber, Tok::kNeg,    Tok::kInput, Tok::kEnd};
  TEST_ASSERT_EQUAL(18, g_t.n);
  for (uint8_t i = 0; i < 18; ++i) TEST_ASSERT_EQUAL_MESSAGE(K(want[i]), K(g_t.t[i].kind), "token kind");
}

void test_lex_lines_columns_comments() {
  L("; hello\n  FD 50 ; one side\nRT 90");
  TEST_ASSERT_EQUAL(2, g_t.t[0].line);
  TEST_ASSERT_EQUAL(3, g_t.t[0].col);
  TEST_ASSERT_EQUAL(3, g_t.t[2].line);
  TEST_ASSERT_EQUAL(1, g_t.t[2].col);
}

void test_lex_names_inputs_quoted_placeholders_operators() {
  L("OBSTACLE? C#5 :size \"Done! ? ?1 ?2 <= >= <> = < > * /");
  const Tok want[] = {Tok::kName, Tok::kName, Tok::kInput, Tok::kQuoted, Tok::kPlaceholder, Tok::kPlaceholder,
                      Tok::kPlaceholder, Tok::kLe, Tok::kGe, Tok::kNe, Tok::kEq, Tok::kLt, Tok::kGt,
                      Tok::kStar, Tok::kSlash, Tok::kEnd};
  for (uint8_t i = 0; i < 16; ++i) TEST_ASSERT_EQUAL(K(want[i]), K(g_t.t[i].kind));
  TEST_ASSERT_EQUAL(2, g_t.t[6].ph);
  char buf[40];
  tokenText("OBSTACLE? C#5 :size \"Done! ?", g_t.t[3], buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("\"Done!", buf);
}

void test_lex_quoted_ends_at_bracket() {
  L("\"ball[ BEEP ]");
  TEST_ASSERT_EQUAL(K(Tok::kQuoted), K(g_t.t[0].kind));
  TEST_ASSERT_EQUAL(5, g_t.t[0].len);
  TEST_ASSERT_EQUAL(K(Tok::kLBracket), K(g_t.t[1].kind));
}

void test_lex_errors_keep_going() {
  L("FD 50 @ FD 10000000 FD 1.2345");
  TEST_ASSERT_EQUAL(7016, g_d.at(0).code);
  TEST_ASSERT_EQUAL(7, g_d.at(0).col);
  TEST_ASSERT_EQUAL(7015, g_d.at(1).code);  // too big: still a NUMBER token (clamped) so the parser goes on
  TEST_ASSERT_EQUAL(K(Tok::kNumber), K(g_t.t[3].kind));
  TEST_ASSERT_EQUAL(7015, g_d.at(2).code);  // 4 decimals
}

void test_lex_huge_number_is_7015_and_clamped() {  // 40 digits: no 64-bit overflow
  L("FD 1234567890123456789012345678901234567890");
  TEST_ASSERT_EQUAL(7015, g_d.at(0).code);
  TEST_ASSERT_EQUAL(1000000000, g_t.t[1].milli);
}

void test_lex_token_overflow() {
  static char big[9000];
  for (uint16_t i = 0; i < 8400; i += 2) { big[i] = 'A'; big[i + 1] = ' '; }  // 4200 words > 4096 tokens
  big[8400] = '\0';
  L(big);
  TEST_ASSERT_EQUAL(TokenList::kCap, g_t.n);
  TEST_ASSERT_EQUAL(K(Tok::kEnd), K(g_t.t[g_t.n - 1].kind));
  TEST_ASSERT_EQUAL(7010, g_d.at(0).code);
}

void test_lex_utf8_comment_and_quoted() {
  L("; 🐢 turtle\nSAY \"héllo FD 5");
  TEST_ASSERT_EQUAL(0, g_d.count());
  TEST_ASSERT_EQUAL(K(Tok::kQuoted), K(g_t.t[1].kind));
  TEST_ASSERT_EQUAL(K(Tok::kName), K(g_t.t[2].kind));
  TEST_ASSERT_EQUAL(2, g_t.t[2].line);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lex_words_numbers_and_end);
  RUN_TEST(test_lex_minus_and_neg);
  RUN_TEST(test_lex_lines_columns_comments);
  RUN_TEST(test_lex_names_inputs_quoted_placeholders_operators);
  RUN_TEST(test_lex_quoted_ends_at_bracket);
  RUN_TEST(test_lex_errors_keep_going);
  RUN_TEST(test_lex_huge_number_is_7015_and_clamped);
  RUN_TEST(test_lex_token_overflow);
  RUN_TEST(test_lex_utf8_comment_and_quoted);
  return UNITY_END();
}
