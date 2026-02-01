#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

class FsFile;

struct TextLocation {
  uint16_t spineIndex = 0;
  uint32_t wordIndex = 0;
};

struct TextRange {
  TextLocation start;
  TextLocation end;
};

struct HighlightRange {
  uint32_t start = 0;
  uint32_t end = 0;
};

struct Bookmark {
  uint16_t spineIndex = 0;
  uint32_t wordIndex = 0;
  uint16_t pageIndexHint = 0;
  uint16_t pageCountHint = 0;
};

struct Highlight {
  uint16_t spineIndex = 0;
  uint32_t startWordIndex = 0;
  uint32_t endWordIndex = 0;
  uint16_t pageIndexHint = 0;
  uint16_t pageCountHint = 0;
};

bool rangesOverlap(uint32_t startA, uint32_t endA, uint32_t startB, uint32_t endB);
TextRange normalizeRange(const TextRange& range);

class AnnotationStore {
 public:
  enum class RecordType : uint8_t { Bookmark = 0, Highlight = 1 };

  bool load(const std::string& cachePath);
  bool save() const;
  void clear();
  void setCachePath(const std::string& cachePath);

  const std::vector<Bookmark>& getBookmarks() const;
  const std::vector<Highlight>& getHighlights() const;
  std::vector<HighlightRange> getHighlightRangesForSpine(uint16_t spineIndex) const;
  bool hasBookmarkInRange(uint16_t spineIndex, uint32_t startWordIndex, uint32_t endWordIndex) const;

  bool addBookmark(uint16_t spineIndex, uint32_t wordIndex, uint16_t pageIndexHint, uint16_t pageCountHint);
  bool addHighlight(uint16_t spineIndex, uint32_t startWordIndex, uint32_t endWordIndex, uint16_t pageIndexHint,
                    uint16_t pageCountHint);
  bool removeBookmark(size_t index);
  bool removeHighlight(size_t index);

  bool loadFromStream(std::istream& input);
  bool saveToStream(std::ostream& output) const;

 private:
  std::string filePath;
  std::vector<Bookmark> bookmarks;
  std::vector<Highlight> highlights;

  bool loadFromFile(FsFile& input);
  bool saveToFile(FsFile& output) const;
  void sortHighlights();
};
