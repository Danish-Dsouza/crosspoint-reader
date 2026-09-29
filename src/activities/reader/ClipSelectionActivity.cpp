#include "ClipSelectionActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/SelectionGeometry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace {

constexpr size_t FONT_PREWARM_TEXT_MAX = 2048;
constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;
constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr unsigned long TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;

bool hasVisibleText(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; ++p) {
    if (*p > ' ') return true;
  }
  return false;
}

bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<uint8_t>(text[0]) == 0xE2 && static_cast<uint8_t>(text[1]) == 0x80 &&
         static_cast<uint8_t>(text[2]) == 0x83;
}

const char* cleanWordStart(const char* text) {
  if (!text) return "";
  if (hasEmSpacePrefix(text)) text += 3;
  while (*text != '\0' && (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t' ||
                           (static_cast<uint8_t>(text[0]) == 0xC2 && static_cast<uint8_t>(text[1]) == 0xA0))) {
    text += static_cast<uint8_t>(text[0]) == 0xC2 ? 2 : 1;
  }
  return text;
}

size_t utf8SequenceLength(const uint8_t lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 1;
}

void appendCleanWord(std::string& result, const char* text) {
  text = cleanWordStart(text);
  const size_t wordStart = result.size();
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0;) {
    if (*p == '\r' || *p == '\n' || *p == '\t') {
      if (result.size() > wordStart && result.back() != ' ' && result.size() < CLIPPING_TEXT_MAX) {
        result.push_back(' ');
      }
      ++p;
      continue;
    }
    if (*p == 0xC2 && p[1] == 0xA0) {
      if (result.size() > wordStart && result.back() != ' ' && result.size() < CLIPPING_TEXT_MAX) {
        result.push_back(' ');
      }
      p += 2;
      continue;
    }
    const size_t length = utf8SequenceLength(*p);
    bool complete = true;
    for (size_t i = 1; i < length; ++i) {
      if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
        complete = false;
        break;
      }
    }
    const size_t appendLength = complete ? length : 1;
    if (result.size() + appendLength > CLIPPING_TEXT_MAX) break;
    result.append(reinterpret_cast<const char*>(p), appendLength);
    p += appendLength;
  }
  while (result.size() > wordStart && result.back() == ' ') result.pop_back();
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::unique_ptr<Page>> pages, const int marginLeft,
                                             const int marginTop, const int initialX, const int initialY)
    : Activity("ClipSelection", renderer, mappedInput),
      pages(std::move(pages)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (!extractWords() || wordCount == 0) {
    if (wordCount == 0) LOG_ERR("CLIP", "No selectable words on current page");
    cancel();
    return;
  }
  uint16_t firstPageRows = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != 0) break;
    firstPageRows = std::max<uint16_t>(firstPageRows, static_cast<uint16_t>(word.row + 1));
  }
  const int middle = closestInRow(firstPageRows / 2, renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  if (initialX >= 0) {
    const int hit = wordAt(initialX, initialY);
    if (hit < 0) {
      cancel();
      return;
    }
    selected = rangeStart = hit;
    ignoreInitialTouch = true;
  }
  requestUpdate();
}

