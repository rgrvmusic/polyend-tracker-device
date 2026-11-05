/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
#include "fs.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace
{
bool hasParentComponent(const char* path)
{
    if (path == nullptr) return true;
    const char* p = path;
    while (*p)
    {
        while (*p == '/') p++;
        const char* start = p;
        while (*p && *p != '/') p++;
        if (p - start == 2 && start[0] == '.' && start[1] == '.') return true;
    }
    return false;
}

bool isDirectory(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isRegular(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void makeDirs(const std::string& path)
{
    std::string current;
    size_t pos = 0;
    while (pos != std::string::npos)
    {
        pos = path.find('/', pos + 1);
        current = path.substr(0, pos);
        if (!current.empty()) mkdir(current.c_str(), 0775);
    }
}

bool removeTree(const std::string& path)
{
    if (!isDirectory(path)) return unlink(path.c_str()) == 0;
    DIR* d = opendir(path.c_str());
    if (d == nullptr) return false;
    std::vector<std::string> entries;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        entries.push_back(path + "/" + e->d_name);
    }
    closedir(d);
    for (const auto& e : entries) removeTree(e);
    return rmdir(path.c_str()) == 0;
}

const char* extensionOf(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? "" : name.c_str() + dot;
}

bool matchesFilter(const std::string& name, FileSystem::enListFileFilter filter, bool folder)
{
    switch (filter)
    {
        case FileSystem::enListFileFilter_folder: return folder;
        case FileSystem::enListFileFilter_wave:
        case FileSystem::enListFileFilter_wave_inst:
        {
            if (folder) return false;
            const char* ext = extensionOf(name);
            return !strcasecmp(ext, ".wav") ||
                   (filter == FileSystem::enListFileFilter_wave_inst &&
                    (!strcasecmp(ext, ".pti") || !strcasecmp(ext, ".mti")));
        }
        case FileSystem::enListFileFilter_firmware:
            return !folder && !strcasecmp(extensionOf(name), ".bin");
        case FileSystem::enListFileFilter_mod:
            // The device's filter admits both importer formats.
            return !folder && (!strcasecmp(extensionOf(name), ".mod") ||
                               !strcasecmp(extensionOf(name), ".it"));
        case FileSystem::enListFileFilter_game:
            return !folder && !strcasecmp(extensionOf(name), ".nes");
        default: return true;
    }
}

// The device counts the entries a folder would contribute when it is split
// into chunks ("/name (1-50)"), with the same filter.
int countFilesInSubdirectory(const std::string& path, FileSystem::enListFileFilter filter)
{
    DIR* d = opendir(path.c_str());
    if (d == nullptr) return 0;
    int count = 0;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;
        bool folder = isDirectory(path + "/" + e->d_name);
        if (matchesFilter(e->d_name, filter, folder)) count++;
    }
    closedir(d);
    return count;
}
} // namespace

HostFileSystem::HostFileSystem(const char* root)
  : root_(root && *root ? root : "sdcard")
{
    makeDirs(root_);
}

bool HostFileSystem::isSdCardInserted()
{
    return isDirectory(root_);
}

std::string HostFileSystem::map(const char* path) const
{
    if (path == nullptr || !*path) return root_;
    if (hasParentComponent(path)) return std::string();
    std::string p(root_);
    while (*path == '/') path++;
    p += "/";
    p += path;
    return p;
}

bool HostFileSystem::inside(const std::string& mapped) const
{
    return !mapped.empty();
}

bool HostFileSystem::exists(const char* path)
{
    std::string p = map(path);
    struct stat st;
    return inside(p) && stat(p.c_str(), &st) == 0;
}

bool HostFileSystem::rename(const char* oldPath, const char* newPath)
{
    std::string a = map(oldPath), b = map(newPath);
    return inside(a) && inside(b) && ::rename(a.c_str(), b.c_str()) == 0;
}

