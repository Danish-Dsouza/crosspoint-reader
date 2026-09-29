#include "ClippingStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <functional>

#include "clippings/ClippingPreview.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t TEXT_OFFSET_VERSION = 2;
constexpr uint8_t LAYOUT_VERSION = 3;
constexpr uint8_t VERSION = 4;
constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr size_t TEXT_COPY_BUFFER_SIZE = 128;
constexpr size_t HEADER_STRING_MAX = CLIPPING_TEXT_MAX;

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(std::hash<std::string>{}(filePath)) +
         ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

bool copyBytes(HalFile& in, HalFile& out, uint16_t length) {
  std::array<uint8_t, TEXT_COPY_BUFFER_SIZE> buffer{};
  while (length > 0) {
    const size_t chunk = std::min<size_t>(length, buffer.size());
    if (in.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      return false;
    }
    if (out.write(buffer.data(), chunk) != chunk) {
      return false;
    }
    length = static_cast<uint16_t>(length - chunk);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  storeFilePath = storeFilePathForBook(filePath, bookType);
  const std::string backup = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover clipping backup");
    return false;
  }
  if (!Storage.exists(storeFilePath.c_str())) return true;

  return readFromFile();
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text,
                                                    const uint32_t layoutSignature, const uint32_t startOffset,
                                                    const uint32_t endOffset) {
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  const time_t now = time(nullptr);
  clipping.timestamp = now > 1577836800 ? static_cast<uint32_t>(now) : 0;
  clipping.layoutSignature = layoutSignature;
  clipping.startOffset = startOffset;
  clipping.endOffset = endOffset;
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  clipping.textLength = static_cast<uint16_t>(std::min(text.size(), CLIPPING_TEXT_MAX));

  clippings.push_back(std::move(clipping));
  dirty = true;
  if (!writeToFile(&text, clippings.size() - 1)) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  dirty = false;
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (index >= clippings.size()) return false;
  if (clippings[index].id[0]) {
    HalFile journal = Storage.open((storeFilePath + ".deleted").c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!journal || journal.write(reinterpret_cast<const uint8_t*>(clippings[index].id), sizeof(clippings[index].id)) !=
                        sizeof(clippings[index].id)) {
      LOG_ERR("CLIP", "Failed to queue clipping deletion");
      return false;
    }
    journal.flush();
  }
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    return false;
  }
  return true;
}

const Clipping* ClippingStore::clippingAt(const size_t index) const {
  if (index >= clippings.size()) return nullptr;
  return &clippings[index];
}

bool ClippingStore::readClippingPreview(const size_t index, char* out, const size_t outSize, size_t& outLength) const {
  outLength = 0;
  if (out && outSize > 0) out[0] = '\0';
  const Clipping* clipping = clippingAt(index);
  if (!out || outSize == 0 || !clipping || storeFilePath.empty()) {
    LOG_ERR("CLIP", "Invalid clipping preview index: %u", static_cast<unsigned>(index));
    return false;
  }
  if (clipping->textLength == 0) return true;

  HalFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) return false;
  if (!f.seek(clipping->textOffset)) {
    LOG_ERR("CLIP", "Failed to seek clipping preview at %u", clipping->textOffset);
    return false;
  }
  const bool ok = clippingPreview::read(f, clipping->textLength, out, outSize, outLength);
  if (!ok) LOG_ERR("CLIP", "Failed to read clipping preview at %u", clipping->textOffset);
  return ok;
}

bool ClippingStore::readClippingText(const size_t index, std::string& out) const {
  const Clipping* clipping = clippingAt(index);
  if (!clipping) return false;
  return readClippingText(*clipping, out);
}