bool ClipSelectionActivity::extractWords() {
  wordCount = 0;
  words = makeUniqueNoThrow<WordBox[]>(MAX_SELECTABLE_WORDS);
  if (!words) {
    LOG_ERR("CLIP", "OOM: selection words (%u bytes)", static_cast<unsigned>(MAX_SELECTABLE_WORDS * sizeof(WordBox)));
    return false;
  }
  rowCount = 0;
  const bool needsFontPrewarm = renderer.isSdCardFont(fontId);
  auto pageText = needsFontPrewarm ? makeUniqueNoThrow<char[]>(FONT_PREWARM_TEXT_MAX) : nullptr;
  size_t pageTextLength = 0;
  if (needsFontPrewarm && !pageText) LOG_DBG("CLIP", "Skipping SD font prewarm: OOM");
  uint8_t styleMask = 0;

  for (size_t pageOffset = 0; pageOffset < pages.size(); ++pageOffset) {
    uint16_t pageWordIndex = 0;
    for (const auto& element : pages[pageOffset]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || !block->valid()) continue;

      const size_t lineStart = wordCount;
      const bool isRtl = block->getBlockStyle().isRtl;
      const size_t remaining = MAX_SELECTABLE_WORDS - lineStart;
      size_t rtlWordCount = 0;
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        const char* text = block->wordText(i);
        if (!hasVisibleText(text)) continue;

        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        int width = renderer.getTextAdvanceX(fontId, text, style);
        if (width <= 0) continue;
        if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }

        if (!isRtl && wordCount == MAX_SELECTABLE_WORDS) break;

        WordBox& word = isRtl ? words[lineStart + (rtlWordCount < remaining ? rtlWordCount : rtlWordCount % remaining)]
                              : words[wordCount++];
        word.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
        word.y = static_cast<int16_t>(marginTop + line.yPos + rubyShift);
        word.width = static_cast<int16_t>(width);
        word.height = static_cast<int16_t>(lineHeight);
        word.row = rowCount;
        word.pageOffset = static_cast<uint8_t>(pageOffset);
        word.pageWordIndex = pageWordIndex++;
        word.startOffset = block->wordSourceRange(i).start;
        word.endOffset = block->wordSourceRange(i).end;
        word.text = text;
        word.style = style;
        word.paragraphStart = hasEmSpacePrefix(text);
        if (pageText) {
          for (const char* p = text; *p != '\0' && pageTextLength + 1 < FONT_PREWARM_TEXT_MAX; ++p) {
            pageText[pageTextLength++] = *p;
          }
          if (pageTextLength + 1 < FONT_PREWARM_TEXT_MAX) pageText[pageTextLength++] = ' ';
        }
        styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(style) & 0x03));
        if (isRtl) ++rtlWordCount;
      }
      if (isRtl) {
        const size_t stored = std::min(remaining, rtlWordCount);
        wordCount = lineStart + stored;
        if (rtlWordCount > remaining) {
          std::rotate(words.get() + lineStart, words.get() + lineStart + rtlWordCount % remaining,
                      words.get() + wordCount);
        }
        std::reverse(words.get() + lineStart, words.get() + wordCount);
      }
      if (wordCount != lineStart) ++rowCount;
      if (wordCount == MAX_SELECTABLE_WORDS) {
        LOG_ERR("CLIP", "Selectable word cap hit (%u); multi-page selection was truncated",
                static_cast<unsigned>(MAX_SELECTABLE_WORDS));
        break;
      }
    }
    if (wordCount == MAX_SELECTABLE_WORDS) break;
  }

  if (styleMask == 0) styleMask = 0x01;
  if (pageText) {
    pageText[pageTextLength] = '\0';
    renderer.ensureSdCardFontReady(fontId, pageText.get(), styleMask);
  }

  const int indentThreshold = lineHeight / 2;
  int previousRowFirst = -1;
  for (size_t i = 0; i < wordCount; ++i) {
    if (i > 0 && words[i].row == words[i - 1].row) continue;
    if (previousRowFirst >= 0 && words[i].pageOffset == words[previousRowFirst].pageOffset &&
        words[i].x > words[previousRowFirst].x + indentThreshold) {
      words[i].paragraphStart = true;
    }
    previousRowFirst = static_cast<int>(i);
  }
  return true;
}

int ClipSelectionActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

int ClipSelectionActivity::wordAt(const int x, int y) const {
  y -= textOffset();
  constexpr int SLOP = 4;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + word.height + SLOP) {
      return i;
    }
  }
  return -1;
}

int ClipSelectionActivity::dragWordAt(const int x, int y) const {
  y -= textOffset();
  int best = -1;
  Rect nearest{};
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    const Rect candidate{word.x, word.y, word.width, word.height};
    if (best < 0 || selectionGeometry::nearerWord(candidate, nearest, x, y)) {
      nearest = candidate;
      best = i;
    }
  }
  return best;
}

bool ClipSelectionActivity::selectionContains(const int x, const int y) const {
  const int offset = textOffset();
  const WordBox* previous = nullptr;
  for (int i = std::min(rangeStart, selected); i <= std::max(rangeStart, selected); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    int left = word.x;
    int right = word.x + word.width;
    if (previous && previous->row == word.row) {
      left = std::min(left, static_cast<int>(previous->x));
      right = std::max(right, previous->x + previous->width);
    }
    if (selectionGeometry::contains(Rect{left, word.y + offset, right - left, word.height}, x, y)) return true;
    previous = &word;
  }
  return false;
}

