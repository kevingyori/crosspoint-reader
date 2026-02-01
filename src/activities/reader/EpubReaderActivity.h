#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "ReaderAnnotations.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderActivity final : public ActivityWithSubactivity {
  struct WordPosition {
    uint32_t wordIndex = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  struct LineWords {
    int y = 0;
    int height = 0;
    std::vector<WordPosition> words;
  };

  struct PageWordInfo {
    bool hasWords = false;
    uint32_t minWordIndex = 0;
    uint32_t maxWordIndex = 0;
    std::vector<LineWords> lines;
  };

  struct SelectionState {
    bool active = false;
    bool startSet = false;
    uint32_t startWordIndex = 0;
    uint32_t endWordIndex = 0;
    int cursorLine = 0;
    int cursorWord = 0;
    bool pendingMoveToStart = false;
    bool pendingMoveToEnd = false;
  };

  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  bool updateRequired = false;
  bool annotationLongPressHandled = false;
  bool annotateMenuActive = false;
  int annotateMenuIndex = 0;
  bool highlightConfirmActive = false;
  int highlightConfirmIndex = 0;
  bool highlightConfirmIgnoreRelease = false;
  bool pageBookmarked = false;
  AnnotationStore annotations;
  PageWordInfo currentPageWordInfo;
  SelectionState selectionState;
  std::string toastMessage;
  unsigned long toastEndMs = 0;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(const Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  void onReaderMenuBack();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openAnnotateMenu();
  void startSelectionMode();
  void cancelSelectionMode();
  void confirmSelection();
  void saveHighlight();
  void addBookmark();
  void updateSelectionWithCursor();
  void moveCursorLeft();
  void moveCursorRight();
  void moveCursorUp();
  void moveCursorDown();
  void moveCursorToPageStart();
  void moveCursorToPageEnd();
  void updatePageWordInfo(const Page& page, int xOffset, int yOffset);
  void drawHighlights() const;
  void drawSelectionCursor() const;
  void drawAnnotateMenu() const;
  void drawHighlightConfirmMenu() const;
  void drawToast() const;
  bool shouldShowAnnotateHint() const;
  const char* annotationHintText() const;
  MappedInputManager::Button getAnnotationShortcutButton() const;
  bool shouldSuppressConfirmRelease() const;
  bool findWordForCursor(const PageWordInfo& info, int lineIndex, int wordIndex, WordPosition& out) const;
  bool getPageWordRange(uint32_t& outMin, uint32_t& outMax) const;
  int findPageForWordIndex(uint16_t spineIndex, uint32_t wordIndex) const;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
