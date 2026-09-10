#ifndef MY_FILE_DETAIL_INTERNAL_H
#define MY_FILE_DETAIL_INTERNAL_H

// 内部公共头文件：平台 IO 原语、错误报告、SIMD 分派、行读取器。
// 仅供 MyFile/ 内部各 TU 包含，使用者只需 file.h。

#include "../file.h"

#include <atomic>
#include <climits>
#include <cstring>
#include <limits>
#include <utility>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstddef>

// ==================== SIMD 头文件 ====================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define MYFILE_HAS_SIMD 1
#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#else
#define MYFILE_HAS_SIMD 0
#endif

// ==================== 平台头文件 ====================
#ifndef _WIN32
#include <sys/mman.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#if defined(__linux__)
#include <sys/inotify.h>
#include <poll.h>
#endif

namespace My::detail
{
    // ==================== 常量 ====================
    constexpr size_t kIoBlockSize = 128 * 1024; // 128KB

    // ==================== 底层文件 IO ====================
#ifdef _WIN32
    int OpenRead(const std::filesystem::path &p);
    int OpenWriteTrunc(const std::filesystem::path &p);
    int OpenWriteAppend(const std::filesystem::path &p);
    int OpenCreate(const std::filesystem::path &p);
    long long ReadFd(int fd, void *buf, size_t n);
    long long WriteFd(int fd, const void *buf, size_t n);
    int CloseFd(int fd);
    std::optional<std::uintmax_t> FdSize(int fd);
#else
    int OpenRead(const std::filesystem::path &p);
    int OpenWriteTrunc(const std::filesystem::path &p);
    int OpenWriteAppend(const std::filesystem::path &p);
    int OpenCreate(const std::filesystem::path &p);
    long long ReadFd(int fd, void *buf, size_t n);
    long long WriteFd(int fd, const void *buf, size_t n);
    int CloseFd(int fd);
    std::optional<std::uintmax_t> FdSize(int fd);
#endif

    // fd RAII 包装
    class Fd
    {
    public:
        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        ~Fd()
        {
            if (fd_ >= 0)
                CloseFd(fd_);
        }
        Fd(const Fd &) = delete;
        Fd &operator=(const Fd &) = delete;
        Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
        Fd &operator=(Fd &&other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0)
                    CloseFd(fd_);
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }
        int get() const { return fd_; }
        explicit operator bool() const { return fd_ >= 0; }
        int release()
        {
            const int fd = fd_;
            fd_ = -1;
            return fd;
        }

    private:
        int fd_ = -1;
    };

    // 循环读写
    size_t ReadFull(int fd, char *buf, size_t n);
    bool WriteFull(int fd, const char *buf, size_t n);

    // seek / flush
    bool SeekFd(int fd, long long offset);
    bool FlushToDisk(int fd);

    // 流式查找第 lineNumber 行的起始字节偏移（基于 fd 扫描，内存 O(1)）
    std::optional<std::uintmax_t> FindLineStartFd(int fd, size_t lineNumber);

    // 流式范围重写：把原文件 [start, end) 区间替换为 replacement
    bool RewriteRange(const std::filesystem::path &path, std::uintmax_t start, std::uintmax_t end,
                      std::string_view replacement, std::string_view apiName, std::string_view displayName);

    // ==================== 路径转换 ====================
    std::filesystem::path ToPath(std::string_view s);

    // ==================== 错误报告 ====================
    extern My::ErrorHandler g_errorHandler;

    void PrintError(std::string_view func, std::string_view filename, const std::error_code &ec);
    void PrintError(std::string_view func, std::string_view filename, std::string_view message);
    void PrintError(std::string_view func, const std::filesystem::path &filename, std::string_view message);

    // ==================== 辅助工具 ====================
    std::filesystem::path MakeTempPath(const std::filesystem::path &dest);
    bool ReplaceAtomically(const std::filesystem::path &tempPath, const std::filesystem::path &destPath,
                           bool writeThrough, std::string_view apiName, std::string_view displayName);

    // ==================== 行读取器 ====================
    class LineReader
    {
    public:
        LineReader() : buffer_(std::make_unique<char[]>(kIoBlockSize)) {}

        bool open(const std::filesystem::path &p)
        {
            fd_ = Fd(OpenRead(p));
            start_ = end_ = 0;
            overflow_.clear();
            eof_ = false;
            error_ = false;
            return static_cast<bool>(fd_);
        }

        bool error() const { return error_; }

        bool nextView(std::string_view &out)
        {
            if (tailReturned_)
            {
                overflow_.clear();
                tailReturned_ = false;
            }
            for (;;)
            {
                if (start_ < end_)
                {
                    const char *begin = buffer_.get() + start_;
                    const char *p = static_cast<const char *>(std::memchr(begin, '\n', end_ - start_));
                    if (p != nullptr)
                    {
                        const size_t len = static_cast<size_t>(p - begin);
                        if (overflow_.empty())
                            out = std::string_view(begin, len);
                        else
                        {
                            overflow_.append(begin, len);
                            out = overflow_;
                            tailReturned_ = true;
                        }
                        start_ += len + 1;
                        return true;
                    }
                    overflow_.append(begin, end_ - start_);
                    start_ = end_;
                }
                if (eof_)
                {
                    if (!overflow_.empty())
                    {
                        out = overflow_;
                        tailReturned_ = true;
                        return true;
                    }
                    return false;
                }
                const auto r = ReadFd(fd_.get(), buffer_.get(), kIoBlockSize);
                if (r < 0)
                {
                    error_ = true;
                    eof_ = true;
                    continue;
                }
                if (r == 0)
                {
                    eof_ = true;
                    continue;
                }
                start_ = 0;
                end_ = static_cast<size_t>(r);
            }
        }

        bool next(std::string &out)
        {
            std::string_view view;
            if (!nextView(view))
                return false;
            out.assign(view.data(), view.size());
            return true;
        }

    private:
        Fd fd_;
        std::unique_ptr<char[]> buffer_;
        size_t start_ = 0;
        size_t end_ = 0;
        std::string overflow_;
        bool eof_ = false;
        bool error_ = false;
        bool tailReturned_ = false;
    };

    // ==================== 行定位（内存版） ====================
    std::optional<size_t> FindLineStart(std::string_view data, size_t lineNumber);

    // ==================== SIMD 换行扫描分派 ====================
    using FindNlFn = const char *(*)(const char *, size_t);
    using CountNlFn = size_t (*)(const char *, size_t);

    extern FindNlFn FindNl;
    extern CountNlFn CountNl;

    // ==================== 异步 IO 线程池 ====================
    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t threads);
        ~ThreadPool();
        template <typename F>
        auto Submit(F &&f) -> std::future<decltype(f())>
        {
            using R = decltype(f());
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
            auto fut = task->get_future();
            {
                std::lock_guard lock(mutex_);
                tasks_.push([task]() { (*task)(); });
            }
            cv_.notify_one();
            return fut;
        }

    private:
        std::vector<std::thread> workers_;
        std::queue<std::function<void()>> tasks_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_ = false;
    };

    ThreadPool &GetPool();

} // namespace My::detail

#endif // MY_FILE_DETAIL_INTERNAL_H
