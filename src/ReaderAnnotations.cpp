#include "ReaderAnnotations.h"

#include <HardwareSerial.h>
#include <SdFat.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>
#include <istream>
#include <ostream>

namespace {
constexpr uint32_t kMagic = 0x4E415043;  // "CPAN"
constexpr uint8_t kVersion = 1;

struct RecordHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t recordSize;
  uint16_t reserved;
  uint32_t count;
};

struct RecordPayload {
  uint8_t type;
  uint8_t flags;
  uint16_t spineIndex;
  uint32_t startWordIndex;
  uint32_t endWordIndex;
  uint16_t pageIndexHint;
  uint16_t pageCountHint;
};

constexpr uint8_t kRecordSize = sizeof(RecordPayload);

bool locationLessOrEqual(const TextLocation& lhs, const TextLocation& rhs) {
  if (lhs.spineIndex != rhs.spineIndex) {
    return lhs.spineIndex < rhs.spineIndex;
  }
  return lhs.wordIndex <= rhs.wordIndex;
}

template <typename Stream>
bool readHeader(Stream& stream, RecordHeader& header) {
  serialization::readPod(stream, header.magic);
  serialization::readPod(stream, header.version);
  serialization::readPod(stream, header.recordSize);
  serialization::readPod(stream, header.reserved);
  serialization::readPod(stream, header.count);
  return true;
}

template <typename Stream>
bool writeHeader(Stream& stream, const RecordHeader& header) {
  serialization::writePod(stream, header.magic);
  serialization::writePod(stream, header.version);
  serialization::writePod(stream, header.recordSize);
  serialization::writePod(stream, header.reserved);
  serialization::writePod(stream, header.count);
  return true;
}

template <typename Stream>
bool readRecord(Stream& stream, RecordPayload& record) {
  serialization::readPod(stream, record.type);
  serialization::readPod(stream, record.flags);
  serialization::readPod(stream, record.spineIndex);
  serialization::readPod(stream, record.startWordIndex);
  serialization::readPod(stream, record.endWordIndex);
  serialization::readPod(stream, record.pageIndexHint);
  serialization::readPod(stream, record.pageCountHint);
  return true;
}

template <typename Stream>
bool writeRecord(Stream& stream, const RecordPayload& record) {
  serialization::writePod(stream, record.type);
  serialization::writePod(stream, record.flags);
  serialization::writePod(stream, record.spineIndex);
  serialization::writePod(stream, record.startWordIndex);
  serialization::writePod(stream, record.endWordIndex);
  serialization::writePod(stream, record.pageIndexHint);
  serialization::writePod(stream, record.pageCountHint);
  return true;
}
}  // namespace

bool rangesOverlap(uint32_t startA, uint32_t endA, uint32_t startB, uint32_t endB) {
  if (startA > endA) std::swap(startA, endA);
  if (startB > endB) std::swap(startB, endB);
  return startA <= endB && startB <= endA;
}

TextRange normalizeRange(const TextRange& range) {
  if (locationLessOrEqual(range.start, range.end)) {
    return range;
  }
  return {range.end, range.start};
}

bool AnnotationStore::load(const std::string& cachePath) {
  setCachePath(cachePath);
  bookmarks.clear();
  highlights.clear();

  FsFile inputFile;
  if (!SdMan.openFileForRead("ANS", filePath, inputFile)) {
    return false;
  }

  const bool ok = loadFromFile(inputFile);
  inputFile.close();
  if (!ok) {
    bookmarks.clear();
    highlights.clear();
  }
  sortHighlights();
  return ok;
}

bool AnnotationStore::save() const {
  if (filePath.empty()) {
    return false;
  }

  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash != std::string::npos) {
    SdMan.mkdir(filePath.substr(0, lastSlash).c_str());
  }

  FsFile outputFile;
  if (!SdMan.openFileForWrite("ANS", filePath, outputFile)) {
    return false;
  }

  const bool ok = saveToFile(outputFile);
  outputFile.close();
  return ok;
}

void AnnotationStore::clear() {
  bookmarks.clear();
  highlights.clear();
}

void AnnotationStore::setCachePath(const std::string& cachePath) {
  filePath = cachePath + "/annotations.bin";
}

const std::vector<Bookmark>& AnnotationStore::getBookmarks() const { return bookmarks; }

const std::vector<Highlight>& AnnotationStore::getHighlights() const { return highlights; }

