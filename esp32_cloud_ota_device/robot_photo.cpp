#include "robot_photo.h"

#include <atomic>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "web_admin.h"

namespace RobotPhoto {
namespace {
constexpr char STORE_PATH[] = "/display.r565";
constexpr char TEMP_PATH[] = "/display.tmp";
constexpr char BACKUP_PATH[] = "/display.bak";
constexpr size_t HEADER_BYTES = 8;
constexpr size_t MAX_FILE_BYTES = HEADER_BYTES + 320U * 240U * 2U;
fs::FS *storage = nullptr;
SemaphoreHandle_t mutex = nullptr;
struct PhotoInfo { bool present = false; uint16_t width = 0; uint16_t height = 0; };
PhotoInfo photos[MAX_PHOTOS];
uint8_t selectedId = 0;
Preferences photoPreferences;
void (*selectionChanged)() = nullptr;
uint16_t viewportWidth = 0;
uint16_t viewportHeight = 0;
std::atomic<bool> photoPresent{false};
std::atomic<uint32_t> photoRevision{1};
bool (*mayModify)() = nullptr;

class Lock {
 public:
  Lock() : held_(mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {}
  ~Lock() { if (held_) xSemaphoreGive(mutex); }
  explicit operator bool() const { return held_; }
 private:
  bool held_;
};

struct UploadState {
  File file;
  bool started = false;
  bool complete = false;
  uint8_t header[HEADER_BYTES] = {};
  size_t received = 0;
  size_t expected = 0;
  uint8_t slot = 0;
  int errorCode = 0;
  const char *error = "";
} upload;

bool dimensionsSupported(uint16_t width, uint16_t height) {
  return (width == 320 && height == 240) || (width == 160 && height == 128) ||
         (width == 240 && height == 320) || (width == 128 && height == 160);
}

bool storedDimensionsSupported(uint16_t width, uint16_t height) {
  // Preserve files prepared by the old status-bar/letterbox version. Uploads
  // must still match the current full-screen viewport in receiveUpload().
  return dimensionsSupported(width, height) ||
         (width == 320 && height == 212) || (width == 160 && height == 110) ||
         (width == 240 && height == 292) || (width == 128 && height == 142);
}

bool decodeHeader(const uint8_t *header, uint16_t &width, uint16_t &height) {
  if (memcmp(header, "R565", 4) != 0) return false;
  width = static_cast<uint16_t>(header[4] | (header[5] << 8));
  height = static_cast<uint16_t>(header[6] | (header[7] << 8));
  return storedDimensionsSupported(width, height);
}

bool validateFile(const char *path, uint16_t *widthOut = nullptr,
                  uint16_t *heightOut = nullptr) {
  File file = storage->open(path, FILE_READ);
  if (!file) return false;
  uint8_t header[HEADER_BYTES];
  uint16_t width = 0, height = 0;
  const bool valid = file.read(header, sizeof(header)) == sizeof(header) &&
      decodeHeader(header, width, height) &&
      file.size() == HEADER_BYTES + static_cast<size_t>(width) * height * 2;
  file.close();
  if (valid && widthOut) *widthOut = width;
  if (valid && heightOut) *heightOut = height;
  return valid;
}

void photoPath(uint8_t id, char (&path)[24]) {
  snprintf(path, sizeof(path), "/photo%u.r565", id);
}

uint8_t firstPhotoLocked() {
  for (uint8_t id = 1; id <= MAX_PHOTOS; ++id) if (photos[id - 1].present) return id;
  return 0;
}

uint8_t freeSlotLocked() {
  for (uint8_t id = 1; id <= MAX_PHOTOS; ++id) if (!photos[id - 1].present) return id;
  return 0;
}

void scanAlbumLocked() {
  for (uint8_t id = 1; id <= MAX_PHOTOS; ++id) {
    char path[24]; photoPath(id, path);
    PhotoInfo &info = photos[id - 1];
    info = PhotoInfo{};
    info.present = validateFile(path, &info.width, &info.height);
  }
  if (selectedId < 1 || selectedId > MAX_PHOTOS || !photos[selectedId - 1].present)
    selectedId = firstPhotoLocked();
  photoPresent.store(selectedId != 0);
}

bool selectLocked(uint8_t id) {
  if (id < 1 || id > MAX_PHOTOS || !photos[id - 1].present) return false;
  if (id == selectedId) return true;
  if (!photoPreferences.putUChar("selected", id)) return false;
  selectedId = id;
  photoRevision.fetch_add(1);
  return true;
}

bool stepLocked(int direction) {
  if (!selectedId) return false;
  uint8_t id = selectedId;
  for (uint8_t checked = 0; checked < MAX_PHOTOS; ++checked) {
    id = direction < 0 ? (id == 1 ? MAX_PHOTOS : id - 1) : (id == MAX_PHOTOS ? 1 : id + 1);
    if (photos[id - 1].present) return selectLocked(id);
  }
  return false;
}

// Migrating the previous single photo is one-time; a later deletion must never
// resurrect the old backup. Existing album slots are never overwritten.
bool migrateLegacyLocked() {
  if (photoPreferences.getBool("album_v1", false)) return true;
  char path[24]; photoPath(1, path);
  const char *legacy = validateFile(STORE_PATH) ? STORE_PATH :
                       validateFile(BACKUP_PATH) ? BACKUP_PATH : nullptr;
  if (legacy && !validateFile(path)) {
    if (storage->exists(path) && !storage->remove(path)) return false;
    if (!storage->rename(legacy, path)) return false;
  }
  if (!photoPreferences.putBool("album_v1", true)) return false;
  storage->remove(STORE_PATH);
  storage->remove(BACKUP_PATH);
  return true;
}

void discardLocked() {
  upload.file.close();
  if (storage && storage->exists(TEMP_PATH)) storage->remove(TEMP_PATH);
}

void resetLocked() {
  discardLocked();
  upload = UploadState{};
}

void failLocked(int code, const char *message) {
  if (!upload.errorCode) {
    upload.errorCode = code;
    upload.error = message;
  }
  discardLocked();
}

bool commitLocked() {
  uint16_t width = 0, height = 0;
  if (!upload.slot || !validateFile(TEMP_PATH, &width, &height)) {
    failLocked(400, "图片数据不完整，请重新上传。");
    return false;
  }
  char path[24]; photoPath(upload.slot, path);
  if (photos[upload.slot - 1].present) {
    failLocked(409, "此位置已有图片，请刷新相册后重试。");
    return false;
  }
  // Only an invalid leftover can occupy a free slot. Never replace a valid image.
  if (storage->exists(path) && !storage->remove(path)) {
    failLocked(507, "清理未完成图片失败，已有照片保持不变。");
    return false;
  }
  if (!storage->rename(TEMP_PATH, path)) {
    failLocked(507, "保存图片失败，已有照片保持不变。");
    return false;
  }
  PhotoInfo &info = photos[upload.slot - 1];
  info.present = true; info.width = width; info.height = height;
  if (!selectedId) {
    selectedId = upload.slot;
    photoPreferences.putUChar("selected", selectedId);
  }
  photoPresent.store(true);
  photoRevision.fetch_add(1);
  return true;
}

void sendError(WebServer &server, int status, const char *message) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "text/plain; charset=utf-8", message);
}

void receiveUpload(WebServer &server) {
  HTTPUpload &part = server.upload();
  Lock lock;
  if (!lock) return;  // Completion rejects an incomplete/unstarted transfer.
  if (part.status == UPLOAD_FILE_ABORTED) {
    resetLocked();
    return;
  }
  if (part.status == UPLOAD_FILE_START) {
    if (upload.started) {
      failLocked(400, "每个请求只能传一张；请使用相册页面批量上传。");
      return;
    }
    resetLocked();
    upload.started = true;
    if (!WebAdmin::authenticated(server)) {
      failLocked(401, "请先登录设备后台。");
      return;
    }
    if (!storage) {
      failLocked(503, "图片存储尚未就绪。");
      return;
    }
    if (mayModify && !mayModify()) {
      failLocked(409, "设备正在忙，请空闲后重新上传。");
      return;
    }
    if (part.name != "photo") {
      failLocked(400, "请选择图片后使用页面中的上传按钮。");
      return;
    }
    upload.slot = freeSlotLocked();
    if (!upload.slot) {
      failLocked(409, "相册已满（最多8张），请先删除不需要的图片。");
      return;
    }
    upload.file = storage->open(TEMP_PATH, FILE_WRITE);
    if (!upload.file) failLocked(507, "无法写入图片，存储空间可能不足。");
    return;
  }
  if (!upload.started || upload.errorCode) return;
  if (!WebAdmin::authenticated(server)) {
    failLocked(401, "登录已失效，请重新登录。");
    return;
  }
  if (mayModify && !mayModify()) {
    failLocked(409, "设备开始处理其他任务，请空闲后重新上传。");
    return;
  }
  if (part.status == UPLOAD_FILE_WRITE) {
    if (upload.complete || part.currentSize > MAX_FILE_BYTES - upload.received) {
      failLocked(413, "图片数据超过屏幕所需大小，请通过此页面上传。");
      return;
    }
    if (upload.received < HEADER_BYTES) {
      const size_t remaining = HEADER_BYTES - upload.received;
      const size_t count = part.currentSize < remaining ? part.currentSize : remaining;
      memcpy(upload.header + upload.received, part.buf, count);
      if (upload.received + count == HEADER_BYTES) {
        uint16_t width = 0, height = 0;
        if (!decodeHeader(upload.header, width, height) ||
            width != viewportWidth || height != viewportHeight) {
          failLocked(400, "图片格式或尺寸不正确，请刷新页面后重新上传。");
          return;
        }
        upload.expected = HEADER_BYTES + static_cast<size_t>(width) * height * 2;
      }
    }
    if (upload.expected && part.currentSize > upload.expected - upload.received) {
      failLocked(400, "图片尾部有多余数据，请重新上传。");
      return;
    }
    if (!upload.file || upload.file.write(part.buf, part.currentSize) != part.currentSize) {
      failLocked(507, "图片写入失败，存储空间可能不足；原图片已保留。");
      return;
    }
    upload.received += part.currentSize;
  } else if (part.status == UPLOAD_FILE_END) {
    upload.file.flush();
    upload.file.close();
    if (!upload.expected || upload.received != upload.expected ||
        part.totalSize != upload.received) {
      failLocked(400, "图片上传不完整，请重新上传。");
      return;
    }
    upload.complete = true;
  }
}
}  // namespace

