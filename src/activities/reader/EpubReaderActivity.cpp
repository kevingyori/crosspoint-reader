#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "AnnotationListActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "fontIds.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long annotateLongPressMs = 700;
constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;

}  // namespace

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();
  annotations.load(epub->getCachePath());

  FsFile f;
  if (SdMan.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      Serial.printf("[%lu] [ERS] Opened for first time, navigating to text reference at index %d\n", millis(),
                    textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor());

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  section.reset();
  epub.reset();
  annotations.clear();
}

void EpubReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (!toastMessage.empty() && toastEndMs > 0 && millis() > toastEndMs) {
    toastMessage.clear();
    toastEndMs = 0;
    updateRequired = true;
  }

  if (annotateMenuActive) {
    const auto annotationButton = getAnnotationShortcutButton();
    if (annotationLongPressHandled && mappedInput.wasReleased(annotationButton)) {
      annotationLongPressHandled = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      annotateMenuActive = false;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (annotateMenuIndex == 0) {
        addBookmark();
      } else if (annotateMenuIndex == 1) {
        startSelectionMode();
      }
      annotateMenuActive = false;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      annotateMenuIndex = (annotateMenuIndex + 2) % 3;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      annotateMenuIndex = (annotateMenuIndex + 1) % 3;
      updateRequired = true;
    }
    return;
  }

  if (highlightConfirmActive) {
    if (highlightConfirmIgnoreRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      highlightConfirmIgnoreRelease = false;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelSelectionMode();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (highlightConfirmIndex == 0) {
        saveHighlight();
      } else {
        cancelSelectionMode();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      highlightConfirmIndex = 1 - highlightConfirmIndex;
      updateRequired = true;
    }
    return;
  }

  if (selectionState.active) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelSelectionMode();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!selectionState.startSet) {
        updateSelectionWithCursor();
        selectionState.startSet = true;
        updateRequired = true;
      } else {
        confirmSelection();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      moveCursorLeft();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      moveCursorRight();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      moveCursorUp();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      moveCursorDown();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      moveCursorLeft();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      moveCursorRight();
      return;
    }
    return;
  }

  const bool annotationEnabled =
      SETTINGS.annotationShortcut != CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_DISABLED;
  const auto annotationButton = getAnnotationShortcutButton();
  bool ignoreAnnotationRelease = false;
  if (annotationEnabled && annotationLongPressHandled && mappedInput.wasReleased(annotationButton)) {
    ignoreAnnotationRelease = true;
    annotationLongPressHandled = false;
  }

  if (annotationEnabled && mappedInput.isPressed(annotationButton) && mappedInput.getHeldTime() >= annotateLongPressMs &&
      !annotationLongPressHandled) {
    annotationLongPressHandled = true;
    openAnnotateMenu();
    return;
  }

  // Long press BACK (1s+) goes directly to home
  if (!ignoreAnnotationRelease && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  // Short press BACK goes to file selection
  if (!ignoreAnnotationRelease && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < goHomeMs) {
    onGoBack();
    return;
  }

  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (shouldSuppressConfirmRelease()) {
      return;
    }
    // Don't start activity transition while rendering
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new EpubReaderMenuActivity(
        this->renderer, this->mappedInput, epub->getTitle(), [this]() { onReaderMenuBack(); },
        [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
    xSemaphoreGive(renderingMutex);
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                              mappedInput.wasPressed(MappedInputManager::Button::Left))
                                           : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                              mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  bool nextTriggered = usePressForPageTurn
                           ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                              mappedInput.wasPressed(MappedInputManager::Button::Right))
                           : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (ignoreAnnotationRelease && annotationButton == MappedInputManager::Button::Left) {
    prevTriggered = false;
  }
  if (ignoreAnnotationRelease && annotationButton == MappedInputManager::Button::Right) {
    nextTriggered = false;
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // any botton press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    updateRequired = true;
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    nextPageNumber = 0;
    currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
    section.reset();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    updateRequired = true;
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = UINT16_MAX;
      currentSpineIndex--;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      xSemaphoreGive(renderingMutex);
    }
    updateRequired = true;
  }
}