std::vector<HighlightRange> AnnotationStore::getHighlightRangesForSpine(const uint16_t spineIndex) const {
  std::vector<HighlightRange> ranges;
  for (const auto& highlight : highlights) {
    if (highlight.spineIndex != spineIndex) {
      continue;
    }
    HighlightRange range{highlight.startWordIndex, highlight.endWordIndex};
    if (range.start > range.end) {
      std::swap(range.start, range.end);
    }
    ranges.push_back(range);
  }

  std::sort(ranges.begin(), ranges.end(), [](const HighlightRange& a, const HighlightRange& b) {
    if (a.start != b.start) {
      return a.start < b.start;
    }
    return a.end < b.end;
  });
  return ranges;
}

bool AnnotationStore::hasBookmarkInRange(const uint16_t spineIndex, uint32_t startWordIndex,
                                         uint32_t endWordIndex) const {
  if (startWordIndex > endWordIndex) {
    std::swap(startWordIndex, endWordIndex);
  }
  for (const auto& bookmark : bookmarks) {
    if (bookmark.spineIndex != spineIndex) {
      continue;
    }
    if (bookmark.wordIndex >= startWordIndex && bookmark.wordIndex <= endWordIndex) {
      return true;
    }
  }
  return false;
}

bool AnnotationStore::addBookmark(const uint16_t spineIndex, const uint32_t wordIndex, const uint16_t pageIndexHint,
                                  const uint16_t pageCountHint) {
  const auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& bookmark) {
    return bookmark.spineIndex == spineIndex && bookmark.wordIndex == wordIndex;
  });
  if (it != bookmarks.end()) {
    return false;
  }
  bookmarks.push_back({spineIndex, wordIndex, pageIndexHint, pageCountHint});
  if (save()) {
    return true;
  }
  bookmarks.pop_back();
  return false;
}

bool AnnotationStore::addHighlight(const uint16_t spineIndex, const uint32_t startWordIndex,
                                   const uint32_t endWordIndex, const uint16_t pageIndexHint,
                                   const uint16_t pageCountHint) {
  uint32_t rangeStart = startWordIndex;
  uint32_t rangeEnd = endWordIndex;
  if (rangeStart > rangeEnd) {
    std::swap(rangeStart, rangeEnd);
  }
  const auto it = std::find_if(highlights.begin(), highlights.end(), [&](const Highlight& highlight) {
    return highlight.spineIndex == spineIndex && highlight.startWordIndex == rangeStart &&
           highlight.endWordIndex == rangeEnd;
  });
  if (it != highlights.end()) {
    return false;
  }
  highlights.push_back({spineIndex, rangeStart, rangeEnd, pageIndexHint, pageCountHint});
  sortHighlights();
  if (save()) {
    return true;
  }
  highlights.pop_back();
  sortHighlights();
  return false;
}

bool AnnotationStore::removeBookmark(const size_t index) {
  if (index >= bookmarks.size()) {
    return false;
  }
  const Bookmark removed = bookmarks[index];
  bookmarks.erase(bookmarks.begin() + static_cast<long>(index));
  if (save()) {
    return true;
  }
  bookmarks.insert(bookmarks.begin() + static_cast<long>(index), removed);
  return false;
}

bool AnnotationStore::removeHighlight(const size_t index) {
  if (index >= highlights.size()) {
    return false;
  }
  const Highlight removed = highlights[index];
  highlights.erase(highlights.begin() + static_cast<long>(index));
  sortHighlights();
  if (save()) {
    return true;
  }
  highlights.insert(highlights.begin() + static_cast<long>(index), removed);
  sortHighlights();
  return false;
}

bool AnnotationStore::loadFromStream(std::istream& input) {
  RecordHeader header{};
  if (!readHeader(input, header)) {
    return false;
  }
  if (header.magic != kMagic || header.version != kVersion || header.recordSize != kRecordSize) {
    Serial.printf("[%lu] [ANS] Invalid header (magic=%lu version=%u size=%u)\n", millis(), header.magic,
                  header.version, header.recordSize);
    return false;
  }

  bookmarks.clear();
  highlights.clear();
  for (uint32_t i = 0; i < header.count; i++) {
    RecordPayload record{};
    if (!readRecord(input, record)) {
      return false;
    }
    if (record.type == static_cast<uint8_t>(RecordType::Bookmark)) {
      bookmarks.push_back({record.spineIndex, record.startWordIndex, record.pageIndexHint, record.pageCountHint});
    } else if (record.type == static_cast<uint8_t>(RecordType::Highlight)) {
      uint32_t rangeStart = record.startWordIndex;
      uint32_t rangeEnd = record.endWordIndex;
      if (rangeStart > rangeEnd) {
        std::swap(rangeStart, rangeEnd);
      }
      highlights.push_back({record.spineIndex, rangeStart, rangeEnd, record.pageIndexHint, record.pageCountHint});
    }
  }
  sortHighlights();
  return true;
}

