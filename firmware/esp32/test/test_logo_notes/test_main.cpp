// Plan N2 Task 1: note names, lengths and the 64-note limit (words.yaml presets.notes).
#include <string.h>
#include <unity.h>

#include "logo/notes.h"

using namespace cq::logo;

namespace {
uint16_t hz(const char* s) {
  uint16_t h = 9999;
  return noteHz(s, static_cast<uint16_t>(strlen(s)), h) ? h : 9999;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_note_frequencies() {
  TEST_ASSERT_EQUAL(262, hz("C4"));
  TEST_ASSERT_EQUAL(440, hz("a4"));
  TEST_ASSERT_EQUAL(277, hz("C#4"));
  TEST_ASSERT_EQUAL(1976, hz("B6"));
  TEST_ASSERT_EQUAL(523, hz("C5"));
  TEST_ASSERT_EQUAL(0, hz("R"));
}

void test_not_notes() {
  TEST_ASSERT_EQUAL(9999, hz("C3"));
  TEST_ASSERT_EQUAL(9999, hz("C7"));
  TEST_ASSERT_EQUAL(9999, hz("H4"));
  TEST_ASSERT_EQUAL(9999, hz("B#4"));
  TEST_ASSERT_EQUAL(9999, hz("Cb4"));
  TEST_ASSERT_EQUAL(9999, hz("RR"));
  TEST_ASSERT_EQUAL(9999, hz("C"));
}

void test_reader_lengths() {
  NoteReader r;
  TEST_ASSERT_EQUAL(NoteReader::Err::kNone, r.word("C4", 2));
  TEST_ASSERT_EQUAL(NoteReader::Err::kNone, r.number(2000));
  TEST_ASSERT_EQUAL(NoteReader::Err::kNotANote, r.number(1000));
  TEST_ASSERT_EQUAL(NoteReader::Err::kNone, r.word("R", 1));
  TEST_ASSERT_EQUAL(NoteReader::Err::kLength, r.number(17000));
  TEST_ASSERT_EQUAL(2, r.count());
  TEST_ASSERT_EQUAL(500, r.notes()[0].ms);
  TEST_ASSERT_EQUAL(250, r.notes()[1].ms);
  TEST_ASSERT_EQUAL(0, r.notes()[1].hz);
}

void test_reader_64_notes_at_most() {
  NoteReader r;
  for (int i = 0; i < 64; ++i) TEST_ASSERT_EQUAL(NoteReader::Err::kNone, r.word("C4", 2));
  TEST_ASSERT_EQUAL(NoteReader::Err::kTooMany, r.word("C4", 2));
  TEST_ASSERT_EQUAL(NoteReader::Err::kNotANote, NoteReader().number(1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_note_frequencies);
  RUN_TEST(test_not_notes);
  RUN_TEST(test_reader_lengths);
  RUN_TEST(test_reader_64_notes_at_most);
  return UNITY_END();
}
