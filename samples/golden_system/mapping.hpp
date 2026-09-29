// Derived platform ownership primitives from the unchanged M25-04 kit.
#pragma once
#include "channel.hpp"
#include <exception>
#include <string_view>
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace golden::cil {
inline bool valid_key(std::string_view key) noexcept {
  return !key.empty() && key.size() <= 48 &&
         std::all_of(key.begin(), key.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}
// Control-thread OS ownership only. No methods here are Runtime callbacks.
class Mapping {
  Region *region_ = nullptr;
  bool owner_name_ = false, fail_close_ = false;
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
  std::array<wchar_t, 80> name_{};
#else
  int fd_ = -1;
  std::array<char, 80> name_{};
#endif
public:
  Mapping() = default;
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;
  ~Mapping() {
    if (close() != Code::ok)
      std::terminate();
  }
  Region *region() noexcept { return region_; }
  bool owns_resources() const noexcept {
#if defined(_WIN32)
    return region_ || handle_;
#else
    return region_ || fd_ >= 0 || owner_name_;
#endif
  }
  void fail_next_close_for_test() noexcept { fail_close_ = true; }
  // On partial failure resources remain visible and close() is mandatory.
  Code open(std::string_view key, bool create) noexcept {
    if (owns_resources() || !valid_key(key))
      return Code::invalid;
#if defined(_WIN32)
    constexpr std::wstring_view prefix = L"Local\\rtfw_golden_";
    name_.fill(L'\0');
    std::copy(prefix.begin(), prefix.end(), name_.begin());
    for (std::size_t i = 0; i < key.size(); ++i)
      name_[prefix.size() + i] = static_cast<wchar_t>(key[i]);
    if (create) {
      handle_ =
          CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                             static_cast<DWORD>(sizeof(Region)), name_.data());
      if (!handle_)
        return Code::io;
      if (GetLastError() == ERROR_ALREADY_EXISTS)
        return Code::exists;
    } else {
      handle_ =
          OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name_.data());
      if (!handle_)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? Code::not_found
                                                      : Code::io;
    }
    region_ = static_cast<Region *>(MapViewOfFile(
        handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Region)));
    if (!region_)
      return Code::schema;
#else
    constexpr std::string_view prefix = "/rtfw_golden_";
    name_.fill('\0');
    std::copy(prefix.begin(), prefix.end(), name_.begin());
    std::copy(key.begin(), key.end(),
              name_.begin() + static_cast<std::ptrdiff_t>(prefix.size()));
    fd_ = shm_open(name_.data(), create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR,
                   0600);
    if (fd_ < 0)
      return errno == EEXIST ? Code::exists
                             : (errno == ENOENT ? Code::not_found : Code::io);
    owner_name_ = create;
    if (create && ftruncate(fd_, static_cast<off_t>(sizeof(Region))) != 0)
      return Code::io;
    struct stat metadata {};
    if (fstat(fd_, &metadata) != 0)
      return Code::io;
    if (metadata.st_size == 0)
      return Code::not_ready;
    if (metadata.st_size != static_cast<off_t>(sizeof(Region)))
      return Code::schema;
    void *memory = mmap(nullptr, sizeof(Region), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED)
      return Code::io;
    region_ = static_cast<Region *>(memory);
#endif
    return Code::ok;
  }
  // Endpoints and all local callers MUST already be detached/joined. Remote
  // views have independent OS lifetimes; this never forcibly unmaps a peer.
  Code close() noexcept {
    if (fail_close_) {
      fail_close_ = false;
      return Code::io;
    }
#if defined(_WIN32)
    if (region_) {
      if (!UnmapViewOfFile(region_))
        return Code::io;
      region_ = nullptr;
    }
    if (handle_) {
      if (!CloseHandle(handle_))
        return Code::io;
      handle_ = nullptr;
    }
#else
    if (owner_name_) {
      if (shm_unlink(name_.data()) != 0 && errno != ENOENT)
        return Code::io;
      owner_name_ = false;
    }
    if (region_) {
      if (munmap(region_, sizeof(Region)) != 0)
        return Code::io;
      region_ = nullptr;
    }
    if (fd_ >= 0) {
      const int fd = fd_;
      fd_ = -1;
      // Linux releases the descriptor even on close errors. Retrying that
      // numeric fd could close an unrelated subsequently acquired object.
      if (::close(fd) != 0)
        return Code::io;
    }
#endif
    return Code::ok;
  }
};
} // namespace golden::cil