bool begin(fs::FS &fileSystem, uint16_t width, uint16_t height) {
  if (!dimensionsSupported(width, height)) return false;
  if (!mutex) mutex = xSemaphoreCreateMutex();
  Lock lock;
  if (!lock) return false;
  storage = &fileSystem;
  viewportWidth = width;
  viewportHeight = height;
  resetLocked();
  if (!photoPreferences.begin("robot-photo", false) || !migrateLegacyLocked()) {
    photoPreferences.end(); storage = nullptr; return false;
  }
  selectedId = photoPreferences.getUChar("selected", 0);
  scanAlbumLocked();
  photoRevision.fetch_add(1);
  return true;
}

bool available() { return photoPresent.load(); }
uint32_t revision() { return photoRevision.load(); }

bool copyTo(uint16_t *pixels, uint16_t canvasWidth, uint16_t canvasHeight) {
  if (!pixels || !photoPresent.load() || !dimensionsSupported(canvasWidth, canvasHeight)) return false;
  Lock lock;
  if (!lock || !storage || !photoPresent.load()) return false;
  char path[24]; photoPath(selectedId, path);
  File file = storage->open(path, FILE_READ);
  uint8_t header[HEADER_BYTES];
  uint16_t sourceWidth = 0, sourceHeight = 0;
  if (!file || file.read(header, sizeof(header)) != sizeof(header) ||
      !decodeHeader(header, sourceWidth, sourceHeight) ||
      file.size() != HEADER_BYTES + static_cast<size_t>(sourceWidth) * sourceHeight * 2) {
    file.close();
    return false;
  }
  // Cover the entire destination with ONE inverse scale for both axes. Sample
  // pixel centers in a centered source crop; no independently rounded crop
  // width/height, which could stretch the image when changing orientation.
  uint32_t stepNumerator = sourceHeight;
  uint32_t stepDenominator = canvasHeight;
  if (static_cast<uint32_t>(sourceWidth) * canvasHeight <
      static_cast<uint32_t>(canvasWidth) * sourceHeight) {
    stepNumerator = sourceWidth;
    stepDenominator = canvasWidth;
  }
  const uint32_t leftNumerator = static_cast<uint32_t>(sourceWidth) * stepDenominator -
                                 static_cast<uint32_t>(canvasWidth) * stepNumerator;
  const uint32_t topNumerator = static_cast<uint32_t>(sourceHeight) * stepDenominator -
                                static_cast<uint32_t>(canvasHeight) * stepNumerator;
  const uint32_t sampleDenominator = 2U * stepDenominator;
  uint8_t row[320 * 2];
  int previousSourceY = -1;
  for (uint16_t y = 0; y < canvasHeight; ++y) {
    const uint16_t sourceY = (topNumerator + (2U * y + 1U) * stepNumerator) / sampleDenominator;
    if (sourceY != previousSourceY) {
      if (!file.seek(HEADER_BYTES + static_cast<size_t>(sourceY) * sourceWidth * 2) ||
          file.read(row, sourceWidth * 2) != sourceWidth * 2) {
        file.close();
        return false;
      }
      previousSourceY = sourceY;
    }
    uint16_t *destination = pixels + static_cast<size_t>(y) * canvasWidth;
    for (uint16_t x = 0; x < canvasWidth; ++x) {
      const uint16_t sourceX = (leftNumerator + (2U * x + 1U) * stepNumerator) / sampleDenominator;
      const size_t offset = static_cast<size_t>(sourceX) * 2;
      destination[x] = static_cast<uint16_t>(row[offset] | (row[offset + 1] << 8));
    }
  }
  file.close();
  return true;
}