int ClipSelectionActivity::nextPageStartIndexForTouchDrag() const {
  if (!touchDragHasMoved || rangeStart < 0 || selected < rangeStart) return -1;

  const uint8_t currentPage = words[selected].pageOffset;
  for (int i = selected + 1; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == currentPage) return -1;
    return words[i].pageOffset > currentPage ? i : -1;
  }
  return -1;
}

bool ClipSelectionActivity::isWithinCurrentPageEndDwellSlop(const int x, const int y) const {
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;
  const WordBox& word = words[selected];
  return word.pageOffset == currentPageOffset && x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         x < word.x + word.width + TOUCH_PAGE_END_DWELL_SLOP_PX && y >= word.y - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y < word.y + word.height + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const int targetRow = static_cast<int>(words[selected].row) + direction;
  if (targetRow < 0 || targetRow >= rowCount) return;
  const int next = closestInRow(static_cast<uint16_t>(targetRow), words[selected].x + words[selected].width / 2);
  if (next >= 0 && next != selected) {
    selectIndex(next);
  }
}

void ClipSelectionActivity::selectIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(wordCount) || index == selected) return;
  selected = index;
  currentPageOffset = words[selected].pageOffset;
  requestUpdate();
}

void ClipSelectionActivity::moveToPage(const int pageOffset) {
  if (pageOffset < 0 || pageOffset >= static_cast<int>(pages.size()) || pageOffset == currentPageOffset) return;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == pageOffset) {
      selectIndex(i);
      return;
    }
  }
}

std::string ClipSelectionActivity::buildSelectedText(const int first, const int last) const {
  std::string text;
  text.reserve(CLIPPING_TEXT_MAX);
  for (int i = first; i <= last; ++i) {
    const char* word = cleanWordStart(words[i].text);
    if (*word == '\0') continue;
    const size_t checkpoint = text.size();
    bool removedHyphen = false;
    if (!text.empty()) {
      const WordBox& previous = words[i - 1];
      if (text.back() == '-' && *word != '-' && std::isalnum(static_cast<unsigned char>(*word))) {
        text.pop_back();
        removedHyphen = true;
      } else if (words[i].paragraphStart) {
        if (text.size() == CLIPPING_TEXT_MAX) break;
        text.push_back('\n');
      } else {
        const bool visuallyAttached =
            words[i].row == previous.row && std::abs(words[i].x - (previous.x + previous.width)) <= 2;
        if (!visuallyAttached) {
          if (text.size() == CLIPPING_TEXT_MAX) break;
          text.push_back(' ');
        }
      }
    }
    const size_t wordStart = text.size();
    appendCleanWord(text, word);
    if (text.size() == wordStart) {
      if (removedHyphen) {
        text.push_back('-');
      } else {
        text.resize(checkpoint);
      }
      break;
    }
    if (text.size() == CLIPPING_TEXT_MAX) break;
  }
  return text;
}

