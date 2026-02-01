#include "AnnotationListActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "fontIds.h"

namespace {
constexpr int kStartY = 60;
constexpr int kLineHeight = 30;
constexpr int kSkipPageMs = 700;
constexpr int kDeleteLongPressMs = 700;
}  // namespace

void AnnotationListActivity::taskTrampoline(void* param) {
  auto* self = static_cast<AnnotationListActivity*>(param);
  self->displayTaskLoop();
}

void AnnotationListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  refreshItems();
  updateRequired = true;
  xTaskCreate(&AnnotationListActivity::taskTrampoline, "AnnotationListTask", 4096, this, 1, &displayTaskHandle);
}

void AnnotationListActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void AnnotationListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (deleteConfirmActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteConfirmActive = false;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (deleteConfirmSelection == 0 && pendingDeleteIndex >= 0) {
        const size_t idx = static_cast<size_t>(pendingDeleteIndex);
        if (mode == Mode::Bookmarks) {
          store.removeBookmark(items[idx].storeIndex);
        } else {
          store.removeHighlight(items[idx].storeIndex);
        }
        refreshItems();
      }
      deleteConfirmActive = false;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      deleteConfirmSelection = 1 - deleteConfirmSelection;
      updateRequired = true;
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kDeleteLongPressMs &&
      !deleteLongPressHandled) {
    if (!items.empty()) {
      deleteConfirmActive = true;
      deleteConfirmSelection = 0;
      pendingDeleteIndex = selectedIndex;
      deleteLongPressHandled = true;
      updateRequired = true;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (deleteLongPressHandled) {
      deleteLongPressHandled = false;
      return;
    }
    if (!items.empty()) {
      onSelect(items[selectedIndex].start);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (items.empty() || (!prevReleased && !nextReleased)) {
    return;
  }

  const bool skipPage = mappedInput.getHeldTime() > kSkipPageMs;
  const int pageItems = getPageItems();
  const int totalItems = static_cast<int>(items.size());

  if (prevReleased) {
    if (skipPage) {
      selectedIndex = ((selectedIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectedIndex = (selectedIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectedIndex = ((selectedIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectedIndex = (selectedIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

[[noreturn]] void AnnotationListActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

int AnnotationListActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - kLineHeight;
  const int availableHeight = endY - kStartY;
  int itemsPerPage = availableHeight / kLineHeight;
  if (itemsPerPage < 1) {
    itemsPerPage = 1;
  }
  return itemsPerPage;
}

std::string AnnotationListActivity::buildLabel(const ListItem& item) const {
  std::string title = "Unknown";
  if (epub) {
    const int tocIndex = epub->getTocIndexForSpineIndex(item.start.spineIndex);
    if (tocIndex != -1) {
      title = epub->getTocItem(tocIndex).title;
    }
  }

  std::string location;
  if (item.pageCountHint > 0) {
    location = std::to_string(item.pageIndexHint + 1) + "/" + std::to_string(item.pageCountHint);
  } else {
    location = "Loc " + std::to_string(item.start.wordIndex);
  }

  return title + " - " + location;
}

void AnnotationListActivity::refreshItems() {
  items.clear();
  if (mode == Mode::Bookmarks) {
    const auto& bookmarks = store.getBookmarks();
    items.reserve(bookmarks.size());
    for (size_t i = 0; i < bookmarks.size(); i++) {
      const auto& bookmark = bookmarks[i];
      ListItem item{};
      item.start = {bookmark.spineIndex, bookmark.wordIndex};
      item.end = item.start;
      item.pageIndexHint = bookmark.pageIndexHint;
      item.pageCountHint = bookmark.pageCountHint;
      item.storeIndex = i;
      items.push_back(std::move(item));
    }
  } else {
    const auto& highlights = store.getHighlights();
    items.reserve(highlights.size());
    for (size_t i = 0; i < highlights.size(); i++) {
      const auto& highlight = highlights[i];
      ListItem item{};
      item.start = {highlight.spineIndex, highlight.startWordIndex};
      item.end = {highlight.spineIndex, highlight.endWordIndex};
      item.pageIndexHint = highlight.pageIndexHint;
      item.pageCountHint = highlight.pageCountHint;
      item.storeIndex = i;
      items.push_back(std::move(item));
    }
  }

  std::sort(items.begin(), items.end(), [](const ListItem& a, const ListItem& b) {
    if (a.start.spineIndex != b.start.spineIndex) {
      return a.start.spineIndex < b.start.spineIndex;
    }
    if (a.start.wordIndex != b.start.wordIndex) {
      return a.start.wordIndex < b.start.wordIndex;
    }
    return a.end.wordIndex < b.end.wordIndex;
  });

  for (auto& item : items) {
    item.label = buildLabel(item);
  }

  if (selectedIndex >= static_cast<int>(items.size())) {
    selectedIndex = items.empty() ? 0 : static_cast<int>(items.size() - 1);
  }
}

void AnnotationListActivity::renderScreen() {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int pageItems = getPageItems();

  const char* title = mode == Mode::Bookmarks ? "Bookmarks" : "Highlights";
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);

  if (items.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, mode == Mode::Bookmarks ? "No bookmarks" : "No highlights");
  } else {
    const int totalItems = static_cast<int>(items.size());
    const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
    renderer.fillRect(0, kStartY + (selectedIndex % pageItems) * kLineHeight - 2, pageWidth - 1, kLineHeight);

    for (int i = 0; i < pageItems; i++) {
      const int itemIndex = pageStartIndex + i;
      if (itemIndex >= totalItems) break;
      const int displayY = kStartY + i * kLineHeight;
      const bool isSelected = itemIndex == selectedIndex;

      const std::string truncated =
          renderer.truncatedText(UI_10_FONT_ID, items[itemIndex].label.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, displayY, truncated.c_str(), !isSelected);
    }
  }

  if (!items.empty()) {
    const char* deleteHint = "Hold OK: Delete";
    const int hintWidth = renderer.getTextWidth(SMALL_FONT_ID, deleteHint);
    renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - hintWidth, pageHeight - 60, deleteHint);
  }

  const auto labels = mappedInput.mapLabels("« Back", "Open", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (deleteConfirmActive) {
    const char* prompt = "Delete item?";
    const int promptWidth = renderer.getTextWidth(UI_12_FONT_ID, prompt, EpdFontFamily::BOLD);
    const int padding = 16;
    const int boxWidth = promptWidth + padding * 2;
    const int boxHeight = 70;
    const int boxX = (pageWidth - boxWidth) / 2;
    const int boxY = (pageHeight - boxHeight) / 2;

    renderer.fillRect(boxX - 2, boxY - 2, boxWidth + 4, boxHeight + 4, true);
    renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);
    renderer.drawText(UI_12_FONT_ID, boxX + padding, boxY + 6, prompt, true, EpdFontFamily::BOLD);

    const int optionY = boxY + 36;
    const char* deleteLabel = "Delete";
    const char* cancelLabel = "Cancel";
    const int deleteWidth = renderer.getTextWidth(UI_10_FONT_ID, deleteLabel);
    const int cancelWidth = renderer.getTextWidth(UI_10_FONT_ID, cancelLabel);
    const int gap = 16;
    const int totalWidth = deleteWidth + cancelWidth + gap;
    const int startX = boxX + (boxWidth - totalWidth) / 2;

    if (deleteConfirmSelection == 0) {
      renderer.fillRect(startX - 4, optionY - 2, deleteWidth + 8, 18, true);
    } else {
      renderer.fillRect(startX + deleteWidth + gap - 4, optionY - 2, cancelWidth + 8, 18, true);
    }
    renderer.drawText(UI_10_FONT_ID, startX, optionY, deleteLabel, deleteConfirmSelection != 0);
    renderer.drawText(UI_10_FONT_ID, startX + deleteWidth + gap, optionY, cancelLabel, deleteConfirmSelection == 0);
  }

  renderer.displayBuffer();
}
