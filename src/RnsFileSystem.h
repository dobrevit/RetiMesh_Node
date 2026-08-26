// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  RnsFileSystem.h — microStore filesystem for microReticulum on LittleFS
//
//  microReticulum builds some of its storage paths relative ("./cache",
//  "./transport_identity"), but the ESP32 VFS only accepts absolute paths.
//  This adapter maps every relative path under RNS_FS_ROOT and never
//  formats: the LittleFS partition also holds the web app and the board.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <string>
#include <list>
#include <memory>
#include <microStore/File.h>
#include <microStore/FileSystem.h>

#ifndef RNS_FS_ROOT
  #define RNS_FS_ROOT "/rns"
#endif

class RnsFileSystem : public microStore::FileSystem {
public:
  RnsFileSystem() : microStore::FileSystem(new Impl()) {}

  static std::string normalize(const char* path) {
    std::string p = path ? path : "";
    if (p.rfind("./", 0) == 0) p = std::string(RNS_FS_ROOT) + p.substr(1);   // "./x" -> "/rns/x"
    else if (p.empty() || p[0] != '/') p = std::string(RNS_FS_ROOT) + "/" + p;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
  }

private:
  class FileImpl : public microStore::FileImpl {
  public:
    explicit FileImpl(fs::File* f) : _file(f) {}
    ~FileImpl() override { if (!_closed) close(); }
    const char* name() const override { return _file->name(); }
    size_t size() const override { return _file->size(); }
    void close() override { _file->close(); _closed = true; }
    int read() override { return _file->read(); }
    size_t write(uint8_t ch) override { return _file->write(ch); }
    size_t read(uint8_t* b, size_t n) override { return _file->read(b, n); }
    size_t write(const uint8_t* b, size_t n) override { return _file->write(b, n); }
    int available() override { return _file->available(); }
    int peek() override { return _file->peek(); }
    size_t tell() override { return _file->position(); }
    long seek(uint32_t pos, microStore::SeekMode m) override {
      fs::SeekMode sm = m == microStore::SeekMode::SeekModeCur ? fs::SeekMode::SeekCur
                      : m == microStore::SeekMode::SeekModeEnd ? fs::SeekMode::SeekEnd : fs::SeekMode::SeekSet;
      return _file->seek(pos, sm);
    }
    void flush() override { _file->flush(); }
    bool isValid() const override { return _file && !_closed; }
  private:
    std::unique_ptr<fs::File> _file;
    bool _closed = false;
  };

  class Impl : public microStore::FileSystemImpl {
  public:
    bool format() override { return false; }               // never
    bool init(bool = true) override { LittleFS.mkdir(RNS_FS_ROOT); return true; }
    microStore::File open(const char* path, microStore::File::Mode mode, const bool = false) override {
      const char* pm;
      switch (mode) {
        case microStore::File::ModeRead:       pm = FILE_READ;   break;
        case microStore::File::ModeWrite:      pm = FILE_WRITE;  break;
        case microStore::File::ModeAppend:     pm = FILE_APPEND; break;
        case microStore::File::ModeReadWrite:  pm = "w+";        break;
        case microStore::File::ModeReadAppend: pm = "a+";        break;
        default: return {};
      }
      std::string p = normalize(path);
      fs::File* f = new fs::File(LittleFS.open(p.c_str(), pm));
      if (!f || !(*f)) { delete f; return {}; }
      return microStore::File(new FileImpl(f));
    }
    bool exists(const char* path) override { return LittleFS.exists(normalize(path).c_str()); }
    bool remove(const char* path) override { return LittleFS.remove(normalize(path).c_str()); }
    bool rename(const char* a, const char* b) override { return LittleFS.rename(normalize(a).c_str(), normalize(b).c_str()); }
    bool mkdir(const char* path) override { std::string p = normalize(path); return LittleFS.exists(p.c_str()) || LittleFS.mkdir(p.c_str()); }
    bool rmdir(const char* path) override { return LittleFS.rmdir(normalize(path).c_str()); }
    bool isDirectory(const char* path) override {
      fs::File f = LittleFS.open(normalize(path).c_str(), FILE_READ);
      if (!f) return false;
      bool d = f.isDirectory(); f.close(); return d;
    }
    std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing cb = nullptr) override {
      std::list<std::string> out;
      fs::File root = LittleFS.open(normalize(path).c_str());
      if (!root) return out;
      for (fs::File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (cb) cb((char*)f.name()); else out.push_back(f.name());
        f.close();
      }
      root.close();
      return out;
    }
    size_t storageSize() override { return LittleFS.totalBytes(); }
    size_t storageAvailable() override { return LittleFS.totalBytes() - LittleFS.usedBytes(); }
  };
};