void EpubReaderActivity::onReaderMenuBack() {
  exitActivity();
  updateRequired = true;
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Calculate values BEFORE we start destroying things
      const int currentP = section ? section->currentPage : 0;
      const int totalP = section ? section->pageCount : 0;
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();

      xSemaphoreTake(renderingMutex, portMAX_DELAY);

      // 1. Close the menu
      exitActivity();

      // 2. Open the Chapter Selector
      enterNewActivity(new EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentP, totalP,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex) {
            if (currentSpineIndex != newSpineIndex) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              section.reset();
            }
            exitActivity();
            updateRequired = true;
          }));

      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SHOW_BOOKMARKS:
    case EpubReaderMenuActivity::MenuAction::SHOW_HIGHLIGHTS: {
      const AnnotationListActivity::Mode mode = action == EpubReaderMenuActivity::MenuAction::SHOW_BOOKMARKS
                                                    ? AnnotationListActivity::Mode::Bookmarks
                                                    : AnnotationListActivity::Mode::Highlights;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new AnnotationListActivity(
          this->renderer, this->mappedInput, epub, annotations, mode,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const TextLocation& location) {
            const int targetPage = findPageForWordIndex(location.spineIndex, location.wordIndex);
            if (currentSpineIndex != static_cast<int>(location.spineIndex)) {
              currentSpineIndex = location.spineIndex;
              section.reset();
            }
            nextPageNumber = targetPage;
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      // 2. Trigger the reader's "Go Home" callback
      if (onGoHome) {
        onGoHome();
      }

      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (epub) {
        // 2. BACKUP: Read current progress
        // We use the current variables that track our position
        uint16_t backupSpine = currentSpineIndex;
        uint16_t backupPage = section->currentPage;
        uint16_t backupPageCount = section->pageCount;

        section.reset();
        // 3. WIPE: Clear the cache directory
        epub->clearCache();

        // 4. RESTORE: Re-setup the directory and rewrite the progress file
        epub->setupCacheDir();

        saveProgress(backupSpine, backupPage, backupPageCount);
      }
      exitActivity();
      updateRequired = true;
      xSemaphoreGive(renderingMutex);
      if (onGoHome) onGoHome();
      break;
    }
  }
}

void EpubReaderActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// TODO: Failure handling
void EpubReaderActivity::renderScreen() {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  // Add status bar margin
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    // Add additional margin for status bar if progress bar is shown
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - SETTINGS.screenMargin +
                            (showProgressBar ? (ScreenComponents::BOOK_PROGRESS_BAR_HEIGHT + progressBarMarginTop) : 0);
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    Serial.printf("[%lu] [ERS] Loading file: %s, index: %d\n", millis(), filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled)) {
      Serial.printf("[%lu] [ERS] Cache not found, building...\n", millis());

      const auto popupFn = [this]() { ScreenComponents::drawPopup(renderer, "Indexing..."); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, popupFn)) {
        Serial.printf("[%lu] [ERS] Failed to persist page data to SD\n", millis());
        section.reset();
        return;
      }
    } else {
      Serial.printf("[%lu] [ERS] Cache found, skipping build...\n", millis());
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    Serial.printf("[%lu] [ERS] No pages to render\n", millis());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Empty chapter", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    Serial.printf("[%lu] [ERS] Page out of bounds: %d (max %d)\n", millis(), section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Out of bounds", true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      Serial.printf("[%lu] [ERS] Failed to load page from SD - clearing section cache\n", millis());
      section->clearCache();
      section.reset();
      return renderScreen();
    }
    const auto start = millis();
    renderContents(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    Serial.printf("[%lu] [ERS] Rendered page in %dms\n", millis(), millis() - start);
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (SdMan.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    Serial.printf("[ERS] Progress saved: Chapter %d, Page %d\n", spineIndex, currentPage);
  } else {
    Serial.printf("[ERS] Could not save progress!\n");
  }
}
void EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop, const int orientedMarginRight,
                                        const int orientedMarginBottom, const int orientedMarginLeft) {
  updatePageWordInfo(page, orientedMarginLeft, orientedMarginTop);
  uint32_t pageMin = 0;
  uint32_t pageMax = 0;
  pageBookmarked = getPageWordRange(pageMin, pageMax) &&
                   annotations.hasBookmarkInRange(static_cast<uint16_t>(currentSpineIndex), pageMin, pageMax);

  drawHighlights();
  page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  if (selectionState.active) {
    drawSelectionCursor();
  }
  if (annotateMenuActive) {
    drawAnnotateMenu();
  }
  if (highlightConfirmActive) {
    drawHighlightConfirmMenu();
  }
  if (!toastMessage.empty()) {
    drawToast();
  }

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  // determine visible status bar elements
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;
  int annotateHintWidth = 0;
  const bool showAnnotateHint = shouldShowAnnotateHint();
  const char* annotateHint = showAnnotateHint ? annotationHintText() : "";

  // Calculate progress in book
  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Hide percentage when progress bar is shown to reduce clutter
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showProgressBar) {
    // Draw progress bar at the very bottom of the screen, from edge to edge of viewable area
    ScreenComponents::drawBookProgressBar(renderer, static_cast<size_t>(bookProgress));
  }

  if (showAnnotateHint && annotateHint && annotateHint[0] != '\0') {
    annotateHintWidth = renderer.getTextWidth(SMALL_FONT_ID, annotateHint);
    int hintX = renderer.getScreenWidth() - orientedMarginRight - annotateHintWidth;
    if (showProgressText || showProgressPercentage) {
      hintX -= progressTextWidth + 10;
    }
    renderer.drawText(SMALL_FONT_ID, hintX, textY, annotateHint);
  }

  if (showBattery) {
    ScreenComponents::drawBattery(renderer, orientedMarginLeft + 1, textY, showBatteryPercentage);
  }

  if (pageBookmarked) {
    const int iconSize = 6;
    const int iconX = renderer.getScreenWidth() - orientedMarginRight - iconSize;
    const int iconY = textY - iconSize - 2;
    renderer.fillRect(iconX, iconY, iconSize, iconSize, true);
  }

  if (showChapterTitle) {
    // Centered chatper title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + annotateHintWidth + (showAnnotateHint ? 40 : 30);

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (tocIndex == -1) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        // Not enough space to center on the screen, center it within the remaining space instead
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      while (titleWidth > availableTitleSpace && title.length() > 11) {
        title.replace(title.length() - 8, 8, "...");
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}

void EpubReaderActivity::openAnnotateMenu() {
  annotateMenuActive = true;
  annotateMenuIndex = 0;
  updateRequired = true;
}

void EpubReaderActivity::startSelectionMode() {
  selectionState = SelectionState{};
  selectionState.active = true;
  selectionState.pendingMoveToStart = true;
  highlightConfirmActive = false;
  highlightConfirmIndex = 0;
  highlightConfirmIgnoreRelease = false;
  updateRequired = true;
}

void EpubReaderActivity::cancelSelectionMode() {
  selectionState = SelectionState{};
  highlightConfirmActive = false;
  highlightConfirmIndex = 0;
  highlightConfirmIgnoreRelease = false;
  updateRequired = true;
}

void EpubReaderActivity::confirmSelection() {
  updateSelectionWithCursor();
  highlightConfirmActive = true;
  highlightConfirmIndex = 0;
  highlightConfirmIgnoreRelease = true;
  updateRequired = true;
}

void EpubReaderActivity::saveHighlight() {
  if (!selectionState.startSet) {
    cancelSelectionMode();
    return;
  }
  uint32_t start = selectionState.startWordIndex;
  uint32_t end = selectionState.endWordIndex;
  if (start > end) {
    std::swap(start, end);
  }
  const uint16_t pageIndex = section ? static_cast<uint16_t>(section->currentPage) : 0;
  const uint16_t pageCount = section ? static_cast<uint16_t>(section->pageCount) : 0;
  const bool saved =
      annotations.addHighlight(static_cast<uint16_t>(currentSpineIndex), start, end, pageIndex, pageCount);
  toastMessage = saved ? "Highlight saved" : "Highlight exists";
  toastEndMs = millis() + 1500;
  cancelSelectionMode();
}

void EpubReaderActivity::addBookmark() {
  uint32_t pageMin = 0;
  uint32_t pageMax = 0;
  if (!getPageWordRange(pageMin, pageMax)) {
    return;
  }
  const uint16_t pageIndex = section ? static_cast<uint16_t>(section->currentPage) : 0;
  const uint16_t pageCount = section ? static_cast<uint16_t>(section->pageCount) : 0;
  const bool saved =
      annotations.addBookmark(static_cast<uint16_t>(currentSpineIndex), pageMin, pageIndex, pageCount);
  toastMessage = saved ? "Bookmark added" : "Bookmark exists";
  toastEndMs = millis() + 1200;
  updateRequired = true;
}

void EpubReaderActivity::updateSelectionWithCursor() {
  WordPosition word{};
  if (!findWordForCursor(currentPageWordInfo, selectionState.cursorLine, selectionState.cursorWord, word)) {
    return;
  }
  selectionState.endWordIndex = word.wordIndex;
  if (!selectionState.startSet) {
    selectionState.startWordIndex = word.wordIndex;
  }
}

void EpubReaderActivity::moveCursorLeft() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  if (selectionState.cursorLine < 0) selectionState.cursorLine = 0;
  if (selectionState.cursorLine >= static_cast<int>(currentPageWordInfo.lines.size())) {
    selectionState.cursorLine = static_cast<int>(currentPageWordInfo.lines.size() - 1);
  }
  auto& line = currentPageWordInfo.lines[selectionState.cursorLine];
  if (line.words.empty()) {
    return;
  }
  if (selectionState.cursorWord > 0) {
    selectionState.cursorWord--;
  } else if (selectionState.cursorLine > 0) {
    selectionState.cursorLine--;
    auto& prevLine = currentPageWordInfo.lines[selectionState.cursorLine];
    selectionState.cursorWord = prevLine.words.empty() ? 0 : static_cast<int>(prevLine.words.size() - 1);
  } else {
    if (section && section->currentPage > 0) {
      section->currentPage--;
      selectionState.pendingMoveToEnd = true;
      updateRequired = true;
      return;
    }
  }
  updateSelectionWithCursor();
  updateRequired = true;
}

