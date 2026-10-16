#include "core/system/file.h"

#include <fmt/format.h>

#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/system/paths.h"

namespace pen {

BinaryFile::BinaryFile(const std::filesystem::path& path, Mode mode) : path_(path) {
#if defined(_WIN32)
  const DWORD access = mode == Mode::Read ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
  const DWORD disposition = mode == Mode::Create ? CREATE_ALWAYS : OPEN_EXISTING;
  HANDLE h = CreateFileW(path.wstring().c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail("cannot open");
  handle_ = h;
#else
  const int flags = mode == Mode::Read ? O_RDONLY : (mode == Mode::Create ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR);
  fd_ = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd_ < 0) fail("cannot open");
#endif
}

BinaryFile::~BinaryFile() { close(); }

BinaryFile::BinaryFile(BinaryFile&& o) noexcept { *this = std::move(o); }

BinaryFile& BinaryFile::operator=(BinaryFile&& o) noexcept {
  if (this != &o) {
    close();
    path_ = std::move(o.path_);
#if defined(_WIN32)
    handle_ = o.handle_;
    o.handle_ = nullptr;
#else
    fd_ = o.fd_;
    o.fd_ = -1;
#endif
  }
  return *this;
}

bool BinaryFile::isOpen() const {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

void BinaryFile::fail(const char* what) const {
#if defined(_WIN32)
  const DWORD err = GetLastError();
  const std::string msg = fmt::format("{} '{}': {}", what, pathToUtf8(path_), std::system_category().message(err));
  if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) throw DiskFullError(msg);
#else
  const int err = errno;
  const std::string msg = fmt::format("{} '{}': {}", what, pathToUtf8(path_), std::strerror(err));
  if (err == ENOSPC || err == EDQUOT) throw DiskFullError(msg);
#endif
  throw FileError(msg);
}

void BinaryFile::readAt(uint64_t offset, void* data, size_t size) const {
  auto* p = static_cast<char*>(data);
  while (size > 0) {
#if defined(_WIN32)
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
    if (!ReadFile(static_cast<HANDLE>(handle_), p, chunk, &got, &ov)) fail("cannot read");
#else
    const ssize_t got = ::pread(fd_, p, std::min<size_t>(size, 1u << 30), static_cast<off_t>(offset));
    if (got < 0) fail("cannot read");
#endif
    if (got == 0) throw FileError(fmt::format("unexpected end of file in '{}'", pathToUtf8(path_)));
    p += got;
    offset += got;
    size -= got;
  }
}

void BinaryFile::writeAt(uint64_t offset, const void* data, size_t size) {
  const auto* p = static_cast<const char*>(data);
  while (size > 0) {
#if defined(_WIN32)
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD put = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
    if (!WriteFile(static_cast<HANDLE>(handle_), p, chunk, &put, &ov)) fail("cannot write");
#else
    const ssize_t put = ::pwrite(fd_, p, std::min<size_t>(size, 1u << 30), static_cast<off_t>(offset));
    if (put < 0) fail("cannot write");
#endif
    p += put;
    offset += put;
    size -= put;
  }
}

uint64_t BinaryFile::size() const {
#if defined(_WIN32)
  LARGE_INTEGER s;
  if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &s)) fail("cannot get the size of");
  return static_cast<uint64_t>(s.QuadPart);
#else
  struct stat st;
  if (::fstat(fd_, &st) != 0) fail("cannot get the size of");
  return static_cast<uint64_t>(st.st_size);
#endif
}

void BinaryFile::truncate(uint64_t size) {
#if defined(_WIN32)
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
  if (!SetFileInformationByHandle(static_cast<HANDLE>(handle_), FileEndOfFileInfo, &info, sizeof(info)))
    fail("cannot truncate");
#else
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) fail("cannot truncate");
#endif
}

void BinaryFile::sync() {
#if defined(_WIN32)
  if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) fail("cannot flush");
#else
  if (::fsync(fd_) != 0) fail("cannot flush");
#endif
}

void BinaryFile::close() {
#if defined(_WIN32)
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
#else
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
#endif
}

void writeFileAtomically(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    BinaryFile f(tmp, BinaryFile::Mode::Create);
    f.writeAt(0, content.data(), content.size());
    f.sync();
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);  // replaces an existing file on both OSes
  if (ec) throw FileError(fmt::format("cannot replace '{}': {}", pathToUtf8(path), ec.message()));
}

uint64_t freeDiskBytes(std::filesystem::path p) {
  std::error_code ec;
  if (p.empty()) p = std::filesystem::current_path(ec);
  p = std::filesystem::absolute(p, ec);
  while (!p.empty() && !std::filesystem::exists(p, ec)) {
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  const auto space = std::filesystem::space(p, ec);
  return ec ? 0 : space.available;
}

}  // namespace pen