bool HostFileSystem::mkdir(uint8_t, const char* path, bool)
{
    std::string p = map(path);
    if (!inside(p)) return false;
    makeDirs(p);
    return isDirectory(p);
}

bool HostFileSystem::remove(const char* path)
{
    std::string p = map(path);
    return inside(p) && unlink(p.c_str()) == 0;
}

bool HostFileSystem::removeDirWithFiles(const char* path)
{
    std::string p = map(path);
    return inside(p) && removeTree(p);
}

bool HostFileSystem::removeOnlyDirFiles(const char* path)
{
    std::string p = map(path);
    if (!inside(p)) return false;
    DIR* d = opendir(p.c_str());
    if (d == nullptr) return false;
    std::vector<std::string> files;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string f = p + "/" + e->d_name;
        if (!isDirectory(f)) files.push_back(f);
    }
    closedir(d);
    for (const auto& f : files) unlink(f.c_str());
    return true;
}

size_t HostFileSystem::getFileSize(const char* path)
{
    std::string p = map(path);
    struct stat st;
    if (!inside(p) || stat(p.c_str(), &st) != 0) return 0;
    return (size_t)st.st_size;
}

size_t HostFileSystem::getFileSize(FileHandle fileHandle)
{
    HostFile* f = static_cast<HostFile*>(fileHandle);
    if (f == nullptr || f->fp == nullptr) return 0;
    long pos = ftell(f->fp);
    fseek(f->fp, 0, SEEK_END);
    long end = ftell(f->fp);
    fseek(f->fp, pos < 0 ? 0 : pos, SEEK_SET);
    return end < 0 ? 0 : (size_t)end;
}

size_t HostFileSystem::getFileAvailable(FileHandle fileHandle)
{
    HostFile* f = static_cast<HostFile*>(fileHandle);
    if (f == nullptr || f->fp == nullptr) return 0;
    long pos = ftell(f->fp);
    if (pos < 0) return 0;
    return getFileSize(fileHandle) > (size_t)pos ? getFileSize(fileHandle) - (size_t)pos : 0;
}

std::tuple<bool, FileSystem::FileHandle> HostFileSystem::openFile(const char* filepath,
                                                                  FileOpenMode openMode,
                                                                  FileHandle* fileHandle)
{
    HostFile* file = nullptr;
    if (fileHandle != nullptr && *fileHandle != nullptr)
    {
        file = static_cast<HostFile*>(*fileHandle);
        if (file->fp != nullptr) fclose(file->fp);
        file->fp = nullptr;
    }
    else
    {
        file = new HostFile();
        if (fileHandle != nullptr) *fileHandle = file;
    }

    std::string p = map(filepath);
    if (!inside(p))
    {
        return std::make_tuple(false, (FileHandle)fileHandle ? (FileHandle)file
                                                             : (FileHandle)nullptr);
    }

    const char* mode = "rb";
    switch (openMode)
    {
        case kWriteOpen:
            mode = isRegular(p) ? "r+b" : "w+b";
            break;
        case kWriteAppend: mode = "a+b"; break;
        case kWriteCreateNew: mode = "w+b"; break;
        default: mode = "rb"; break;
    }
    file->fp = fopen(p.c_str(), mode);
    file->path = p;
    if (file->fp == nullptr)
    {
        // As on the device, a caller-provided handle is kept even when the open
        // fails; only a handle we allocated ourselves is released.
        if (fileHandle == nullptr) delete file;
        return std::make_tuple(false, fileHandle ? (FileHandle)file : (FileHandle)nullptr);
    }
    return std::make_tuple(true, (FileHandle)file);
}

// The interface is signed, as the device's SdFat is: -1 means the transfer
// failed, and the UI treats that differently from a short read at end of file.
int32_t HostFileSystem::readFile(void* destBuf, size_t bufLen, FileHandle file)
{
    HostFile* f = static_cast<HostFile*>(file);
    if (f == nullptr || f->fp == nullptr) return -1;
    size_t n = fread(destBuf, 1, bufLen, f->fp);
    if (n == 0 && ferror(f->fp)) return -1;
    return (int32_t)n;
}