bool select(uint8_t id) {
  Lock lock;
  return lock && storage && selectLocked(id);
}

bool step(int direction) {
  Lock lock;
  return lock && storage && stepLocked(direction);
}

CommandResult handleNaturalLanguage(const String &input) {
  CommandResult result;
  if (input.length() > 96) return result;
  String text = input;
  const char *punctuation[] = {" ", "\t", "\r", "\n", "。", "，", "！", "？", ".", ",", "!", "?"};
  for (const char *mark : punctuation) text.replace(mark, "");
  const char *prefixes[] = {"请帮我", "帮我", "请", "给我"};
  for (const char *prefix : prefixes) {
    if (text.startsWith(prefix)) { text.remove(0, strlen(prefix)); break; }
  }
  int direction = 0;
  int wanted = 0;
  bool open = text == "打开相册" || text == "显示照片" || text == "显示图片" || text == "打开照片";
  const bool close = text == "退出相册" || text == "退出照片" || text == "关闭相册" || text == "返回主界面";
  if (text == "下一张" || text == "下一张图片" || text == "下一张照片" ||
      text == "切换下一张" || text == "切换下一张图片" || text == "切换下一张照片") direction = 1;
  if (text == "上一张" || text == "上一张图片" || text == "上一张照片" ||
      text == "切换上一张" || text == "切换上一张图片" || text == "切换上一张照片") direction = -1;
  if (!open && !close && !direction) {
    String number;
    if (text.startsWith("显示第")) number = text.substring(strlen("显示第"));
    else if (text.startsWith("切换到第")) number = text.substring(strlen("切换到第"));
    else return result;
    const char *suffixes[] = {"张图片", "张照片", "张"};
    bool validSuffix = false;
    for (const char *suffix : suffixes) if (number.endsWith(suffix)) {
      number.remove(number.length() - strlen(suffix)); validSuffix = true; break;
    }
    if (!validSuffix || number.isEmpty()) return result;
    const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    for (int i = 0; i <= 10; ++i) if (number == digits[i]) wanted = i;
    if (number == "两") wanted = 2;
    bool decimal = true;
    for (size_t i = 0; i < number.length(); ++i) if (number[i] < '0' || number[i] > '9') decimal = false;
    if (decimal && number.length() <= 3) wanted = number.toInt();
    result.handled = true;
    if (wanted < 1 || wanted > MAX_PHOTOS) {
      result.message = "图片编号为1到8，请按相册中显示的编号选择。"; return result;
    }
  }
  result.handled = true;
  if (close) { result.changed = true; result.message = "已退出相册，恢复待机页面。"; return result; }
  Lock lock;
  if (!lock || !storage) { result.message = "相册暂不可用，请稍后重试。"; return result; }
  if (!selectedId) { result.message = "相册中还没有图片，请先在后台上传。"; return result; }
  const bool ok = direction ? stepLocked(direction) : wanted ? selectLocked(wanted) : true;
  if (!ok) { result.message = "此编号没有图片，或选择保存失败，请查看后台相册。"; return result; }
  result.changed = true;
  result.showPhoto = true;
  result.message = "已显示第" + String(selectedId) + "张图片。";
  return result;
}