bool ClippingStore::readClippingText(const Clipping& clipping, std::string& out) const {
  out.clear();
  if (clipping.textLength == 0) return true;
  if (storeFilePath.empty()) return false;

  HalFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) {
    return false;
  }
  if (!f.seek(clipping.textOffset)) {
    LOG_ERR("CLIP", "Failed to seek clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
    return false;
  }
  out.resize(clipping.textLength);
  const int expected = static_cast<int>(clipping.textLength);
  const bool ok = f.read(&out[0], clipping.textLength) == expected;
  if (!ok) {
    out.clear();
    LOG_ERR("CLIP", "Failed to read clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
  }
  return ok;
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

bool ClippingStore::prepareSync() {
  for (Clipping& clipping : clippings) {
    if (clipping.id[0]) continue;
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      clipping.id[i * 2] = HEX_DIGITS[bytes[i] >> 4];
      clipping.id[i * 2 + 1] = HEX_DIGITS[bytes[i] & 15];
    }
    clipping.id[32] = '\0';
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::finishUploads() {
  for (Clipping& clipping : clippings) {
    if (!clipping.pendingUpload) continue;
    clipping.pendingUpload = false;
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::nextDeletion(uint32_t& offset, char (&id)[65]) const {
  id[0] = '\0';
  const std::string path = storeFilePath + ".deleted";
  if (!Storage.exists(path.c_str())) return true;
  HalFile journal;
  if (!Storage.openFileForRead("CLIP", path, journal)) return false;
  if (journal.size() % sizeof(id) != 0 || !journal.seek(offset)) {
    LOG_ERR("CLIP", "Invalid clipping deletion journal");
    return false;
  }
  while (offset < journal.size()) {
    if (journal.read(reinterpret_cast<uint8_t*>(id), sizeof(id)) != sizeof(id)) {
      LOG_ERR("CLIP", "Failed to read clipping deletion");
      return false;
    }
    offset += sizeof(id);
    id[sizeof(id) - 1] = '\0';
    const bool stillPresent =
        std::any_of(clippings.begin(), clippings.end(), [&id](const Clipping& c) { return strcmp(c.id, id) == 0; });
    if (!stillPresent) return true;
  }
  id[0] = '\0';
  return true;
}

bool ClippingStore::finishDeletions() {
  const std::string path = storeFilePath + ".deleted";
  if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
  LOG_ERR("CLIP", "Failed to clear acknowledged deletions");
  return false;
}

bool ClippingStore::applyRemote(const Clipping& clipping, const std::string& text, const bool deleted) {
  const auto it = std::find_if(clippings.begin(), clippings.end(),
                               [&clipping](const Clipping& c) { return strcmp(c.id, clipping.id) == 0; });
  const size_t index = static_cast<size_t>(it - clippings.begin());
  if (it == clippings.end()) {
    if (deleted) return true;
    if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
      LOG_ERR("CLIP", "Remote clipping exceeds local book limit");
      return false;
    }
    clippings.push_back(clipping);
    if (!writeToFile(&text, index)) {
      clippings.pop_back();
      return false;
    }
    return true;
  }
  if (!deleted && it->syncRevision == clipping.syncRevision && !it->pendingUpload) return true;
  const Clipping previous = *it;
  if (deleted)
    clippings.erase(it);
  else
    *it = clipping;
  if (writeToFile(deleted ? nullptr : &text, index)) return true;
  if (deleted)
    clippings.insert(clippings.begin() + index, previous);
  else
    clippings[index] = previous;
  return false;
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out) {
  out.clear();
  HalFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) || (version < LEGACY_VERSION || version > VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, title, HEADER_STRING_MAX) ||
      !serialization::tryReadString(f, author, HEADER_STRING_MAX) ||
      !serialization::tryReadString(f, storedPath, HEADER_STRING_MAX)) {
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    return false;
  }

  if (path == storeFilePath) {
    if (bookTitle.empty()) bookTitle = std::move(title);
    if (bookAuthor.empty()) bookAuthor = std::move(author);
  }
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= LAYOUT_VERSION && !serialization::tryReadPod(f, clipping.layoutSignature)) {
      LOG_ERR("CLIP", "Clipping file truncated at layout signature, record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= VERSION &&
        (!serialization::tryReadPod(f, clipping.startOffset) || !serialization::tryReadPod(f, clipping.endOffset) ||
         !serialization::tryReadPod(f, clipping.syncRevision) ||
         !serialization::tryReadPod(f, clipping.pendingUpload) ||
         f.read(reinterpret_cast<uint8_t*>(clipping.id), sizeof(clipping.id)) != sizeof(clipping.id))) {
      LOG_ERR("CLIP", "Truncated clipping sync metadata");
      return false;
    }
    clipping.id[sizeof(clipping.id) - 1] = '\0';
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    if (version == LEGACY_VERSION) {
      uint32_t textLen = 0;
      if (!serialization::tryReadPod(f, textLen)) {
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      clipping.textLength = static_cast<uint16_t>(std::min<uint32_t>(textLen, CLIPPING_TEXT_MAX));
      if (textLen > 0 && !f.seekCur(textLen)) {
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    } else {
      if (!serialization::tryReadPod(f, clipping.textLength)) {
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      if (clipping.textLength > CLIPPING_TEXT_MAX) {
        LOG_ERR("CLIP", "Clipping text length %u exceeds max, record %u: %s", clipping.textLength, i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      if (clipping.textLength > 0 && !f.seekCur(clipping.textLength)) {
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    }
    out.push_back(std::move(clipping));
  }

  return true;
}

bool ClippingStore::writeToFile(const std::string* replacementText, const size_t replacementIndex,
                                const std::string* textSourcePath) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  const std::string tmpPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered clipping backup: %s", storeFilePath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(backupPath.c_str()) && Storage.exists(storeFilePath.c_str())) Storage.remove(backupPath.c_str());

  HalFile source;
  const std::string& sourcePath = textSourcePath ? *textSourcePath : storeFilePath;
  const bool hasTextSource = Storage.exists(sourcePath.c_str());
  const bool hasDestination = Storage.exists(storeFilePath.c_str());
  if (hasTextSource && !Storage.openFileForRead("CLIP", sourcePath, source)) {
    LOG_ERR("CLIP", "Failed to open clipping source for rewrite: %s", sourcePath.c_str());
    return false;
  }

  HalFile f = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (source) source.close();
    LOG_ERR("CLIP", "Failed to open clipping temp file for write: %s", tmpPath.c_str());
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  std::vector<uint32_t> newTextOffsets;
  newTextOffsets.reserve(count);
  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.layoutSignature) ||
        !serialization::tryWritePod(f, clipping.startOffset) || !serialization::tryWritePod(f, clipping.endOffset) ||
        !serialization::tryWritePod(f, clipping.syncRevision) ||
        !serialization::tryWritePod(f, clipping.pendingUpload) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.id), sizeof(clipping.id)) != sizeof(clipping.id) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = replacementText && i == replacementIndex;
    const uint16_t textLen = useReplacement
                                 ? static_cast<uint16_t>(std::min(replacementText->size(), CLIPPING_TEXT_MAX))
                                 : clipping.textLength;
    if (!serialization::tryWritePod(f, textLen)) {
      LOG_ERR("CLIP", "Failed to write clipping text length %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const uint32_t newTextOffset = static_cast<uint32_t>(f.position());
    bool wroteText = true;
    if (textLen > 0 && useReplacement) {
      wroteText = f.write(reinterpret_cast<const uint8_t*>(replacementText->data()), textLen) == textLen;
    } else if (textLen > 0) {
      wroteText = source && source.seek(clipping.textOffset) && copyBytes(source, f, textLen);
    }
    if (!wroteText) {
      LOG_ERR("CLIP", "Failed to write clipping text %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    newTextOffsets.push_back(newTextOffset);
  }

  f.flush();
  f.close();
  if (source) source.close();

  if (hasDestination && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to replace clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    if (hasDestination) Storage.rename(backupPath.c_str(), storeFilePath.c_str());
    return false;
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    clippings[i].textOffset = newTextOffsets[i];
  }
  return true;
}

bool ClippingStore::hasForFilePath(const std::string& filePath, const std::string& bookType) {
  return Storage.exists(storeFilePathForBook(filePath, bookType).c_str());
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author, const std::string& bookType,
                                       const bool preserveSource) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);
  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = newStorePath;
  writer.clippings = std::move(migratedClippings);
  if (oldStorePath == newStorePath) {
    return writer.writeToFile();
  }

  const std::string rewriteBackupPath = newStorePath + ".bak";
  if (!Storage.exists(newStorePath.c_str()) && Storage.exists(rewriteBackupPath.c_str())) {
    if (!Storage.rename(rewriteBackupPath.c_str(), newStorePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover destination clipping backup: %s", rewriteBackupPath.c_str());
      return false;
    }
  } else if (Storage.exists(newStorePath.c_str()) && Storage.exists(rewriteBackupPath.c_str()) &&
             !Storage.remove(rewriteBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale destination clipping backup: %s", rewriteBackupPath.c_str());
    return false;
  }

  const std::string backupPath = newStorePath + ".migrate.bak";
  const bool hasDestination = Storage.exists(newStorePath.c_str());
  if (hasDestination) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to remove stale clipping migration backup: %s", backupPath.c_str());
      return false;
    }
    if (!Storage.rename(newStorePath.c_str(), backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to back up destination clippings: %s", newStorePath.c_str());
      return false;
    }
  }
  if (!writer.writeToFile(nullptr, SIZE_MAX, &oldStorePath)) {
    LOG_ERR("CLIP", "Failed to write migrated clippings: %s", newStorePath.c_str());
    if (hasDestination && !Storage.rename(backupPath.c_str(), newStorePath.c_str())) {
      LOG_ERR("CLIP", "Failed to restore destination clipping backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (!preserveSource && !Storage.remove(oldStorePath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove migrated source clippings (non-fatal): %s", oldStorePath.c_str());
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  return true;
}