int32_t HostFileSystem::writeFile(const void* data, size_t dataSize, FileHandle fileHandle)
{
    HostFile* f = static_cast<HostFile*>(fileHandle);
    if (f == nullptr || f->fp == nullptr) return -1;
    size_t n = fwrite(data, 1, dataSize, f->fp);
    if (n == 0 && dataSize != 0) return -1; // SdFat reports the short count, not a failure
    return (int32_t)n;
}

bool HostFileSystem::seekFileCur(int offset, FileHandle fileHandle)
{
    HostFile* f = static_cast<HostFile*>(fileHandle);
    return f != nullptr && f->fp != nullptr && fseek(f->fp, offset, SEEK_CUR) == 0;
}

bool HostFileSystem::seekFile(uint32_t pos, FileHandle fileHandle)
{
    HostFile* f = static_cast<HostFile*>(fileHandle);
    return f != nullptr && f->fp != nullptr && fseek(f->fp, (long)pos, SEEK_SET) == 0;
}

void HostFileSystem::endFile(FileHandle file)
{
    HostFile* f = static_cast<HostFile*>(file);
    if (f != nullptr && f->fp != nullptr) fflush(f->fp);
}

void HostFileSystem::closeFile(FileHandle file, bool deleteOnClose)
{
    HostFile* f = static_cast<HostFile*>(file);
    if (f == nullptr) return;
    if (f->fp != nullptr)
    {
        fclose(f->fp);
        f->fp = nullptr; // closing twice is harmless, as on the device
    }
    if (deleteOnClose) delete f;
}

bool HostFileSystem::isFileOpen(FileHandle file)
{
    HostFile* f = static_cast<HostFile*>(file);
    return f != nullptr && f->fp != nullptr;
}

int HostFileSystem::createInstrumentsList(const char*, char**, uint8_t, uint32_t* sizeOut)
{
    if (sizeOut) *sizeOut = 0;
    return 0;
}

int HostFileSystem::countContainedFiles(const char* path)
{
    std::string p = map(path);
    DIR* d = inside(p) ? opendir(p.c_str()) : nullptr;
    if (d == nullptr) return 0;
    int count = 0;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (isRegular(p + "/" + e->d_name)) count++;
    }
    closedir(d);
    return count;
}

// Mirrors SdDir::createFilesList(): the caller passes a pointer array whose
// entries the firmware frees itself, so every slot is owned by this function.
// Entry 0..start_line-1 belong to the parent-folder list (the caller passes
// start_line = parentFolder), and the return value counts them in.  Folders
// are written with a leading slash, and a folder larger than
// max_files_in_subdir is chunked the way the device chunks it.
int HostFileSystem::createFilesList(const char* path, uint8_t start_line, char** list,
                                    uint8_t list_length, uint16_t max_used_memory,
                                    enListFileFilter filter, uint16_t skip_count,
                                    uint16_t max_files_in_subdir,
                                    enListFileFilter folderSizeFilter)
{
    for (uint8_t i = 0; i < list_length; i++)
    {
        if (list[i] != nullptr)
        {
            delete list[i];
            list[i] = nullptr;
        }
    }

    uint8_t n = start_line;
    if (max_used_memory < 255) return n;

    std::string p = map(path);
    DIR* d = inside(p) ? opendir(p.c_str()) : nullptr;
    if (d == nullptr) return n;

    uint16_t memory_used = 0;
    while (n < list_length)
    {
        if (memory_used > max_used_memory - 255) break;
        struct dirent* e = readdir(d);
        if (e == nullptr) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue; // AM_HID: the device hides these
        bool folder = isDirectory(p + "/" + e->d_name);
        if (!matchesFilter(e->d_name, filter, folder)) continue;
        if (skip_count > 0)
        {
            skip_count--;
            continue;
        }

        const uint16_t nameLen = (uint16_t)strlen(e->d_name);
        uint16_t len = nameLen + (folder ? 2 : 1);
        list[n] = new char[len];
        memory_used += len;

        if (folder)
        {
            snprintf(list[n], len, "/%s", e->d_name);
            int files = countFilesInSubdirectory(p + "/" + e->d_name, folderSizeFilter);
            if (max_files_in_subdir != 0 && files > (int)max_files_in_subdir)
            {
                uint16_t offset = 0;
                const uint16_t chunkLen = nameLen + 2 + 4 + 2 * 5;
                while (files > 0 && n < list_length - 1)
                {
                    if (list[n] != nullptr)
                    {
                        delete list[n];
                        memory_used -= len;
                    }
                    list[n] = new char[chunkLen];
                    len = chunkLen;
                    memory_used += len;
                    snprintf(list[n], len, "/%s (%d-%d)", e->d_name, offset + 1,
                             offset + (int)max_files_in_subdir);
                    files -= max_files_in_subdir;
                    n++;
                    offset += max_files_in_subdir;
                }
                n--;
            }
        }
        else
        {
            snprintf(list[n], len, "%s", e->d_name);
        }
        n++;
    }
    closedir(d);
    return n;
}

