#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#define FILE_WRITE "w"
#define FILE_READ "r"
inline bool failMemoWrite = false;
inline bool failProfileBackupRemove = false;
inline std::map<std::string, std::shared_ptr<std::string>> memoFiles;
class File {
 public:
  File() = default;
  File(std::shared_ptr<std::string> value, bool write) : data(value), writable(write) {}
  explicit operator bool() const { return bool(data); }
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t write(const uint8_t *bytes, size_t length) {
    if (!data || !writable || failMemoWrite) return 0;
    data->append(reinterpret_cast<const char *>(bytes), length);
    return length;
  }
  int read() { return !data || position >= data->size() ? -1 : static_cast<unsigned char>((*data)[position++]); }
  size_t readBytes(char *out, size_t length) {
    size_t count = 0;
    while(count < length) { const int c=read();if(c<0)break;out[count++]=static_cast<char>(c); }
    return count;
  }
  void flush() {}
  void close() {}
 private:
  std::shared_ptr<std::string> data;
  size_t position = 0;
  bool writable = false;
};
namespace fs {
class FS {
 public:
  File open(const char *path, const char *mode) {
    if(std::strcmp(mode,FILE_WRITE)==0) {
      if(failMemoWrite)return File();
      auto value=std::make_shared<std::string>();memoFiles[path]=value;return File(value,true);
    }
    auto it=memoFiles.find(path);return it==memoFiles.end()?File():File(it->second,false);
  }
  bool exists(const char *path) { return memoFiles.count(path)!=0; }
  bool remove(const char *path) {
    if (failProfileBackupRemove && std::strcmp(path, "/personal_profile.bak") == 0) return false;
    return memoFiles.erase(path)!=0;
  }
  bool rename(const char *from,const char *to) {
    auto it=memoFiles.find(from);if(it==memoFiles.end()||memoFiles.count(to))return false;
    memoFiles[to]=it->second;memoFiles.erase(it);return true;
  }
};
}
