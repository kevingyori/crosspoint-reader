#include <Arduino.h>
#include <unity.h>

#include <Serialization.h>

#include <sstream>

#include "ReaderAnnotations.h"

namespace {
constexpr uint32_t kMagic = 0x4E415043;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRecordSize = 16;

void writeHeader(std::ostream& out, uint32_t count) {
  serialization::writePod(out, kMagic);
  serialization::writePod(out, kVersion);
  serialization::writePod(out, kRecordSize);
  serialization::writePod(out, static_cast<uint16_t>(0));
  serialization::writePod(out, count);
}

void writeBookmark(std::ostream& out, uint16_t spine, uint32_t word, uint16_t pageIndex, uint16_t pageCount) {
  serialization::writePod(out, static_cast<uint8_t>(0));
  serialization::writePod(out, static_cast<uint8_t>(0));
  serialization::writePod(out, spine);
  serialization::writePod(out, word);
  serialization::writePod(out, word);
  serialization::writePod(out, pageIndex);
  serialization::writePod(out, pageCount);
}

void writeHighlight(std::ostream& out, uint16_t spine, uint32_t start, uint32_t end, uint16_t pageIndex,
                    uint16_t pageCount) {
  serialization::writePod(out, static_cast<uint8_t>(1));
  serialization::writePod(out, static_cast<uint8_t>(0));
  serialization::writePod(out, spine);
  serialization::writePod(out, start);
  serialization::writePod(out, end);
  serialization::writePod(out, pageIndex);
  serialization::writePod(out, pageCount);
}
}  // namespace

void test_ranges_overlap() {
  TEST_ASSERT_TRUE(rangesOverlap(10, 20, 15, 30));
  TEST_ASSERT_TRUE(rangesOverlap(20, 10, 15, 18));
  TEST_ASSERT_FALSE(rangesOverlap(1, 5, 6, 9));
}

void test_normalize_range() {
  TextRange range{{2, 50}, {2, 10}};
  auto normalized = normalizeRange(range);
  TEST_ASSERT_EQUAL_UINT16(2, normalized.start.spineIndex);
  TEST_ASSERT_EQUAL_UINT32(10, normalized.start.wordIndex);
  TEST_ASSERT_EQUAL_UINT32(50, normalized.end.wordIndex);
}

void test_annotations_roundtrip() {
  std::stringstream input;
  writeHeader(input, 2);
  writeBookmark(input, 1, 100, 4, 20);
  writeHighlight(input, 1, 200, 210, 5, 20);

  AnnotationStore store;
  TEST_ASSERT_TRUE(store.loadFromStream(input));
  TEST_ASSERT_EQUAL(1, store.getBookmarks().size());
  TEST_ASSERT_EQUAL(1, store.getHighlights().size());
  TEST_ASSERT_EQUAL_UINT16(1, store.getBookmarks()[0].spineIndex);
  TEST_ASSERT_EQUAL_UINT32(100, store.getBookmarks()[0].wordIndex);
  TEST_ASSERT_EQUAL_UINT32(200, store.getHighlights()[0].startWordIndex);
  TEST_ASSERT_EQUAL_UINT32(210, store.getHighlights()[0].endWordIndex);

  std::stringstream output;
  TEST_ASSERT_TRUE(store.saveToStream(output));
  TEST_ASSERT_EQUAL_STRING(input.str().c_str(), output.str().c_str());
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ranges_overlap);
  RUN_TEST(test_normalize_range);
  RUN_TEST(test_annotations_roundtrip);
  UNITY_END();
}

void loop() {}