void EpubReaderActivity::moveCursorRight() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  if (selectionState.cursorLine < 0) selectionState.cursorLine = 0;
  if (selectionState.cursorLine >= static_cast<int>(currentPageWordInfo.lines.size())) {
    selectionState.cursorLine = static_cast<int>(currentPageWordInfo.lines.size() - 1);
  }
  auto& line = currentPageWordInfo.lines[selectionState.cursorLine];
  if (line.words.empty()) {
    return;
  }
  if (selectionState.cursorWord + 1 < static_cast<int>(line.words.size())) {
    selectionState.cursorWord++;
  } else if (selectionState.cursorLine + 1 < static_cast<int>(currentPageWordInfo.lines.size())) {
    selectionState.cursorLine++;
    auto& nextLine = currentPageWordInfo.lines[selectionState.cursorLine];
    selectionState.cursorWord = 0;
    if (nextLine.words.empty()) {
      selectionState.cursorWord = 0;
    }
  } else {
    if (section && section->currentPage + 1 < section->pageCount) {
      section->currentPage++;
      selectionState.pendingMoveToStart = true;
      updateRequired = true;
      return;
    }
  }
  updateSelectionWithCursor();
  updateRequired = true;
}

void EpubReaderActivity::moveCursorUp() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  WordPosition current{};
  if (!findWordForCursor(currentPageWordInfo, selectionState.cursorLine, selectionState.cursorWord, current)) {
    return;
  }
  if (selectionState.cursorLine == 0) {
    if (section && section->currentPage > 0) {
      section->currentPage--;
      selectionState.pendingMoveToEnd = true;
      updateRequired = true;
    }
    return;
  }
  const int targetX = current.x + current.width / 2;
  const int newLineIndex = selectionState.cursorLine - 1;
  const auto& line = currentPageWordInfo.lines[newLineIndex];
  int bestIndex = 0;
  int bestDistance = INT_MAX;
  for (size_t i = 0; i < line.words.size(); i++) {
    const int center = line.words[i].x + line.words[i].width / 2;
    const int distance = std::abs(center - targetX);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }
  selectionState.cursorLine = newLineIndex;
  selectionState.cursorWord = bestIndex;
  updateSelectionWithCursor();
  updateRequired = true;
}

