/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// Host file system and configuration store for the Polyend Tracker core.
//
// FileSystem and ConfigManager are plain interfaces in core/include/Platform.h,
// so the host backs them with a directory and a file:
//
//   TRACKER_SD (default ./sdcard)   the card root; paths are relative to it
//   <root>/config.bin               the EEPROM image, addressed by byte offset
#ifndef TRACKER_HOST_FS_H
#define TRACKER_HOST_FS_H

#include "Platform.h"
#include "ConfigManager.h"

#include <cstdint>
#include <cstdio>
#include <string>

class HostFileSystem : public FileSystem
{
  public:
    explicit HostFileSystem(const char* root);

    bool isSdCardInserted() override;
    void setMountEventCallback(CardStateChangeCallback funct) override { mountCb = funct; }

    bool exists(const char* path) override;
    bool rename(const char* oldPath, const char* newPath) override;
    bool mkdir(uint8_t hidden, const char* path, bool pFlag = true) override;
    bool remove(const char* path) override;
    bool removeDirWithFiles(const char* path) override;
    bool removeOnlyDirFiles(const char* path) override;

    size_t getFileSize(const char* path) override;
    size_t getFileSize(FileHandle fileHandle) override;
    size_t getFileAvailable(FileHandle fileHandle) override;

    std::tuple<bool, FileHandle> openFile(const char* filepath, FileOpenMode openMode = kRead,
                                          FileHandle* fileHandle = nullptr) override;
    int32_t readFile(void* destBuf, size_t bufLen, FileHandle file) override;
    int32_t writeFile(const void* data, size_t dataSize, FileHandle fileHandle) override;
    bool seekFileCur(int offset, FileHandle fileHandle) override;
    bool seekFile(uint32_t pos, FileHandle fileHandle) override;
    void closeFile(FileHandle file, bool deleteOnClose = true) override;
    void endFile(FileHandle file) override;
    bool isFileOpen(FileHandle file) override;

    bool enableFastSeek(FileHandle, uint32_t*, uint32_t, uint32_t* = nullptr,
                        uint32_t* = nullptr) override { return false; }
    bool readSectorsAsync(uint8_t*, uint32_t, uint32_t) override { return false; }
    bool pollAsyncRead(bool* = nullptr) override { return false; }
    bool waitAsyncRead() override { return false; }
    bool isAsyncReadBusy() override { return false; }

    int createInstrumentsList(const char* path, char** list, uint8_t list_length,
                              uint32_t* sizeOut) override;
    int countContainedFiles(const char* path) override;
    int createFilesList(const char* path, uint8_t start_line, char** list, uint8_t list_length,
                        uint16_t max_used_memory, enListFileFilter filter = enListFileFilter_all,
                        uint16_t skip_count = 0, uint16_t max_files_in_subdir = 0,
                        enListFileFilter folderSizeFilter = enListFileFilter_all) override;
    void listDirContents(const char* dirpath, DirContentType contentType,
                         std::function<bool(const char* name)> nameFilter, char* namesBuffer,
                         int namesBufferSize, int maxNameLen, int& outNamesCount,
                         char** fileList) override;

    int readNbFiles(const char* romPath) override;
    bool nesDir_open(const char* path) override { return false; }
    bool nesDir_readItem(char*, uint8_t*, uint8_t*, uint32_t*) override { return false; }
    void nesDir_close() override {}

    const std::string& root() const { return root_; }

  private:
    struct HostFile
    {
        FILE* fp = nullptr;
        std::string path;
    };

    std::string map(const char* path) const;
    bool inside(const std::string& mapped) const;

    std::string root_;
    CardStateChangeCallback mountCb = nullptr;
};

class HostConfigManager : public ConfigManager
{
  public:
    explicit HostConfigManager(const std::string& directory);

    bool readData(int address, void* data, int size) override;
    void writeData(int address, void* data, int size) override;

  private:
    std::string path_;
};

#endif