uint8_t requestedId(WebServer &server) {
  const String id = server.arg("id");
  return id.length() == 1 && id[0] >= '1' && id[0] <= '8' ? id[0] - '0' : 0;
}

void registerRoutes(WebServer &server, bool (*canModify)(), void (*onSelection)()) {
  mayModify = canModify;
  selectionChanged = onSelection;
  server.on("/display/photo/status", HTTP_GET, [&server]() {
    if (!WebAdmin::requireSession(server)) return;
    Lock lock;
    if (!lock || !storage) { sendError(server, 503, "图片存储尚未就绪，请稍后重试。"); return; }
    JsonDocument doc;
    doc["ready"] = true; doc["width"] = viewportWidth; doc["height"] = viewportHeight;
    doc["available"] = photoPresent.load(); doc["current"] = selectedId; doc["capacity"] = MAX_PHOTOS;
    doc["revision"] = photoRevision.load();
    JsonArray items = doc["items"].to<JsonArray>();
    for (uint8_t id = 1; id <= MAX_PHOTOS; ++id) {
      const PhotoInfo &info = photos[id - 1];
      if (!info.present) continue;
      JsonObject item = items.add<JsonObject>();
      item["id"] = id; item["width"] = info.width; item["height"] = info.height;
      item["bytes"] = HEADER_BYTES + static_cast<size_t>(info.width) * info.height * 2;
      item["needs_reupload"] = !dimensionsSupported(info.width, info.height);
    }
    doc["count"] = items.size();
    String response; serializeJson(doc, response);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", response);
  });
  server.on("/display/photo", HTTP_POST, [&server]() {
    if (!WebAdmin::requireSession(server)) { Lock lock; if (lock) resetLocked(); return; }
    Lock lock;
    if (!lock || !storage) { sendError(server, 503, "图片存储忙，请稍后重试。"); return; }
    if (mayModify && !mayModify()) failLocked(409, "设备正在忙，请空闲后重新上传。");
    if (!upload.started || !upload.complete) failLocked(400, "未收到完整图片，请重新上传。");
    if (!upload.errorCode) commitLocked();
    if (upload.errorCode) sendError(server, upload.errorCode, upload.error);
    else {
      char response[40]; snprintf(response, sizeof(response), "{\"ok\":true,\"id\":%u}", upload.slot);
      server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", response);
    }
    resetLocked();
  }, [&server]() { receiveUpload(server); });
  server.on("/display/photo/select", HTTP_POST, [&server]() {
    if (!WebAdmin::requireSession(server)) return;
    if (mayModify && !mayModify()) { sendError(server, 409, "设备正在忙，请稍后切换。"); return; }
    const String action = server.arg("action");
    const bool ok = action == "next" ? step(1) : action == "previous" ? step(-1) :
                    action.isEmpty() ? select(requestedId(server)) : false;
    if (!ok) { sendError(server, 422, "图片不存在或保存失败。"); return; }
    if (selectionChanged) selectionChanged();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/display/photo/image", HTTP_GET, [&server]() {
    if (!WebAdmin::requireSession(server)) return;
    if (mayModify && !mayModify()) { sendError(server, 409, "设备正在忙，请稍后预览。"); return; }
    Lock lock;
    const uint8_t id = requestedId(server);
    if (!lock || !storage || !id || !photos[id - 1].present) { sendError(server, 404, "图片不存在。"); return; }
    char path[24]; photoPath(id, path);
    File file = storage->open(path, FILE_READ);
    if (!file) { sendError(server, 500, "读取图片失败。"); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(file, "application/octet-stream");
    file.close();
  });
  server.on("/display/photo/delete", HTTP_POST, [&server]() {
    if (!WebAdmin::requireSession(server)) return;
    if (mayModify && !mayModify()) { sendError(server, 409, "设备正在忙，请空闲后重试。"); return; }
    Lock lock;
    const uint8_t id = requestedId(server);
    if (!lock || !storage || !id || !photos[id - 1].present) { sendError(server, 404, "请指定已保存的图片编号。"); return; }
    resetLocked();
    char path[24]; photoPath(id, path);
    if (!storage->remove(path)) { sendError(server, 507, "删除失败，图片保持不变。"); return; }
    photos[id - 1] = PhotoInfo{};
    if (selectedId == id) {
      selectedId = firstPhotoLocked();
      photoPreferences.putUChar("selected", selectedId);
    }
    photoPresent.store(selectedId != 0); photoRevision.fetch_add(1);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"ok\":true}");
  });
}
}  // namespace RobotPhoto