void EpubReaderActivity::moveCursorDown() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  WordPosition current{};
  if (!findWordForCursor(currentPageWordInfo, selectionState.cursorLine, selectionState.cursorWord, current)) {
    return;
  }
  if (selectionState.cursorLine + 1 >= static_cast<int>(currentPageWordInfo.lines.size())) {
    if (section && section->currentPage + 1 < section->pageCount) {
      section->currentPage++;
      selectionState.pendingMoveToStart = true;
      updateRequired = true;
    }
    return;
  }
  const int targetX = current.x + current.width / 2;
  const int newLineIndex = selectionState.cursorLine + 1;
  const auto& line = currentPageWordInfo.lines[newLineIndex];
  int bestIndex = 0;
  int bestDistance = INT_MAX;
  for (size_t i = 0; i < line.words.size(); i++) {
    const int center = line.words[i].x + line.words[i].width / 2;
    const int distance = std::abs(center - targetX);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }
  selectionState.cursorLine = newLineIndex;
  selectionState.cursorWord = bestIndex;
  updateSelectionWithCursor();
  updateRequired = true;
}

void EpubReaderActivity::moveCursorToPageStart() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  selectionState.cursorLine = 0;
  selectionState.cursorWord = 0;
  updateSelectionWithCursor();
}

void EpubReaderActivity::moveCursorToPageEnd() {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  selectionState.cursorLine = static_cast<int>(currentPageWordInfo.lines.size() - 1);
  const auto& line = currentPageWordInfo.lines[selectionState.cursorLine];
  selectionState.cursorWord = line.words.empty() ? 0 : static_cast<int>(line.words.size() - 1);
  updateSelectionWithCursor();
}

void EpubReaderActivity::updatePageWordInfo(const Page& page, const int xOffset, const int yOffset) {
  PageWordInfo info{};
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  for (const auto& element : page.elements) {
    auto* pageLine = dynamic_cast<PageLine*>(element.get());
    if (!pageLine) {
      continue;
    }
    const auto& block = pageLine->getBlock();
    const auto& words = block.getWords();
    const auto& xpos = block.getWordXpos();
    const auto& styles = block.getWordStyles();
    const auto& indices = block.getWordIndices();
    if (words.empty()) {
      continue;
    }

    LineWords lineInfo{};
    lineInfo.y = yOffset + pageLine->yPos;
    lineInfo.height = lineHeight;

    auto wordIt = words.begin();
    auto xposIt = xpos.begin();
    auto styleIt = styles.begin();
    auto indexIt = indices.begin();
    for (; wordIt != words.end() && xposIt != xpos.end() && styleIt != styles.end() && indexIt != indices.end();
         ++wordIt, ++xposIt, ++styleIt, ++indexIt) {
      WordPosition pos{};
      pos.wordIndex = *indexIt;
      pos.x = xOffset + pageLine->xPos + *xposIt;
      pos.y = lineInfo.y;
      pos.width = renderer.getTextWidth(SETTINGS.getReaderFontId(), wordIt->c_str(), *styleIt);
      pos.height = lineHeight;
      lineInfo.words.push_back(pos);

      if (!info.hasWords) {
        info.minWordIndex = pos.wordIndex;
        info.maxWordIndex = pos.wordIndex;
        info.hasWords = true;
      } else {
        info.minWordIndex = std::min(info.minWordIndex, pos.wordIndex);
        info.maxWordIndex = std::max(info.maxWordIndex, pos.wordIndex);
      }
    }
    if (!lineInfo.words.empty()) {
      info.lines.push_back(std::move(lineInfo));
    }
  }

  currentPageWordInfo = std::move(info);

  if (selectionState.active) {
    if (selectionState.pendingMoveToStart) {
      selectionState.pendingMoveToStart = false;
      moveCursorToPageStart();
    }
    if (selectionState.pendingMoveToEnd) {
      selectionState.pendingMoveToEnd = false;
      moveCursorToPageEnd();
    }
  }
}