void ClipSelectionActivity::confirmSelection(const ClippingResult::Action action) {
  if (rangeStart < 0) {
    rangeStart = selected;
    requestUpdate();
    return;
  }

  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.action = action;
  result.text = buildSelectedText(first, last);
  result.startPageOffset = words[first].pageOffset;
  result.endPageOffset = words[last].pageOffset;
  result.startWordIndex = words[first].pageWordIndex;
  result.endWordIndex = words[last].pageWordIndex;
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  result.startOffset = UINT32_MAX;
  result.endOffset = 0;
  for (int i = first; i <= last; ++i) {
    if (words[i].startOffset == UINT32_MAX || words[i].endOffset == UINT32_MAX) {
      result.startOffset = result.endOffset = UINT32_MAX;
      break;
    }
    result.startOffset = std::min(result.startOffset, words[i].startOffset);
    result.endOffset = std::max(result.endOffset, words[i].endOffset);
  }
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

void ClipSelectionActivity::loop() {
  RenderLock lock;
  if (wordCount == 0) return;

  int touchX = 0;
  int touchY = 0;
  if (ignoreInitialTouch) {
    if (!mappedInput.isScreenTouchHeld(touchX, touchY)) ignoreInitialTouch = false;
    return;
  }
  if (!touchDragSelecting && mappedInput.wasScreenTapped(touchX, touchY)) {
    if (rangeStart >= 0) {
      const int action =
          selectionGeometry::actionAt(actionRect(), UITheme::getInstance().getMetrics().menuSpacing, touchX, touchY);
      if (action >= 0) {
        static constexpr ClippingResult::Action ACTIONS[] = {
            ClippingResult::Action::Lookup, ClippingResult::Action::Clip, ClippingResult::Action::Bookmark};
        confirmSelection(ACTIONS[action]);
        return;
      }
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      const bool onStart = words[first].pageOffset == currentPageOffset &&
                           selectionGeometry::contains(handleRect(first, true), touchX, touchY);
      const bool onEnd = words[last].pageOffset == currentPageOffset &&
                         selectionGeometry::contains(handleRect(last, false), touchX, touchY);
      if (!onStart && !onEnd && !selectionContains(touchX, touchY)) cancel();
    } else {
      const int hit = wordAt(touchX, touchY);
      if (hit < 0) {
        cancel();
      } else {
        selected = rangeStart = hit;
        requestUpdate();
      }
    }
    return;
  }
  if (touchDragSelecting) {
    if (mappedInput.isScreenTouchHeld(touchX, touchY)) {
      const int deltaX = touchX - touchDragStartX;
      const int deltaY = touchY - touchDragStartY;
      touchDragHasMoved = touchDragHasMoved || deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX ||
                          deltaY >= TOUCH_DRAG_MOVEMENT_PX || deltaY <= -TOUCH_DRAG_MOVEMENT_PX;

      const int hit = dragWordAt(touchX + dragOffsetX, touchY + dragOffsetY);
      if (hit >= 0) {
        const int previousOffset = textOffset();
        selectIndex(hit);
        dragOffsetY += textOffset() - previousOffset;
      }

      // A drag ends normally when released on the final word. Holding there
      // for a moment is the explicit request to carry the range onto the
      // next preloaded page.
      const int nextPageStart = nextPageStartIndexForTouchDrag();
      if (nextPageStart >= 0 &&
          isWithinCurrentPageEndDwellSlop(touchX + dragOffsetX, touchY + dragOffsetY - textOffset())) {
        const unsigned long now = millis();
        if (touchDragPageEndIndex != selected) {
          touchDragPageEndIndex = selected;
          touchDragPageEndHeldSince = now;
        } else if (now - touchDragPageEndHeldSince >= TOUCH_PAGE_ADVANCE_HOLD_MS) {
          touchDragPageEndIndex = -1;
          selectIndex(nextPageStart);
        }
      } else {
        touchDragPageEndIndex = -1;
      }
      return;
    }
    touchDragSelecting = false;
    touchDragHasMoved = false;
    touchDragPageEndIndex = -1;
    requestUpdate();
    return;
  } else if (mappedInput.wasScreenTouchPressed(touchX, touchY)) {
    if (rangeStart >= 0) {
      const Rect actions = actionRect();
      if (selectionGeometry::actionAt(actions, UITheme::getInstance().getMetrics().menuSpacing, touchX, touchY) >= 0)
        return;
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const int index = endpoint == 0 ? first : last;
        if (words[index].pageOffset != currentPageOffset) continue;
        const Rect handle = handleRect(index, endpoint == 0);
        if (touchX < handle.x || touchX >= handle.x + handle.width || touchY < handle.y ||
            touchY >= handle.y + handle.height)
          continue;
        selected = index;
        rangeStart = endpoint == 0 ? last : first;
        dragOffsetX = words[index].x + words[index].width / 2 - touchX;
        dragOffsetY = words[index].y + textOffset() + words[index].height / 2 - touchY;
        touchDragSelecting = true;
        touchDragHasMoved = false;
        touchDragStartX = touchX;
        touchDragStartY = touchY;
        touchDragPageEndIndex = -1;
        return;
      }
      return;
    }
    const int hit = wordAt(touchX, touchY);
    if (hit >= 0) {
      const int previousOffset = textOffset();
      selected = rangeStart = hit;
      dragOffsetX = 0;
      dragOffsetY = textOffset() - previousOffset;
      touchDragSelecting = true;
      touchDragHasMoved = false;
      touchDragStartX = touchX;
      touchDragStartY = touchY;
      touchDragPageEndIndex = -1;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (rangeStart >= 0) {
      rangeStart = -1;
      requestUpdate();
    } else {
      cancel();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmSelection();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    moveToPage(static_cast<int>(currentPageOffset) + 1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    moveToPage(static_cast<int>(currentPageOffset) - 1);
    return;
  }

  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (moveLeft && selected > 0) {
    selectIndex(selected - 1);
    lastHorizontalMoveTime = now;
  } else if (moveRight && selected + 1 < static_cast<int>(wordCount)) {
    selectIndex(selected + 1);
    lastHorizontalMoveTime = now;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

Rect ClipSelectionActivity::handleRect(const int index, const bool start) const {
  const WordBox& word = words[index];
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int size = std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2);
  const int edge = start ? word.x : word.x + word.width;
  return Rect{std::clamp(edge - (start ? size : 0), safe.x, safe.x + safe.width - size),
              std::clamp(word.y + textOffset() + word.height, safe.y, safe.y + safe.height - size), size, size};
}

int ClipSelectionActivity::selectionTop() const {
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  int top = renderer.getScreenHeight();
  for (int i = first; i <= last; ++i) {
    if (words[i].pageOffset == currentPageOffset) top = std::min(top, static_cast<int>(words[i].y));
  }
  return top;
}

Rect ClipSelectionActivity::actionRect() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int font = uiScaleSpec().smallFontId;
  const int labelWidth =
      std::max({renderer.getTextWidth(font, tr(STR_LOOKUP)), renderer.getTextWidth(font, tr(STR_CLIP)),
                renderer.getTextWidth(font, tr(STR_BOOKMARK_OPTION))});
  const int padding = metrics.menuSpacing;
  const int width = std::min(safe.width, 3 * (labelWidth + padding * 2) + padding * 4);
  const int lines = labelWidth > (width - padding * 4) / 3 ? 2 : 1;
  const int height = std::max(36, renderer.getLineHeight(font) * lines + padding * 2);
  int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  while (first < last && words[first].pageOffset < currentPageOffset) ++first;
  return selectionGeometry::actions(safe, selectionTop(), height + padding * 2, metrics.verticalSpacing, width,
                                    words[first].x - padding);
}

int ClipSelectionActivity::textOffset() const {
  if (rangeStart < 0 || !mappedInput.hasTouch()) return 0;
  return selectionGeometry::textOffset(actionRect(), selectionTop(),
                                       UITheme::getInstance().getMetrics().verticalSpacing);
}

void ClipSelectionActivity::drawSelection() const {
  const int offset = textOffset();
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  const WordBox* previous = nullptr;
  for (int i = first; i <= last; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (previous && previous->row == word.row) {
      const int previousRight = previous->x + previous->width;
      const int wordRight = word.x + word.width;
      if (previousRight < word.x) {
        renderer.fillRectDither(previousRight, word.y + offset, word.x - previousRight, word.height, Color::LightGray);
      } else if (wordRight < previous->x) {
        renderer.fillRectDither(wordRight, word.y + offset, previous->x - wordRight, word.height, Color::LightGray);
      }
    }
    renderer.fillRectDither(word.x, word.y + offset, word.width, word.height, Color::LightGray);
    renderer.drawText(fontId, word.x, word.y + offset, word.text, true, word.style);
    previous = &word;
  }
  if (rangeStart >= 0 && mappedInput.hasTouch()) {
    if (words[first].pageOffset == currentPageOffset) GUI.drawSelectionHandle(renderer, handleRect(first, true), true);
    if (words[last].pageOffset == currentPageOffset) GUI.drawSelectionHandle(renderer, handleRect(last, false), false);
    if (!touchDragSelecting) GUI.drawSelectionActions(renderer, actionRect());
  } else {
    const WordBox& cursor = words[selected];
    renderer.drawRect(cursor.x, cursor.y + offset, cursor.width, cursor.height, true);
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  const int offset = textOffset();
  renderer.clearScreen();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft, marginTop + offset);
  scope.endScanAndPrewarm();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft, marginTop + offset);
  if (wordCount != 0) drawSelection();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), rangeStart < 0 ? tr(STR_SELECT) : tr(STR_DONE),
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