void HostFileSystem::listDirContents(const char* dirpath, DirContentType contentType,
                                     std::function<bool(const char* name)> nameFilter,
                                     char* namesBuffer, int namesBufferSize, int maxNameLen,
                                     int& count, char** fileList)
{
    const int capacity = count; // in: how many names fit; out: how many were written
    std::string p = map(dirpath);
    DIR* d = inside(p) ? opendir(p.c_str()) : nullptr;
    count = 0;
    if (d == nullptr) return;

    std::vector<std::string> entries;
    while (struct dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string name = e->d_name;
        bool folder = isDirectory(p + "/" + name);
        if (contentType == kFolders && !folder) continue;
        if (contentType == kFiles && folder) continue;
        if (nameFilter && !nameFilter(e->d_name)) continue;
        entries.push_back(std::move(name));
    }
    closedir(d);
    std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
        int c = strcasecmp(a.c_str(), b.c_str());
        return c ? c < 0 : a < b;
    });

    int pos = 0;
    for (const auto& name : entries)
    {
        if (pos >= capacity) break;
        int nameLen = (int)name.size() + 1 + (contentType == kFolders ? 1 : 0);
        if (nameLen > namesBufferSize) break;
        if (nameLen > maxNameLen) continue;
        fileList[pos] = namesBuffer;
        if (contentType == kFolders) *namesBuffer++ = '/';
        memcpy(namesBuffer, name.c_str(), name.size() + 1);
        namesBuffer += nameLen;
        namesBufferSize -= nameLen;
        pos++;
    }
    count = pos;
}

int HostFileSystem::readNbFiles(const char* romPath)
{
    return countContainedFiles(romPath);
}

// ---------------------------------------------------------------------------

HostConfigManager::HostConfigManager(const std::string& directory)
  : path_(directory + "/config.bin")
{
}

bool HostConfigManager::readData(int address, void* data, int size)
{
    if (address < 0 || data == nullptr || size <= 0) return false;
    FILE* f = fopen(path_.c_str(), "rb");
    if (f == nullptr) return false;
    if (fseek(f, (long)address, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    return got == (size_t)size;
}

void HostConfigManager::writeData(int address, void* data, int size)
{
    if (address < 0 || data == nullptr || size <= 0) return;
    FILE* f = fopen(path_.c_str(), "r+b");
    if (f == nullptr) f = fopen(path_.c_str(), "w+b");
    if (f == nullptr) return;
    if (fseek(f, (long)address, SEEK_SET) == 0) fwrite(data, 1, (size_t)size, f);
    fclose(f);
}
