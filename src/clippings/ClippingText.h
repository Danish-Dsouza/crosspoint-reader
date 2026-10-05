#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace clippingText {
inline bool hasVisibleText(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; ++p) {
    if (*p > ' ') return true;
  }
  return false;
}

inline bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<uint8_t>(text[0]) == 0xE2 && static_cast<uint8_t>(text[1]) == 0x80 &&
         static_cast<uint8_t>(text[2]) == 0x83;
}

// Rendered discretionary hyphens have no source codepoint; literal hyphens do.
inline bool append(std::string& text, std::string_view word, const uint32_t start, const uint32_t end,
                   const char separator, const size_t limit) {
  if (start != UINT32_MAX && end != UINT32_MAX && end >= start && !word.empty() && word.back() == '-') {
    uint32_t codepoints = 0;
    for (const unsigned char c : word) {
      if ((c & 0xc0) != 0x80) ++codepoints;
    }
    if (codepoints > end - start) word.remove_suffix(1);
  }
  const bool addSeparator = !text.empty() && separator != '\0';
  if (text.size() + word.size() + addSeparator > limit) return false;
  if (addSeparator) text.push_back(separator);
  text.append(word);
  return true;
}
}  // namespace clippingText