void EpubReaderActivity::drawHighlights() const {
  if (!currentPageWordInfo.hasWords) {
    return;
  }
  const auto ranges = annotations.getHighlightRangesForSpine(static_cast<uint16_t>(currentSpineIndex));
  size_t rangeIndex = 0;

  bool hasSelection = selectionState.active && selectionState.startSet;
  HighlightRange selectionRange{};
  if (hasSelection) {
    selectionRange.start = selectionState.startWordIndex;
    selectionRange.end = selectionState.endWordIndex;
    if (selectionRange.start > selectionRange.end) {
      std::swap(selectionRange.start, selectionRange.end);
    }
  }

  auto drawDither = [this](const WordPosition& word, const int rowStep) {
    if (word.width <= 0 || word.height <= 0) {
      return;
    }
    int row = 0;
    for (int y = word.y; y < word.y + word.height; y += rowStep) {
      const int offset = (row % 2);
      for (int x = word.x + offset; x < word.x + word.width; x += 2) {
        renderer.drawPixel(x, y, true);
      }
      row++;
    }
  };

  for (const auto& line : currentPageWordInfo.lines) {
    for (const auto& word : line.words) {
      while (rangeIndex < ranges.size() && word.wordIndex > ranges[rangeIndex].end) {
        rangeIndex++;
      }
      bool inSaved = rangeIndex < ranges.size() && word.wordIndex >= ranges[rangeIndex].start &&
                     word.wordIndex <= ranges[rangeIndex].end;
      bool inSelection = hasSelection && word.wordIndex >= selectionRange.start &&
                         word.wordIndex <= selectionRange.end;
      if (inSelection) {
        drawDither(word, 2);
      } else if (inSaved) {
        drawDither(word, 3);
      }
    }
  }
}

void EpubReaderActivity::drawSelectionCursor() const {
  WordPosition word{};
  if (!findWordForCursor(currentPageWordInfo, selectionState.cursorLine, selectionState.cursorWord, word)) {
    return;
  }
  renderer.drawRect(word.x - 1, word.y - 1, word.width + 2, word.height + 2, true);
}

void EpubReaderActivity::drawAnnotateMenu() const {
  const char* items[] = {"Add bookmark", "Highlight text", "Cancel"};
  const int itemCount = 3;
  int maxWidth = 0;
  for (const auto* item : items) {
    maxWidth = std::max(maxWidth, renderer.getTextWidth(UI_10_FONT_ID, item));
  }

  const int padding = 14;
  const int lineHeight = 20;
  const int boxWidth = maxWidth + padding * 2;
  const int boxHeight = padding * 2 + itemCount * lineHeight;
  const int boxX = (renderer.getScreenWidth() - boxWidth) / 2;
  const int boxY = (renderer.getScreenHeight() - boxHeight) / 2;

  renderer.fillRect(boxX - 2, boxY - 2, boxWidth + 4, boxHeight + 4, true);
  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);

  for (int i = 0; i < itemCount; i++) {
    const int textY = boxY + padding + i * lineHeight;
    const bool selected = annotateMenuIndex == i;
    if (selected) {
      renderer.fillRect(boxX + 4, textY - 2, boxWidth - 8, lineHeight, true);
    }
    renderer.drawText(UI_10_FONT_ID, boxX + padding, textY, items[i], !selected);
  }
}

void EpubReaderActivity::drawHighlightConfirmMenu() const {
  const char* items[] = {"Save highlight", "Cancel"};
  const int itemCount = 2;
  int maxWidth = 0;
  for (const auto* item : items) {
    maxWidth = std::max(maxWidth, renderer.getTextWidth(UI_10_FONT_ID, item));
  }

  const int padding = 14;
  const int lineHeight = 20;
  const int boxWidth = maxWidth + padding * 2;
  const int boxHeight = padding * 2 + itemCount * lineHeight;
  const int boxX = (renderer.getScreenWidth() - boxWidth) / 2;
  const int boxY = (renderer.getScreenHeight() - boxHeight) / 2;

  renderer.fillRect(boxX - 2, boxY - 2, boxWidth + 4, boxHeight + 4, true);
  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);

  for (int i = 0; i < itemCount; i++) {
    const int textY = boxY + padding + i * lineHeight;
    const bool selected = highlightConfirmIndex == i;
    if (selected) {
      renderer.fillRect(boxX + 4, textY - 2, boxWidth - 8, lineHeight, true);
    }
    renderer.drawText(UI_10_FONT_ID, boxX + padding, textY, items[i], !selected);
  }
}

void EpubReaderActivity::drawToast() const {
  const int padding = 14;
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, toastMessage.c_str(), EpdFontFamily::BOLD);
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int boxWidth = textWidth + padding * 2;
  const int boxHeight = textHeight + padding * 2;
  const int boxX = (renderer.getScreenWidth() - boxWidth) / 2;
  const int boxY = 60;

  renderer.fillRect(boxX - 2, boxY - 2, boxWidth + 4, boxHeight + 4, true);
  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);
  renderer.drawText(UI_10_FONT_ID, boxX + padding, boxY + padding, toastMessage.c_str(), true, EpdFontFamily::BOLD);
}