bool AnnotationStore::loadFromFile(FsFile& input) {
  RecordHeader header{};
  if (!readHeader(input, header)) {
    return false;
  }
  if (header.magic != kMagic || header.version != kVersion || header.recordSize != kRecordSize) {
    Serial.printf("[%lu] [ANS] Invalid header (magic=%lu version=%u size=%u)\n", millis(), header.magic,
                  header.version, header.recordSize);
    return false;
  }

  bookmarks.clear();
  highlights.clear();
  for (uint32_t i = 0; i < header.count; i++) {
    RecordPayload record{};
    if (!readRecord(input, record)) {
      return false;
    }
    if (record.type == static_cast<uint8_t>(RecordType::Bookmark)) {
      bookmarks.push_back({record.spineIndex, record.startWordIndex, record.pageIndexHint, record.pageCountHint});
    } else if (record.type == static_cast<uint8_t>(RecordType::Highlight)) {
      uint32_t rangeStart = record.startWordIndex;
      uint32_t rangeEnd = record.endWordIndex;
      if (rangeStart > rangeEnd) {
        std::swap(rangeStart, rangeEnd);
      }
      highlights.push_back({record.spineIndex, rangeStart, rangeEnd, record.pageIndexHint, record.pageCountHint});
    }
  }
  sortHighlights();
  return true;
}

bool AnnotationStore::saveToStream(std::ostream& output) const {
  RecordHeader header{kMagic, kVersion, kRecordSize, 0,
                      static_cast<uint32_t>(bookmarks.size() + highlights.size())};
  if (!writeHeader(output, header)) {
    return false;
  }

  for (const auto& bookmark : bookmarks) {
    RecordPayload record{};
    record.type = static_cast<uint8_t>(RecordType::Bookmark);
    record.flags = 0;
    record.spineIndex = bookmark.spineIndex;
    record.startWordIndex = bookmark.wordIndex;
    record.endWordIndex = bookmark.wordIndex;
    record.pageIndexHint = bookmark.pageIndexHint;
    record.pageCountHint = bookmark.pageCountHint;
    if (!writeRecord(output, record)) {
      return false;
    }
  }

  for (const auto& highlight : highlights) {
    RecordPayload record{};
    record.type = static_cast<uint8_t>(RecordType::Highlight);
    record.flags = 0;
    record.spineIndex = highlight.spineIndex;
    record.startWordIndex = highlight.startWordIndex;
    record.endWordIndex = highlight.endWordIndex;
    record.pageIndexHint = highlight.pageIndexHint;
    record.pageCountHint = highlight.pageCountHint;
    if (!writeRecord(output, record)) {
      return false;
    }
  }

  return true;
}

bool AnnotationStore::saveToFile(FsFile& output) const {
  RecordHeader header{kMagic, kVersion, kRecordSize, 0,
                      static_cast<uint32_t>(bookmarks.size() + highlights.size())};
  if (!writeHeader(output, header)) {
    return false;
  }

  for (const auto& bookmark : bookmarks) {
    RecordPayload record{};
    record.type = static_cast<uint8_t>(RecordType::Bookmark);
    record.flags = 0;
    record.spineIndex = bookmark.spineIndex;
    record.startWordIndex = bookmark.wordIndex;
    record.endWordIndex = bookmark.wordIndex;
    record.pageIndexHint = bookmark.pageIndexHint;
    record.pageCountHint = bookmark.pageCountHint;
    if (!writeRecord(output, record)) {
      return false;
    }
  }

  for (const auto& highlight : highlights) {
    RecordPayload record{};
    record.type = static_cast<uint8_t>(RecordType::Highlight);
    record.flags = 0;
    record.spineIndex = highlight.spineIndex;
    record.startWordIndex = highlight.startWordIndex;
    record.endWordIndex = highlight.endWordIndex;
    record.pageIndexHint = highlight.pageIndexHint;
    record.pageCountHint = highlight.pageCountHint;
    if (!writeRecord(output, record)) {
      return false;
    }
  }

  return true;
}

void AnnotationStore::sortHighlights() {
  std::sort(highlights.begin(), highlights.end(), [](const Highlight& a, const Highlight& b) {
    if (a.spineIndex != b.spineIndex) {
      return a.spineIndex < b.spineIndex;
    }
    if (a.startWordIndex != b.startWordIndex) {
      return a.startWordIndex < b.startWordIndex;
    }
    return a.endWordIndex < b.endWordIndex;
  });
}
