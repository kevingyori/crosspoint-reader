#pragma once

#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "MappedInputManager.h"
#include "ReaderAnnotations.h"

class AnnotationListActivity final : public ActivityWithSubactivity {
 public:
  enum class Mode { Bookmarks, Highlights };

  AnnotationListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                         AnnotationStore& store, Mode mode, const std::function<void()>& onBack,
                         const std::function<void(const TextLocation&)>& onSelect)
      : ActivityWithSubactivity("AnnotationList", renderer, mappedInput),
        epub(std::move(epub)),
        store(store),
        mode(mode),
        onBack(onBack),
        onSelect(onSelect) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct ListItem {
    TextLocation start;
    TextLocation end;
    uint16_t pageIndexHint;
    uint16_t pageCountHint;
    size_t storeIndex;
    std::string label;
  };

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void refreshItems();
  int getPageItems() const;
  std::string buildLabel(const ListItem& item) const;

  std::shared_ptr<Epub> epub;
  AnnotationStore& store;
  Mode mode;
  std::vector<ListItem> items;

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  bool deleteConfirmActive = false;
  int deleteConfirmSelection = 0;
  int pendingDeleteIndex = -1;
  bool deleteLongPressHandled = false;

  const std::function<void()> onBack;
  const std::function<void(const TextLocation&)> onSelect;
};