bool EpubReaderActivity::shouldShowAnnotateHint() const {
  return SETTINGS.annotationShortcut != CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_DISABLED &&
         SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE;
}

const char* EpubReaderActivity::annotationHintText() const {
  static const char* hints[] = {"Hold OK", "Hold Back", "Hold Left", "Hold Right", ""};
  const auto shortcut =
      static_cast<CrossPointSettings::ANNOTATION_SHORTCUT>(SETTINGS.annotationShortcut);
  if (shortcut >= CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATION_SHORTCUT_COUNT) {
    return "";
  }
  return hints[shortcut];
}

MappedInputManager::Button EpubReaderActivity::getAnnotationShortcutButton() const {
  switch (static_cast<CrossPointSettings::ANNOTATION_SHORTCUT>(SETTINGS.annotationShortcut)) {
    case CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_BACK_LONG_PRESS:
      return MappedInputManager::Button::Back;
    case CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_LEFT_LONG_PRESS:
      return MappedInputManager::Button::Left;
    case CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_RIGHT_LONG_PRESS:
      return MappedInputManager::Button::Right;
    case CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_OK_LONG_PRESS:
    default:
      return MappedInputManager::Button::Confirm;
  }
}

bool EpubReaderActivity::shouldSuppressConfirmRelease() const {
  return SETTINGS.annotationShortcut == CrossPointSettings::ANNOTATION_SHORTCUT::ANNOTATE_OK_LONG_PRESS &&
         (annotationLongPressHandled || mappedInput.getHeldTime() >= annotateLongPressMs);
}

bool EpubReaderActivity::findWordForCursor(const PageWordInfo& info, const int lineIndex, const int wordIndex,
                                           WordPosition& out) const {
  if (!info.hasWords || lineIndex < 0 || wordIndex < 0) {
    return false;
  }
  if (lineIndex >= static_cast<int>(info.lines.size())) {
    return false;
  }
  const auto& line = info.lines[lineIndex];
  if (wordIndex >= static_cast<int>(line.words.size())) {
    return false;
  }
  out = line.words[wordIndex];
  return true;
}

bool EpubReaderActivity::getPageWordRange(uint32_t& outMin, uint32_t& outMax) const {
  if (!currentPageWordInfo.hasWords) {
    return false;
  }
  outMin = currentPageWordInfo.minWordIndex;
  outMax = currentPageWordInfo.maxWordIndex;
  return true;
}

int EpubReaderActivity::findPageForWordIndex(const uint16_t spineIndex, const uint32_t wordIndex) const {
  if (!epub) {
    return 0;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL_WITH_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - SETTINGS.screenMargin +
                            (showProgressBar ? (ScreenComponents::BOOK_PROGRESS_BAR_HEIGHT + progressBarMarginTop) : 0);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  Section tempSection(epub, spineIndex, renderer);
  if (!tempSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                   SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                   viewportHeight, SETTINGS.hyphenationEnabled)) {
    const auto popupFn = [this]() { ScreenComponents::drawPopup(renderer, "Indexing..."); };
    if (!tempSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                       SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                       viewportHeight, SETTINGS.hyphenationEnabled, popupFn)) {
      return 0;
    }
  }

  int fallbackPage = 0;
  for (int pageIndex = 0; pageIndex < tempSection.pageCount; pageIndex++) {
    tempSection.currentPage = pageIndex;
    auto page = tempSection.loadPageFromSectionFile();
    if (!page) {
      continue;
    }
    bool hasWords = false;
    uint32_t minWord = 0;
    uint32_t maxWord = 0;
    for (const auto& element : page->elements) {
      auto* pageLine = dynamic_cast<PageLine*>(element.get());
      if (!pageLine) {
        continue;
      }
      const auto& indices = pageLine->getBlock().getWordIndices();
      for (const auto& idx : indices) {
        if (!hasWords) {
          minWord = idx;
          maxWord = idx;
          hasWords = true;
        } else {
          minWord = std::min(minWord, idx);
          maxWord = std::max(maxWord, idx);
        }
      }
    }
    if (!hasWords) {
      continue;
    }
    if (wordIndex < minWord) {
      return fallbackPage;
    }
    if (wordIndex >= minWord && wordIndex <= maxWord) {
      return pageIndex;
    }
    fallbackPage = pageIndex;
  }
  return fallbackPage;
}
