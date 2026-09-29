#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

namespace selectionGeometry {
inline Rect actions(const Rect safe, const int textTop, const int height, const int gap, const int preferredWidth = 0,
                    const int anchorX = 0) {
  const int width = preferredWidth > 0 ? std::min(safe.width, preferredWidth) : safe.width;
  return Rect{std::clamp(anchorX, safe.x, safe.x + safe.width - width),
              std::max(safe.y, textTop - height - gap), width, height};
}

inline int textOffset(const Rect actions, const int textTop, const int gap) {
  return std::max(0, actions.y + actions.height + gap - textTop);
}

inline Rect button(const Rect actions, const int index, const int padding) {
  const int width = (actions.width - padding * 4) / 3;
  return Rect{actions.x + padding + index * (width + padding), actions.y + padding, width,
              actions.height - padding * 2};
}

inline bool contains(const Rect rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

inline int actionAt(const Rect actions, const int padding, const int x, const int y) {
  for (int i = 0; i < 3; ++i) {
    if (contains(button(actions, i, padding), x, y)) return i;
  }
  return -1;
}
}  // namespace selectionGeometry
