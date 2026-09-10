#include "file.h"
#include <atomic>
#include <climits>
#include <cstring>
#include <limits>
#include <utility>
#include <algorithm>
#include <filesystem>
#include <iostream> // for std::cerr
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

// SIMD 头文件（换行扫描加速）——仅 x86/x86_64 支持
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define MYFILE_HAS_SIMD 1
#ifdef _WIN32
#include <intrin.h> // __cpuid, _mm_* intrinsics (MSVC)
#else
#include <x86intrin.h> // GCC/Clang SSE/AVX intrinsics
#endif
#else
#define MYFILE_HAS_SIMD 0
#endif

// mmap 头文件（非 Windows）
#ifndef _WIN32
#include <sys/mman.h> // mmap / munmap
#endif

// FileWatcher 平台头文件
#ifdef _WIN32
#include <winsock2.h> // 避免 windows.h 与 winsock 顺序冲突
#elif defined(__linux__)
#include <sys/inotify.h>
#include <poll.h>
#endif
// macOS: FSEvents 待实现，FileWatcher 暂不可用

#ifdef _WIN32
#define NOMINMAX // 避免 windows.h 的 min/max 宏干扰 std::min/std::max
#include <windows.h>
#include <io.h> // _wopen / _read / _write / _close / _filelengthi64 / _get_osfhandle
#include <fcntl.h>
#include <process.h> // _getpid
#include <sys/stat.h>
#else
#include <unistd.h> // open / read / write / close
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace
{
    // ==================== 底层文件 IO ====================
    // 直接使用操作系统文件描述符 + 大块读写，绕开 iostream。
    // iostream(fstream) 每一字节都经过 streambuf 虚函数层次，通常比直接 read/write 慢 2~10 倍；
    // 业界高性能文件库（folly、fast_io、llvm::MemoryBuffer）均直接使用系统调用。
    // Windows 下用 _wopen 宽字符路径，中文路径不受代码页影响。

    constexpr size_t kIoBlockSize = 128 * 1024; // 128KB 大块读写，减少系统调用次数

#ifdef _WIN32
    int OpenRead(const std::filesystem::path &p) { return _wopen(p.c_str(), _O_RDONLY | _O_BINARY); }
    int OpenWriteTrunc(const std::filesystem::path &p)
    {
        return _wopen(p.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    }
    int OpenWriteAppend(const std::filesystem::path &p)
    {
        return _wopen(p.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
    }
    int OpenCreate(const std::filesystem::path &p)
    {
        return _wopen(p.c_str(), _O_WRONLY | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
    }
    // _read/_write 的缓冲区大小参数为 unsigned int，大块读写需分段
    long long ReadFd(int fd, void *buf, size_t n)
    {
        return _read(fd, buf, static_cast<unsigned int>(std::min<size_t>(n, UINT_MAX)));
    }
    long long WriteFd(int fd, const void *buf, size_t n)
    {
        return _write(fd, buf, static_cast<unsigned int>(std::min<size_t>(n, UINT_MAX)));
    }
    int CloseFd(int fd) { return _close(fd); }
    std::optional<std::uintmax_t> FdSize(int fd)
    {
        const long long size = _filelengthi64(fd);
        if (size < 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uintmax_t>(size);
    }
#else
    int OpenRead(const std::filesystem::path &p) { return ::open(p.c_str(), O_RDONLY | O_CLOEXEC); }
    int OpenWriteTrunc(const std::filesystem::path &p)
    {
        return ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    int OpenWriteAppend(const std::filesystem::path &p)
    {
        return ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    int OpenCreate(const std::filesystem::path &p)
    {
        return ::open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    }
    long long ReadFd(int fd, void *buf, size_t n) { return ::read(fd, buf, n); }
    long long WriteFd(int fd, const void *buf, size_t n) { return ::write(fd, buf, n); }
    int CloseFd(int fd) { return ::close(fd); }
    std::optional<std::uintmax_t> FdSize(int fd)
    {
        struct stat st;
        if (::fstat(fd, &st) != 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uintmax_t>(st.st_size);
    }
#endif

    // fd 的 RAII 包装：析构自动 close，支持移动
    class Fd
    {
    public:
        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        ~Fd()
        {
            if (fd_ >= 0)
            {
                CloseFd(fd_);
            }
        }
        Fd(const Fd &) = delete;
        Fd &operator=(const Fd &) = delete;
        Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
        Fd &operator=(Fd &&other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0)
                {
                    CloseFd(fd_);
                }
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        int get() const { return fd_; }
        explicit operator bool() const { return fd_ >= 0; }
        // 移交所有权，用于需要显式检查 close 错误的场景
        int release()
        {
            const int fd = fd_;
            fd_ = -1;
            return fd;
        }

    private:
        int fd_ = -1;
    };

    // 循环读取直到读满 n 字节或 EOF/出错，返回实际读取字节数
    size_t ReadFull(int fd, char *buf, size_t n)
    {
        size_t total = 0;
        while (total < n)
        {
            const auto r = ReadFd(fd, buf + total, n - total);
            if (r <= 0)
            {
                break;
            }
            total += static_cast<size_t>(r);
        }
        return total;
    }

    // 循环写入直到写完 n 字节，返回是否成功
    bool WriteFull(int fd, const char *buf, size_t n)
    {
        size_t total = 0;
        while (total < n)
        {
            const auto w = WriteFd(fd, buf + total, n - total);
            if (w <= 0)
            {
                return false;
            }
            total += static_cast<size_t>(w);
        }
        return true;
    }

    // 底层块读行迭代器：大块 read + memchr 拆行，取代 std::getline 的逐字符 iostream 扫描。
    // 行内容不含 '\n'；支持跨块超长行（拼接到内部溢出缓冲区）。
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

        // 是否发生过 I/O 层读错误。读错误会使遍历按 EOF 提前结束，
        // 调用方必须检查本标记，否则可能把截断的数据当完整数据返回给用户
        bool error() const { return error_; }

        // 零拷贝读取下一行：out 指向内部缓冲区（或 overflow_ 成员），
        // 仅在下次调用 next/nextView 之前有效。
        bool nextView(std::string_view &out)
        {
            // 清理上次已返回的尾部视图（上一行来自 overflow_）
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
                        {
                            out = std::string_view(begin, len); // 常规行：零拷贝
                        }
                        else
                        {
                            overflow_.append(begin, len); // 跨块行：拼接后引用成员
                            out = overflow_;
                            tailReturned_ = true;
                        }
                        start_ += len + 1;
                        return true;
                    }
                    // 缓冲区内无 '\n'：整段拼入 overflow，继续读取下一块
                    overflow_.append(begin, end_ - start_);
                    start_ = end_;
                }

                if (eof_)
                {
                    if (!overflow_.empty())
                    {
                        out = overflow_; // 最后一行无 '\n'
                        tailReturned_ = true;
                        return true;
                    }
                    return false;
                }

                const auto r = ReadFd(fd_.get(), buffer_.get(), kIoBlockSize);
                if (r < 0)
                {
                    error_ = true; // 读错误：记录后按 EOF 结束，由调用方检查 error() 决定成败
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

        // 拷贝版：基于 nextView 实现，用于需要持有行内容的场景
        bool next(std::string &out)
        {
            std::string_view view;
            if (!nextView(view))
            {
                return false;
            }
            out.assign(view.data(), view.size());
            return true;
        }

    private:
        Fd fd_;
        // 堆分配而非 std::array 成员：128KB 栈缓冲在深递归或小栈工作线程上有溢出风险，
        // 一次堆分配的开销相对 IO 可忽略
        std::unique_ptr<char[]> buffer_;
        size_t start_ = 0;
        size_t end_ = 0;
        std::string overflow_;
        bool eof_ = false;
        bool error_ = false;
        bool tailReturned_ = false;
    };

    // 将 string_view 转换为 std::filesystem::path。
    // Windows 下假定调用方传入的是 UTF-8 编码（源码以 /utf-8 编译），
    // 直接用 path(string_view) 会按 ANSI 代码页转换，导致中文路径乱码。
    std::filesystem::path ToPath(std::string_view s)
    {
#ifdef _WIN32
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
        return std::filesystem::path(s);
#endif
    }

    // 查找第 lineNumber 行（1 起）的起始字节偏移。
    // 若文件末尾以 '\n' 结尾，允许 lineNumber = 总行数 + 1，此时返回 data.size()（视为末尾空行）。
    // 行号超出范围返回 nullopt。
    std::optional<size_t> FindLineStart(std::string_view data, size_t lineNumber)
    {
        if (lineNumber == 0)
        {
            return std::nullopt;
        }

        size_t pos = 0;
        size_t line = 1;
        while (line < lineNumber)
        {
            const size_t newline = data.find('\n', pos);
            if (newline == std::string_view::npos)
            {
                return std::nullopt;
            }
            pos = newline + 1;
            ++line;
        }
        return pos;
    }

    // ==================== 错误回调定制 ====================
    void DefaultErrorHandler(std::string_view func, std::string_view filename, std::string_view message)
    {
        std::cerr << "[File::" << func << "] " << filename << ": " << message << '\n';
    }

    My::ErrorHandler g_errorHandler = DefaultErrorHandler;

    // 统一错误报告入口：有自定义 handler 走 handler，否则 cerr
    void ReportError(std::string_view func, std::string_view filename, std::string_view message)
    {
        if (g_errorHandler)
        {
            g_errorHandler(func, filename, message);
        }
        else
        {
            std::cerr << "[File::" << func << "] " << filename << ": " << message << '\n';
        }
    }

    void ReportError(std::string_view func, std::string_view filename, const std::error_code &ec)
    {
        ReportError(func, filename, std::string_view(ec.message()));
    }

    void ReportError(std::string_view func, const std::filesystem::path &filename, std::string_view message)
    {
        const std::u8string name = filename.u8string();
        ReportError(func,
                    std::string_view(reinterpret_cast<const char *>(name.data()), name.size()),
                    message);
    }

    // 保留旧名兼容：全部转发到 ReportError
    void PrintError(std::string_view func, std::string_view filename, const std::error_code &ec)
    {
        ReportError(func, filename, ec);
    }

    void PrintError(std::string_view func, std::string_view filename, std::string_view message)
    {
        ReportError(func, filename, message);
    }

    void PrintError(std::string_view func, const std::filesystem::path &filename, std::string_view message)
    {
        ReportError(func, filename, message);
    }

    // 临时文件路径：与目标同目录（保证 rename 在同一文件系统内，原子性前提），
    // 带 PID + 进程内计数器，多进程/多线程同时写同一目标不会撞名
    std::filesystem::path MakeTempPath(const std::filesystem::path &dest)
    {
        static std::atomic<unsigned> counter{0};
        std::filesystem::path temp = dest;
#ifdef _WIN32
        temp += ".tmp." + std::to_string(_getpid()) + "." +
                std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
#else
        temp += ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
#endif
        return temp;
    }

    // 数据落盘：把页缓存中的内容强制刷到存储介质，掉电后内容不丢。仅 close 前调用一次
    bool FlushToDisk(int fd)
    {
#ifdef _WIN32
        const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        return handle != INVALID_HANDLE_VALUE && ::FlushFileBuffers(handle) != 0;
#else
        return ::fsync(fd) == 0;
#endif
    }

    // 原子替换：同目录内 rename 到目标位置。writeThrough 时要求替换操作本身落盘。
    // Windows 上替换刚写完的文件可能被杀毒软件的扫描句柄短暂顶住（sharing violation），
    // 退避重试几次；POSIX rename 无此问题，失败即真失败
    bool ReplaceAtomically(const std::filesystem::path &tempPath, const std::filesystem::path &destPath,
                           bool writeThrough, std::string_view apiName, std::string_view displayName)
    {
#ifdef _WIN32
        const DWORD flags = MOVEFILE_REPLACE_EXISTING | (writeThrough ? MOVEFILE_WRITE_THROUGH : 0);
        for (int attempt = 0;; ++attempt)
        {
            if (::MoveFileExW(tempPath.c_str(), destPath.c_str(), flags))
            {
                return true;
            }
            if (attempt >= 4)
            {
                break;
            }
            ::Sleep(10 << attempt); // 10/20/40/80ms 退避
        }
#else
        std::error_code ec;
        std::filesystem::rename(tempPath, destPath, ec);
        if (!ec)
        {
            return true;
        }
#endif
        PrintError(apiName, displayName, "原子替换失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }

    // ==================== SIMD 换行扫描 ====================
    // 运行时分派：检测 CPU 是否支持 AVX2/SSE2，选择最快的 '\n' 查找实现。
    // 比 memchr 快 2~3 倍：每次比较 16/32 字节而非逐字节。
    // 仅 x86/x86_64 支持 SIMD；非 x86 平台回落 memchr。

    using FindNlFn = const char *(*)(const char *, size_t);
    using CountNlFn = size_t (*)(const char *, size_t);

#if MYFILE_HAS_SIMD
    const int kSimdAvx2 = 2, kSimdSse2 = 1, kSimdScalar = 0;

    int DetectSimdLevel()
    {
#ifdef __AVX2__
        return kSimdAvx2; // 编译期已确定 AVX2
#elif defined(__SSE2__)
        return kSimdSse2;
#else
        int info[4] = {};
#ifdef _WIN32
        __cpuid(info, 1);
#else
        __cpuid(1, info[0], info[1], info[2], info[3]);
#endif
        if (info[2] & (1 << 28))
            return kSimdAvx2;
        if (info[3] & (1 << 26))
            return kSimdSse2;
        return kSimdScalar;
#endif
    }

    const int g_simdLevel = DetectSimdLevel();

    // ---- FindNl: 查找第一个 '\n' ----

    const char *FindNlScalar(const char *data, size_t len)
    {
        return static_cast<const char *>(std::memchr(data, '\n', len));
    }

#ifdef _WIN32
    const char *FindNlSse2Impl(const char *data, size_t len)
    {
        const __m128i nl = _mm_set1_epi8('\n');
        size_t i = 0;
        for (; i + 16 <= len; i += 16)
        {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
            const __m128i cmp = _mm_cmpeq_epi8(chunk, nl);
            const int mask = _mm_movemask_epi8(cmp);
            if (mask)
            {
                unsigned long idx;
                _BitScanForward(&idx, static_cast<unsigned long>(mask));
                return data + i + idx;
            }
        }
        return static_cast<const char *>(std::memchr(data + i, '\n', len - i));
    }

    const char *FindNlAvx2Impl(const char *data, size_t len)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        size_t i = 0;
        for (; i + 32 <= len; i += 32)
        {
            const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            const __m256i cmp = _mm256_cmpeq_epi8(chunk, nl);
            const int mask = _mm256_movemask_epi8(cmp);
            if (mask)
            {
                unsigned long idx;
                _BitScanForward(&idx, static_cast<unsigned long>(mask));
                return data + i + idx;
            }
        }
        return FindNlSse2Impl(data + i, len - i);
    }
#else
    __attribute__((target("sse2")))
    const char *FindNlSse2Impl(const char *data, size_t len)
    {
        const __m128i nl = _mm_set1_epi8('\n');
        size_t i = 0;
        for (; i + 16 <= len; i += 16)
        {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
            const __m128i cmp = _mm_cmpeq_epi8(chunk, nl);
            const int mask = _mm_movemask_epi8(cmp);
            if (mask)
                return data + i + __builtin_ctz(mask);
        }
        return static_cast<const char *>(std::memchr(data + i, '\n', len - i));
    }

    __attribute__((target("avx2")))
    const char *
    FindNlAvx2Impl(const char *data, size_t len)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        size_t i = 0;
        for (; i + 32 <= len; i += 32)
        {
            const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            const __m256i cmp = _mm256_cmpeq_epi8(chunk, nl);
            const int mask = _mm256_movemask_epi8(cmp);
            if (mask)
                return data + i + __builtin_ctz(mask);
        }
        return FindNlSse2Impl(data + i, len - i);
    }
#endif

    const char *FindNlStub(const char *data, size_t len)
    {
        return static_cast<const char *>(std::memchr(data, '\n', len));
    }

    // ---- CountNl: 统计区间内全部 '\n' 数量（单次遍历，SIMD 加速） ----

    size_t CountNlScalar(const char *data, size_t len)
    {
        size_t count = 0;
        const char *p = data;
        while ((p = static_cast<const char *>(std::memchr(p, '\n', len - (p - data)))) != nullptr)
        {
            ++count;
            ++p;
        }
        return count;
    }

#ifdef _WIN32
    size_t CountNlSse2Impl(const char *data, size_t len)
    {
        const __m128i nl = _mm_set1_epi8('\n');
        size_t count = 0;
        size_t i = 0;
        for (; i + 16 <= len; i += 16)
        {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
            const __m128i cmp = _mm_cmpeq_epi8(chunk, nl);
            const int mask = _mm_movemask_epi8(cmp);
            count += __popcnt(mask);
        }
        // 尾部
        for (; i < len; ++i)
            count += (data[i] == '\n');
        return count;
    }

    size_t CountNlAvx2Impl(const char *data, size_t len)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        size_t count = 0;
        size_t i = 0;
        for (; i + 32 <= len; i += 32)
        {
            const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            const __m256i cmp = _mm256_cmpeq_epi8(chunk, nl);
            const int mask = _mm256_movemask_epi8(cmp);
            count += __popcnt(static_cast<unsigned int>(mask));
        }
        // 尾部回退 SSE2 路径
        if (i < len)
            count += CountNlSse2Impl(data + i, len - i);
        return count;
    }
#else
    __attribute__((target("sse2")))
    size_t CountNlSse2Impl(const char *data, size_t len)
    {
        const __m128i nl = _mm_set1_epi8('\n');
        size_t count = 0;
        size_t i = 0;
        for (; i + 16 <= len; i += 16)
        {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
            const __m128i cmp = _mm_cmpeq_epi8(chunk, nl);
            const int mask = _mm_movemask_epi8(cmp);
            count += __builtin_popcount(mask);
        }
        for (; i < len; ++i)
            count += (data[i] == '\n');
        return count;
    }

    __attribute__((target("avx2")))
    size_t
    CountNlAvx2Impl(const char *data, size_t len)
    {
        const __m256i nl = _mm256_set1_epi8('\n');
        size_t count = 0;
        size_t i = 0;
        for (; i + 32 <= len; i += 32)
        {
            const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + i));
            const __m256i cmp = _mm256_cmpeq_epi8(chunk, nl);
            const int mask = _mm256_movemask_epi8(cmp);
            count += __builtin_popcount(static_cast<unsigned int>(mask));
        }
        if (i < len)
            count += CountNlSse2Impl(data + i, len - i);
        return count;
    }
#endif

    // 运行时分派入口
    FindNlFn FindNl = []() -> FindNlFn
    {
        if (g_simdLevel >= kSimdAvx2)
            return FindNlAvx2Impl;
        if (g_simdLevel >= kSimdSse2)
            return FindNlSse2Impl;
        return FindNlStub;
    }();

    CountNlFn CountNl = []() -> CountNlFn
    {
        if (g_simdLevel >= kSimdAvx2)
            return CountNlAvx2Impl;
        if (g_simdLevel >= kSimdSse2)
            return CountNlSse2Impl;
        return CountNlScalar;
    }();

#else // 非 x86 平台：直接用 memchr

    const char *FindNlStub(const char *data, size_t len)
    {
        return static_cast<const char *>(std::memchr(data, '\n', len));
    }

    FindNlFn FindNl = FindNlStub;

    size_t CountNlScalar(const char *data, size_t len)
    {
        size_t count = 0;
        const char *p = data;
        while ((p = static_cast<const char *>(std::memchr(p, '\n', len - (p - data)))) != nullptr)
        {
            ++count;
            ++p;
        }
        return count;
    }

    CountNlFn CountNl = CountNlScalar;

#endif // MYFILE_HAS_SIMD

    // ==================== 异步 IO 线程池 ====================
    // 简单的固定大小工作线程池，供 asyncReadall / asyncWriteAll 使用。
    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t threads)
        {
            for (size_t i = 0; i < threads; ++i)
            {
                workers_.emplace_back([this]
                                      {
                    for (;;)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock lock(mutex_);
                            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                            if (stop_ && tasks_.empty()) return;
                            task = std::move(tasks_.front());
                            tasks_.pop();
                        }
                        task();
                    } });
            }
        }
        ~ThreadPool()
        {
            {
                std::lock_guard lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            for (auto &w : workers_)
                w.join();
        }
        template <typename F>
        auto Submit(F &&f) -> std::future<decltype(f())>
        {
            using R = decltype(f());
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
            auto fut = task->get_future();
            {
                std::lock_guard lock(mutex_);
                tasks_.push([task]()
                            { (*task)(); });
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

    ThreadPool &GetPool()
    {
        static ThreadPool pool((std::max)(2u, std::thread::hardware_concurrency()));
        return pool;
    }

    // 移动读位置（编辑场景在复制前段后跳到区间末尾继续复制）
    bool SeekFd(int fd, long long offset)
    {
#ifdef _WIN32
        return _lseeki64(fd, offset, SEEK_SET) == offset;
#else
        return ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset);
#endif
    }

    // 流式查找第 lineNumber 行（1 起）的起始字节偏移，内存 O(1)。
    // 语义与内存版 FindLineStart 一致：返回第 lineNumber-1 个 '\n' 之后的位置；
    // 文件以 '\n' 结尾时，查"总行数+1"自然得到文件大小（视为末尾空行起点）；
    // 扫描到 EOF 仍未找够（或读错误）返回 nullopt。
    // 每次都从文件头扫描：调用方常在同一 fd 上连续定位多个行号
    std::optional<std::uintmax_t> FindLineStartFd(int fd, size_t lineNumber)
    {
        if (lineNumber == 0)
        {
            return std::nullopt;
        }
        if (lineNumber == 1)
        {
            return 0;
        }
        if (!SeekFd(fd, 0))
        {
            return std::nullopt;
        }

        const auto buffer = std::make_unique<char[]>(kIoBlockSize);
        std::uintmax_t blockOffset = 0;
        size_t newlinesNeeded = lineNumber - 1;
        for (;;)
        {
            const auto r = ReadFd(fd, buffer.get(), kIoBlockSize);
            if (r <= 0)
            {
                return std::nullopt;
            }
            const auto n = static_cast<size_t>(r);
            const char *p = buffer.get();
            const char *const stop = buffer.get() + n;
            while (newlinesNeeded > 0 &&
                   (p = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(stop - p)))) != nullptr)
            {
                ++p;
                --newlinesNeeded;
            }
            if (newlinesNeeded == 0)
            {
                return blockOffset + static_cast<std::uintmax_t>(p - buffer.get());
            }
            blockOffset += n;
        }
    }

    // 流式范围重写（大文件行编辑核心）：把原文件 [start, end) 区间替换为 replacement，
    // 其余内容分块复制到同目录临时文件，成功后原子替换原文件。
    // 内存 O(1)（一块 128KB 缓冲），写盘 O(N)；start == end 为纯插入，replacement 为空为删除。
    // 替换操作本身不做强制刷盘（与 writeAll 同级持久性），需要掉电安全的整文件覆盖用 writeAllAtomic
    bool RewriteRange(const std::filesystem::path &path, std::uintmax_t start, std::uintmax_t end,
                      std::string_view replacement, std::string_view apiName, std::string_view displayName)
    {
        Fd in(OpenRead(path));
        if (!in)
        {
            PrintError(apiName, displayName, "无法打开文件");
            return false;
        }
        const auto sizeOpt = FdSize(in.get());
        if (!sizeOpt)
        {
            PrintError(apiName, displayName, "无法获取文件大小");
            return false;
        }
        const std::uintmax_t fileSize = *sizeOpt;
        if (start > end || end > fileSize)
        {
            PrintError(apiName, displayName, "编辑区间超出文件范围");
            return false;
        }

        const std::filesystem::path tempPath = MakeTempPath(path);
        Fd out(OpenWriteTrunc(tempPath));
        if (!out)
        {
            PrintError(apiName, displayName, "无法创建临时文件");
            return false;
        }
        auto fail = [&](std::string_view msg)
        {
            PrintError(apiName, displayName, msg);
            std::error_code ignore;
            std::filesystem::remove(tempPath, ignore);
            return false;
        };

        const auto buffer = std::make_unique<char[]>(kIoBlockSize);

        // 1) 原样复制 [0, start)
        std::uintmax_t remaining = start;
        while (remaining > 0)
        {
            const auto chunk = static_cast<size_t>(std::min<std::uintmax_t>(remaining, kIoBlockSize));
            const auto r = ReadFd(in.get(), buffer.get(), chunk);
            if (r <= 0)
            {
                return fail("读取原文件失败");
            }
            if (!WriteFull(out.get(), buffer.get(), static_cast<size_t>(r)))
            {
                return fail("写入临时文件失败");
            }
            remaining -= static_cast<std::uintmax_t>(r);
        }

        // 2) 写入替换内容
        if (!replacement.empty() && !WriteFull(out.get(), replacement.data(), replacement.size()))
        {
            return fail("写入临时文件失败");
        }

        // 3) 原文件 seek 到 end，复制剩余部分（end == fileSize 时没有剩余，跳过）
        if (end < fileSize)
        {
            if (!SeekFd(in.get(), static_cast<long long>(end)))
            {
                return fail("定位原文件失败");
            }
            for (;;)
            {
                const auto r = ReadFd(in.get(), buffer.get(), kIoBlockSize);
                if (r < 0)
                {
                    return fail("读取原文件失败");
                }
                if (r == 0)
                {
                    break;
                }
                if (!WriteFull(out.get(), buffer.get(), static_cast<size_t>(r)))
                {
                    return fail("写入临时文件失败");
                }
            }
        }

        if (CloseFd(out.release()) != 0)
        {
            return fail("关闭临时文件失败");
        }
        // rename 前必须关闭读句柄（Windows 上目标文件被打开时 MoveFileEx 替换会失败）
        if (CloseFd(in.release()) != 0)
        {
            std::error_code ignore;
            std::filesystem::remove(tempPath, ignore);
            PrintError(apiName, displayName, "关闭原文件失败");
            return false;
        }

        return ReplaceAtomically(tempPath, path, false, apiName, displayName);
    }
} // namespace

// ==================== 错误回调 API ====================
void My::setErrorHandler(My::ErrorHandler handler) { g_errorHandler = handler; }
My::ErrorHandler My::getErrorHandler() { return g_errorHandler; }

// ==================== 文件信息 ====================

bool My::File::exists(std::string_view filename)
{
    std::error_code ec;
    bool result = std::filesystem::exists(ToPath(filename), ec);
    if (ec)
    {
        PrintError("exists", filename, ec);
        return false;
    }
    return result;
}

std::optional<std::uintmax_t> My::File::size(std::string_view filename)
{
    std::error_code ec;
    auto result = std::filesystem::file_size(ToPath(filename), ec);
    if (ec)
    {
        PrintError("size", filename, ec);
        return std::nullopt;
    }
    return result;
}

std::optional<std::chrono::system_clock::time_point>
My::File::lastModified(std::string_view filename)
{
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(ToPath(filename), ec);
    if (ec)
        return std::nullopt;

    return std::chrono::clock_cast<std::chrono::system_clock>(ftime);
}

std::optional<size_t> My::File::lineCount(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
    {
        PrintError("lineCount", filename, "无法打开文件");
        return std::nullopt;
    }

    // 大块 read + memchr 统计 '\n'：系统调用次数少，且 memchr 由 CRT 以 SIMD 实现，
    // 比 iostream 逐字节扫描快一到两个数量级
    size_t count = 0;
    size_t totalRead = 0;
    char lastByte = '\n';
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);

    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
        {
            PrintError("lineCount", filename, "读取文件失败");
            return std::nullopt; // 读错误时宁可报失败，也不能返回偏小的行数
        }
        if (r == 0)
        {
            break;
        }
        const auto n = static_cast<size_t>(r);
        totalRead += n;

        const char *p = buffer.get();
        const char *const end = buffer.get() + n;
        count += CountNl(p, static_cast<size_t>(end - p));
        lastByte = buffer[n - 1];
    }

    // 文件不以 '\n' 结尾时，最后一行没有换行符，需要补计
    if (totalRead > 0 && lastByte != '\n')
    {
        ++count;
    }
    return count;
}

bool My::File::remove(std::string_view filename)
{
    std::error_code ec;
    bool result = std::filesystem::remove(ToPath(filename), ec);
    if (ec)
    {
        PrintError("remove", filename, ec);
        return false;
    }
    return result;
}

bool My::File::copy(std::string_view src, std::string_view dest)
{
    std::error_code ec;
    bool result = std::filesystem::copy_file(
        ToPath(src),
        ToPath(dest),
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if (!result || ec)
    {
        PrintError("copy", src, ec ? ec.message().c_str() : "复制失败");
        return false;
    }
    return true;
}

bool My::File::copyLarge(std::string_view src, std::string_view dest, size_t /*bufferSize*/)
{
    // 直接委托标准库实现：Windows 内部走 CopyFileW（内核态复制），
    // Linux 走 copy_file_range/sendfile，均远快于用户态分块循环。
    // 原手写实现还存在 bufferSize==0 时死循环、超出 streamsize 范围溢出的问题。
    std::error_code ec;
    const bool result = std::filesystem::copy_file(
        ToPath(src), ToPath(dest),
        std::filesystem::copy_options::overwrite_existing,
        ec);
    if (!result || ec)
    {
        PrintError("copyLarge", src, ec ? ec.message().c_str() : "复制失败");
        return false;
    }
    return true;
}

bool My::File::createDirectory(std::string_view path)
{
    std::error_code ec;
    bool result = std::filesystem::create_directory(ToPath(path), ec);
    if (ec)
    {
        PrintError("createDirectory", path, ec);
        return false;
    }
    return result;
}

bool My::File::createDirectories(std::string_view path)
{
    std::error_code ec;
    bool result = std::filesystem::create_directories(ToPath(path), ec);
    if (ec)
    {
        PrintError("createDirectories", path, ec);
        return false;
    }
    return result;
}

bool My::File::removeDirectory(std::string_view path)
{
    std::error_code ec;
    std::filesystem::remove_all(ToPath(path), ec);
    if (ec)
    {
        PrintError("removeDirectory", path, ec);
        return false;
    }
    return true; // 目录原本不存在也视为成功（幂等，类似 rm -rf）
}

bool My::File::move(std::string_view src, std::string_view dest)
{
#ifdef _WIN32
    // Windows 下移动文件可能被杀毒软件扫描句柄短暂阻塞（sharing violation），
    // 退避重试与 ReplaceAtomically 同策略
    for (int attempt = 0;; ++attempt)
    {
        if (::MoveFileExW(ToPath(src).c_str(), ToPath(dest).c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            return true;
        }
        if (attempt >= 4)
            break;
        ::Sleep(10 << attempt); // 10/20/40/80ms 退避
    }
    PrintError("move", src, "移动失败");
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(ToPath(src), ToPath(dest), ec);
    if (ec)
    {
        PrintError("move", src, ec);
        return false;
    }
    return true;
#endif
}

bool My::File::touch(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    std::error_code ec;

    if (std::filesystem::exists(path, ec))
    {
        if (ec)
        {
            PrintError("touch", filename, ec);
            return false;
        }
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
        if (ec)
        {
            PrintError("touch", filename, ec);
            return false;
        }
        return true;
    }

    // 文件不存在：创建空文件
    Fd fd(OpenCreate(path));
    if (!fd)
    {
        PrintError("touch", filename, "无法创建文件");
        return false;
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("touch", filename, "关闭文件失败");
        return false;
    }
    return true;
}

// ==================== 文件读取 ====================

std::optional<std::string> My::File::readall(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
    {
        PrintError("readall", filename, "无法打开文件");
        return std::nullopt;
    }

    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt)
    {
        PrintError("readall", filename, "无法获取文件大小");
        return std::nullopt;
    }
    const auto size = *sizeOpt;
    if (size > std::string().max_size())
    {
        PrintError("readall", filename, "文件过大，超出内存上限");
        return std::nullopt;
    }

    // C++23 resize_and_overwrite：直接在目标缓冲区上 read，
    // 避免“先清零再读入”对同一块内存的两次写入
    std::string data;
    if (size > 0)
    {
        data.resize_and_overwrite(static_cast<size_t>(size), [&](char *buf, size_t n)
                                  {
                                      return ReadFull(fd.get(), buf, n); // 返回实际读到的字节数
                                  });
    }
    if (data.size() != size)
    {
        PrintError("readall", filename, "读取不完整");
        return std::nullopt;
    }
    return data;
}

std::optional<std::vector<uint8_t>> My::File::readBytes(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
    {
        PrintError("readBytes", filename, "无法打开文件");
        return std::nullopt;
    }

    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt)
    {
        PrintError("readBytes", filename, "无法获取文件大小");
        return std::nullopt;
    }
    const auto size = *sizeOpt;
    if (size > static_cast<std::uintmax_t>((std::numeric_limits<size_t>::max)()))
    {
        PrintError("readBytes", filename, "文件过大");
        return std::nullopt;
    }

    // 注：vector 构造会清零一遍内存；相比磁盘 IO 带宽，这笔开销通常小于 5%，可接受
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 &&
        ReadFull(fd.get(), reinterpret_cast<char *>(buffer.data()), buffer.size()) != buffer.size())
    {
        PrintError("readBytes", filename, "读取不完整");
        return std::nullopt;
    }

    return buffer;
}

std::optional<std::string> My::File::readLine(std::string_view filename, size_t lineNumber)
{
    if (lineNumber == 0)
    {
        PrintError("readLine", filename, "行号必须从1开始");
        return std::nullopt;
    }

    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("readLine", filename, "无法打开文件");
        return std::nullopt;
    }

    std::string line;
    for (size_t currentLine = 1; currentLine <= lineNumber; ++currentLine)
    {
        if (!reader.next(line))
        {
            if (reader.error())
            {
                PrintError("readLine", filename, "读取文件失败");
            }
            else
            {
                PrintError("readLine", filename, "行号超出范围");
            }
            return std::nullopt;
        }
    }
    return line;
}

std::optional<std::string> My::File::readLine(std::string_view filename, size_t lineNumber, const LineIndex &index)
{
    if (!index.valid())
        return std::nullopt;
    if (lineNumber == 0)
    {
        PrintError("readLine", filename, "行号必须从1开始");
        return std::nullopt;
    }
    if (lineNumber > index.lineCount())
    {
        PrintError("readLine", filename, "行号超出范围");
        return std::nullopt;
    }
    if (!index.validate(filename))
    {
        return std::nullopt;
    }
    const auto offset = index.lineStart(lineNumber);
    if (!offset)
        return std::nullopt;

    const std::filesystem::path path = ToPath(filename);
    Fd fd(OpenRead(path));
    if (!fd)
        return std::nullopt;
    if (!SeekFd(fd.get(), static_cast<long long>(*offset)))
        return std::nullopt;

    // 循环读取直到找到 \n 或 EOF，支持超过 128KB 的超长行
    std::string result;
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt; // 读错误
        if (r == 0)
            break; // EOF

        const char *nl = static_cast<const char *>(std::memchr(buffer.get(), '\n', static_cast<size_t>(r)));
        if (nl)
        {
            result.append(buffer.get(), static_cast<size_t>(nl - buffer.get()));
            break;
        }
        result.append(buffer.get(), static_cast<size_t>(r));
    }
    return result;
}

// ==================== 行索引缓存 ====================

My::LineIndex::LineIndex(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    Fd fd(OpenRead(path));
    if (!fd)
        return;
    fileSize_ = FdSize(fd.get()).value_or(0);
    std::error_code ec;
    mtime_ = std::chrono::clock_cast<std::chrono::system_clock>(
        std::filesystem::last_write_time(path, ec));

    // 空文件：不推入偏移，lineCount() 返回 0
    if (fileSize_ == 0)
    {
        valid_ = true;
        return;
    }

    offsets_.reserve(static_cast<size_t>(std::min<std::uintmax_t>(fileSize_ / 40 + 1, 1000000)));
    offsets_.push_back(0);

    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    std::uintmax_t offset = 0;
    bool lastByteIsNl = false;
    bool readError = false;
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
        {
            readError = true;
            break;
        }
        if (r == 0)
            break;
        const char *p = buffer.get();
        size_t remaining = static_cast<size_t>(r);
        lastByteIsNl = (remaining > 0 && p[remaining - 1] == '\n');
        while (remaining > 0)
        {
            const char *nl = FindNl(p, remaining);
            if (!nl)
                break;
            offset += static_cast<std::uintmax_t>(nl - p) + 1;
            offsets_.push_back(offset);
            remaining -= static_cast<size_t>(nl - p) + 1;
            p = nl + 1;
        }
        if (remaining > 0)
            offset += remaining;
    }

    // 末尾 \n 后不计空行（与 File::lineCount / LineReader 语义一致）
    if (lastByteIsNl && !readError && offsets_.size() > 0)
    {
        offsets_.pop_back();
    }

    valid_ = !readError;
}

size_t My::LineIndex::lineCount() const
{
    return offsets_.empty() ? 0 : offsets_.size();
}

std::optional<std::uintmax_t> My::LineIndex::lineStart(size_t lineNumber) const
{
    if (lineNumber == 0 || lineNumber > offsets_.size())
        return std::nullopt;
    return offsets_[lineNumber - 1];
}

bool My::LineIndex::validate(std::string_view filename) const
{
    if (!valid_)
        return false;
    std::error_code ec;
    const auto path = ToPath(filename);
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz != fileSize_)
        return false;
    auto ft = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    return std::chrono::clock_cast<std::chrono::system_clock>(ft) == mtime_;
}

bool My::LineIndex::valid() const
{
    return valid_;
}

std::optional<std::vector<std::string>> My::File::readLines(std::string_view filename, size_t startLine, size_t endLine)
{
    if (startLine == 0 || endLine == 0 || startLine > endLine)
    {
        PrintError("readLines", filename, "无效的行号范围");
        return std::nullopt;
    }

    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("readLines", filename, "无法打开文件");
        return std::nullopt;
    }

    std::vector<std::string> result;
    // 行号范围可能远大于文件实际行数，按声明范围一次性预留可能分配巨量内存甚至溢出
    result.reserve(static_cast<size_t>(std::min<std::uintmax_t>(endLine - startLine + 1, 1024)));

    std::string line;
    size_t currentLine = 0;

    while (reader.next(line))
    {
        ++currentLine;
        if (currentLine >= startLine && currentLine <= endLine)
        {
            result.push_back(std::move(line));
        }
        if (currentLine >= endLine)
        {
            break;
        }
    }

    if (reader.error())
    {
        PrintError("readLines", filename, "读取文件失败");
        return std::nullopt;
    }
    if (result.size() != endLine - startLine + 1)
    {
        PrintError("readLines", filename, "行号范围超出文件范围");
        return std::nullopt;
    }

    return result;
}

std::optional<std::vector<std::string>> My::File::readLines(std::string_view filename, const std::vector<size_t> &lineNumbers)
{
    if (lineNumbers.empty())
    {
        return std::vector<std::string>{};
    }

    for (size_t num : lineNumbers)
    {
        if (num == 0)
        {
            PrintError("readLines", filename, "行号必须从1开始");
            return std::nullopt;
        }
    }

    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("readLines", filename, "无法打开文件");
        return std::nullopt;
    }

    std::vector<std::pair<size_t, size_t>> sortedLines;
    sortedLines.reserve(lineNumbers.size());
    for (size_t i = 0; i < lineNumbers.size(); ++i)
    {
        sortedLines.emplace_back(lineNumbers[i], i);
    }
    std::sort(sortedLines.begin(), sortedLines.end());

    size_t maxLine = sortedLines.back().first;

    std::vector<std::string> result(lineNumbers.size());
    std::string line;
    size_t currentLine = 0;
    size_t nextIdx = 0;

    while (reader.next(line) && nextIdx < sortedLines.size())
    {
        ++currentLine;

        while (nextIdx < sortedLines.size() && sortedLines[nextIdx].first == currentLine)
        {
            // 行号唯一时移动赋值避免拷贝；重复行号（如 {1,1}）则拷贝
            const bool unique = (nextIdx + 1 >= sortedLines.size() ||
                                 sortedLines[nextIdx + 1].first != currentLine);
            result[sortedLines[nextIdx].second] = unique ? std::move(line) : line;
            ++nextIdx;
        }

        if (currentLine >= maxLine)
        {
            break;
        }
    }

    if (reader.error())
    {
        PrintError("readLines", filename, "读取文件失败");
        return std::nullopt;
    }
    if (nextIdx < sortedLines.size())
    {
        PrintError("readLines", filename, "部分行号超出文件范围");
        return std::nullopt;
    }

    return result;
}

std::optional<std::vector<std::string>> My::File::readAllLines(std::string_view filename)
{
    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("readAllLines", filename, "无法打开文件");
        return std::nullopt;
    }

    std::vector<std::string> lines;
    std::string line;
    while (reader.next(line))
    {
        lines.push_back(std::move(line));
    }
    if (reader.error())
    {
        PrintError("readAllLines", filename, "读取文件失败");
        return std::nullopt; // 读到一半出错：宁可失败，也不返回被截断的行集合
    }
    return lines;
}

std::optional<My::MemoryMappedFile> My::File::readMapped(std::string_view filename)
{
    My::MemoryMappedFile mmf(filename);
    if (!mmf.isMapped())
    {
        return std::nullopt;
    }
    return mmf;
}

// ==================== MemoryMappedFile ====================

My::MemoryMappedFile::MemoryMappedFile(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0)
        return;
    mappedSize_ = static_cast<size_t>(sz);
#ifdef _WIN32
    const HANDLE fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        mappedSize_ = 0;
        return;
    }
    mappingHandle_ = CreateFileMappingW(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mappingHandle_)
    {
        CloseHandle(fileHandle);
        mappedSize_ = 0;
        return;
    }
    viewBase_ = MapViewOfFile(mappingHandle_, FILE_MAP_READ, 0, 0, mappedSize_);
    CloseHandle(fileHandle);
    if (!viewBase_)
    {
        CloseHandle(mappingHandle_);
        mappingHandle_ = nullptr;
        mappedSize_ = 0;
    }
#else
    const int fd = OpenRead(path);
    if (fd < 0)
    {
        mappedSize_ = 0;
        return;
    }
    void *addr = mmap(nullptr, mappedSize_, PROT_READ, MAP_PRIVATE, fd, 0);
    CloseFd(fd);
    if (addr == MAP_FAILED)
    {
        mappedSize_ = 0;
        return;
    }
    mappedAddr_ = addr;
#endif
}

My::MemoryMappedFile::~MemoryMappedFile() { unmap(); }

My::MemoryMappedFile::MemoryMappedFile(MemoryMappedFile &&o) noexcept
#ifdef _WIN32
    : mappingHandle_(o.mappingHandle_), viewBase_(o.viewBase_), mappedSize_(o.mappedSize_)
#else
    : mappedAddr_(o.mappedAddr_), mappedSize_(o.mappedSize_)
#endif
{
#ifdef _WIN32
    o.mappingHandle_ = nullptr;
    o.viewBase_ = nullptr;
#else
    o.mappedAddr_ = nullptr;
#endif
    o.mappedSize_ = 0;
}

My::MemoryMappedFile &My::MemoryMappedFile::operator=(MemoryMappedFile &&o) noexcept
{
    if (this != &o)
    {
        unmap();
#ifdef _WIN32
        mappingHandle_ = o.mappingHandle_;
        viewBase_ = o.viewBase_;
        o.mappingHandle_ = nullptr;
        o.viewBase_ = nullptr;
#else
        mappedAddr_ = o.mappedAddr_;
        o.mappedAddr_ = nullptr;
#endif
        mappedSize_ = o.mappedSize_;
        o.mappedSize_ = 0;
    }
    return *this;
}

const char *My::MemoryMappedFile::data() const
{
#ifdef _WIN32
    return static_cast<const char *>(viewBase_);
#else
    return static_cast<const char *>(mappedAddr_);
#endif
}

size_t My::MemoryMappedFile::size() const { return mappedSize_; }
bool My::MemoryMappedFile::isMapped() const { return mappedSize_ > 0; }
std::string_view My::MemoryMappedFile::view() const { return {data(), mappedSize_}; }

void My::MemoryMappedFile::unmap()
{
#ifdef _WIN32
    if (viewBase_)
    {
        UnmapViewOfFile(viewBase_);
        viewBase_ = nullptr;
    }
    if (mappingHandle_)
    {
        CloseHandle(mappingHandle_);
        mappingHandle_ = nullptr;
    }
#else
    if (mappedAddr_)
    {
        munmap(mappedAddr_, mappedSize_);
        mappedAddr_ = nullptr;
    }
#endif
    mappedSize_ = 0;
}

bool My::File::forEachLine(std::string_view filename,
                           const std::function<bool(size_t, std::string_view)> &handler)
{
    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("forEachLine", filename, "无法打开文件");
        return false;
    }

    // 零拷贝遍历：回调收到的 string_view 直接引用内部块缓冲区，仅在回调返回前有效
    std::string_view line;
    size_t lineNumber = 0;
    while (reader.nextView(line))
    {
        ++lineNumber;
        if (!handler(lineNumber, line))
        {
            return true; // 提前终止是正常行为
        }
    }
    if (reader.error())
    {
        PrintError("forEachLine", filename, "读取文件失败");
        return false;
    }
    return true;
}

// ==================== 目录遍历与哈希 ====================

std::vector<std::string> My::File::listFiles(std::string_view path)
{
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(ToPath(path), ec))
    {
        if (entry.is_regular_file())
        {
            const std::u8string u8name = entry.path().filename().u8string();
            result.push_back(std::string(reinterpret_cast<const char *>(u8name.data()), u8name.size()));
        }
    }
    return result;
}

bool My::File::walk(std::string_view path,
                    const std::function<bool(std::string_view, bool)> &callback)
{
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(ToPath(path), ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;
        const std::u8string u8path = it->path().u8string();
        if (!callback(std::string_view(reinterpret_cast<const char *>(u8path.data()), u8path.size()), it->is_directory()))
        {
            return true;
        }
    }
    return !ec;
}

namespace
{
    bool GlobMatch(std::string_view pattern, std::string_view name)
    {
        size_t pi = 0, ni = 0, starP = std::string_view::npos, starN = 0;
        while (ni < name.size())
        {
            if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni]))
            {
                ++pi;
                ++ni;
            }
            else if (pi < pattern.size() && pattern[pi] == '*')
            {
                starP = pi++;
                starN = ni;
            }
            else if (starP != std::string_view::npos)
            {
                pi = starP + 1;
                ni = ++starN;
            }
            else
                return false;
        }
        while (pi < pattern.size() && pattern[pi] == '*')
            ++pi;
        return pi == pattern.size();
    }

    // ==================== SHA-256 ====================
    constexpr uint32_t kSha256K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    constexpr uint32_t Rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Sha256Transform(uint32_t state[8], const uint8_t block[64])
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = Rr(w[i - 15], 7) ^ Rr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rr(w[i - 2], 17) ^ Rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
                 e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t S1 = Rr(e, 6) ^ Rr(e, 11) ^ Rr(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + kSha256K[i] + w[i];
            uint32_t S0 = Rr(a, 2) ^ Rr(a, 13) ^ Rr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
} // namespace (GlobMatch + SHA-256)

std::vector<std::string> My::File::globFiles(std::string_view path, std::string_view pattern)
{
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(ToPath(path), ec))
    {
        const std::u8string u8name = entry.path().filename().u8string();
        const std::string name(reinterpret_cast<const char *>(u8name.data()), u8name.size());
        if (GlobMatch(pattern, name))
        {
            result.push_back(name);
        }
    }
    return result;
}

std::optional<std::string> My::File::fileHash(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
        return std::nullopt;

    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    uint8_t pending[64]{};
    size_t pendingLen = 0;
    uint64_t totalBytes = 0;

    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt;
        if (r == 0)
            break;
        totalBytes += static_cast<size_t>(r);

        const uint8_t *data = reinterpret_cast<const uint8_t *>(buffer.get());
        size_t remaining = static_cast<size_t>(r);

        if (pendingLen > 0)
        {
            size_t need = 64 - pendingLen;
            size_t take = (std::min)(need, remaining);
            std::memcpy(pending + pendingLen, data, take);
            pendingLen += take;
            data += take;
            remaining -= take;
            if (pendingLen == 64)
            {
                Sha256Transform(state, pending);
                pendingLen = 0;
            }
        }
        while (remaining >= 64)
        {
            Sha256Transform(state, data);
            data += 64;
            remaining -= 64;
        }
        if (remaining > 0)
        {
            std::memcpy(pending, data, remaining);
            pendingLen = remaining;
        }
    }

    pending[pendingLen++] = 0x80;
    if (pendingLen > 56)
    {
        std::memset(pending + pendingLen, 0, 64 - pendingLen);
        Sha256Transform(state, pending);
        pendingLen = 0;
    }
    std::memset(pending + pendingLen, 0, 56 - pendingLen);
    uint64_t bits = totalBytes * 8;
    for (int i = 0; i < 8; ++i)
        pending[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    Sha256Transform(state, pending);

    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (int i = 0; i < 8; ++i)
    {
        result[i * 8 + 0] = hex[(state[i] >> 28) & 0xf];
        result[i * 8 + 1] = hex[(state[i] >> 24) & 0xf];
        result[i * 8 + 2] = hex[(state[i] >> 20) & 0xf];
        result[i * 8 + 3] = hex[(state[i] >> 16) & 0xf];
        result[i * 8 + 4] = hex[(state[i] >> 12) & 0xf];
        result[i * 8 + 5] = hex[(state[i] >> 8) & 0xf];
        result[i * 8 + 6] = hex[(state[i] >> 4) & 0xf];
        result[i * 8 + 7] = hex[state[i] & 0xf];
    }
    return result;
}

bool My::File::writeAll(std::string_view filename, std::string_view content)
{
    Fd fd(OpenWriteTrunc(ToPath(filename)));
    if (!fd)
    {
        PrintError("writeAll", filename, "无法创建文件");
        return false;
    }
    if (!WriteFull(fd.get(), content.data(), content.size()))
    {
        PrintError("writeAll", filename, "写入失败");
        return false;
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("writeAll", filename, "关闭文件失败");
        return false;
    }
    return true;
}

bool My::File::writeBytes(std::string_view filename, const std::vector<uint8_t> &data)
{
    Fd fd(OpenWriteTrunc(ToPath(filename)));
    if (!fd)
    {
        PrintError("writeBytes", filename, "无法创建文件");
        return false;
    }
    if (!WriteFull(fd.get(), reinterpret_cast<const char *>(data.data()), data.size()))
    {
        PrintError("writeBytes", filename, "写入失败");
        return false;
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("writeBytes", filename, "关闭文件失败");
        return false;
    }
    return true;
}

bool My::File::appendAll(std::string_view filename, std::string_view content)
{
    Fd fd(OpenWriteAppend(ToPath(filename)));
    if (!fd)
    {
        PrintError("appendAll", filename, "无法打开文件");
        return false;
    }
    if (!WriteFull(fd.get(), content.data(), content.size()))
    {
        PrintError("appendAll", filename, "写入失败");
        return false;
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("appendAll", filename, "关闭文件失败");
        return false;
    }
    return true;
}

bool My::File::writeAllAtomic(std::string_view filename, std::string_view content, bool durable)
{
    const std::filesystem::path destPath = ToPath(filename);
    const std::filesystem::path tempPath = MakeTempPath(destPath);

    Fd fd(OpenWriteTrunc(tempPath));
    if (!fd)
    {
        PrintError("writeAllAtomic", filename, "无法创建临时文件");
        return false;
    }
    if (!WriteFull(fd.get(), content.data(), content.size()))
    {
        PrintError("writeAllAtomic", filename, "写入临时文件失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }
    // durable=true：内容刷盘后再替换。否则掉电时可能出现"新文件存在但内容为空/不完整"
    // （页缓存中的数据已随 rename 元数据一起丢失），这是仅靠 rename 无法覆盖的窗口
    if (durable && !FlushToDisk(fd.get()))
    {
        PrintError("writeAllAtomic", filename, "刷盘失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("writeAllAtomic", filename, "关闭临时文件失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }

    return ReplaceAtomically(tempPath, destPath, durable, "writeAllAtomic", filename);
}

// ==================== 插入方法 ====================
// 全部基于 RewriteRange 流式重写：内存 O(1)，不再"整读整写"（GB 级文件旧实现峰值内存 = 文件大小 × 2）

bool My::File::insertAt(std::string_view filename, size_t position, std::string_view content)
{
    // position == 文件大小表示追加到末尾；越界由 RewriteRange 按实际大小校验
    return RewriteRange(ToPath(filename), position, position, content, "insertAt", filename);
}

bool My::File::insertBeforeLine(std::string_view filename, size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0)
    {
        PrintError("insertBeforeLine", filename, "行号必须从1开始");
        return false;
    }

    const std::filesystem::path path = ToPath(filename);
    std::optional<std::uintmax_t> pos;
    {
        Fd in(OpenRead(path));
        if (!in)
        {
            PrintError("insertBeforeLine", filename, "无法打开文件");
            return false;
        }
        pos = FindLineStartFd(in.get(), lineNumber);
    }
    if (!pos)
    {
        PrintError("insertBeforeLine", filename, "行号超出范围");
        return false;
    }
    return RewriteRange(path, *pos, *pos, content, "insertBeforeLine", filename);
}

bool My::File::insertAfterLine(std::string_view filename, size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0)
    {
        PrintError("insertAfterLine", filename, "行号必须从1开始");
        return false;
    }

    const std::filesystem::path path = ToPath(filename);
    std::optional<std::uintmax_t> pos;
    {
        Fd in(OpenRead(path));
        if (!in)
        {
            PrintError("insertAfterLine", filename, "无法打开文件");
            return false;
        }
        // 插入点 = 第 lineNumber+1 行的起始偏移
        pos = FindLineStartFd(in.get(), lineNumber + 1);
        if (!pos)
        {
            // 目标行之后没有更多行：仅当 lineNumber 是最后一行时允许，插入到文件末尾
            // （回归：原实现要求文件末尾必须有 '\n'，否则"在最后一行后插入"会失败）
            if (!FindLineStartFd(in.get(), lineNumber))
            {
                PrintError("insertAfterLine", filename, "行号超出范围");
                return false;
            }
            pos = FdSize(in.get());
            if (!pos)
            {
                PrintError("insertAfterLine", filename, "无法获取文件大小");
                return false;
            }
        }
    }
    return RewriteRange(path, *pos, *pos, content, "insertAfterLine", filename);
}

// ==================== 删除行方法 ====================

bool My::File::deleteLine(std::string_view filename, size_t lineNumber)
{
    return deleteLines(filename, lineNumber, lineNumber);
}

bool My::File::deleteLines(std::string_view filename, size_t startLine, size_t endLine)
{
    if (startLine == 0 || endLine == 0 || startLine > endLine)
    {
        PrintError("deleteLines", filename, "无效的行号范围");
        return false;
    }

    const std::filesystem::path path = ToPath(filename);
    std::optional<std::uintmax_t> startPos, endPos;
    {
        Fd in(OpenRead(path));
        if (!in)
        {
            PrintError("deleteLines", filename, "无法打开文件");
            return false;
        }
        startPos = FindLineStartFd(in.get(), startLine);
        if (!startPos)
        {
            PrintError("deleteLines", filename, "起始行号超出范围");
            return false;
        }
        endPos = FindLineStartFd(in.get(), endLine + 1);
        if (!endPos)
        {
            // 末尾没有空行时，允许删除到文件末尾（endLine 必须是最后一行）
            if (!FindLineStartFd(in.get(), endLine))
            {
                PrintError("deleteLines", filename, "结束行号超出范围");
                return false;
            }
            endPos = FdSize(in.get());
            if (!endPos)
            {
                PrintError("deleteLines", filename, "无法获取文件大小");
                return false;
            }
        }
    }
    return RewriteRange(path, *startPos, *endPos, std::string_view{}, "deleteLines", filename);
}

// ==================== 异步 IO ====================

std::future<std::optional<std::string>> My::File::asyncReadall(std::string_view filename)
{
    std::string fn(filename);
    return GetPool().Submit([fn]()
                            { return readall(fn); });
}

std::future<bool> My::File::asyncWriteAll(std::string_view filename, std::string_view content)
{
    std::string fn(filename), ct(content);
    return GetPool().Submit([fn = std::move(fn), ct = std::move(ct)]()
                            { return writeAll(fn, ct); });
}

// ==================== 链式写入构建器 ====================

class My::File::Writer::Impl
{
public:
    std::filesystem::path filename;
    bool appendMode;
    std::string buffer;
    bool poisoned = false; // insert() 读取失败时置 true，阻止 commit() 截断原文件

    Impl(std::string_view fname, bool append) : filename(ToPath(fname)), appendMode(append) {}
};

My::File::Writer::Writer(std::string_view filename, bool appendMode)
    : pImpl(std::make_unique<Impl>(filename, appendMode))
{
}

My::File::Writer::~Writer()
{
    if (autoCommit_ && pImpl && !pImpl->buffer.empty())
    {
        if (!commit())
        {
            const std::u8string u8name = pImpl->filename.u8string();
            ReportError("Writer::~Writer",
                        std::string_view(reinterpret_cast<const char *>(u8name.data()), u8name.size()),
                        "自动提交失败");
        }
    }
}

My::File::Writer::Writer(Writer &&) noexcept = default;

My::File::Writer &My::File::Writer::operator=(Writer &&) noexcept = default;

My::File::Writer &My::File::Writer::write(std::string_view content)
{
    pImpl->buffer.append(content);
    return *this;
}

My::File::Writer &My::File::Writer::writeLine(std::string_view content)
{
    pImpl->buffer.append(content);
    pImpl->buffer += '\n';
    return *this;
}

My::File::Writer &My::File::Writer::writeBytes(const std::vector<uint8_t> &data)
{
    pImpl->buffer.append(reinterpret_cast<const char *>(data.data()), data.size());
    return *this;
}

bool My::File::Writer::commit()
{
    if (pImpl->poisoned)
    {
        PrintError("Writer::commit", pImpl->filename, "Writer 已污染(文件存在但读取失败)，拒绝提交以防数据丢失");
        return false;
    }

    Fd fd(pImpl->appendMode ? OpenWriteAppend(pImpl->filename) : OpenWriteTrunc(pImpl->filename));
    if (!fd)
    {
        PrintError("Writer::commit", pImpl->filename, "无法打开文件");
        return false;
    }

    if (!WriteFull(fd.get(), pImpl->buffer.data(), pImpl->buffer.size()))
    {
        PrintError("Writer::commit", pImpl->filename, "写入失败");
        return false; // 失败时保留缓冲区，允许调用方修复后重试 commit
    }
    if (CloseFd(fd.release()) != 0)
    {
        PrintError("Writer::commit", pImpl->filename, "关闭文件失败");
        return false;
    }
    pImpl->buffer.clear();
    return true;
}

std::string_view My::File::Writer::view() const
{
    return pImpl->buffer;
}

std::string My::File::Writer::str() const
{
    return pImpl->buffer;
}

My::File::Writer &My::File::Writer::clear()
{
    pImpl->buffer.clear();
    return *this;
}

My::File::Writer &My::File::Writer::reserve(size_t size)
{
    pImpl->buffer.reserve(size);
    return *this;
}

My::File::Writer &My::File::Writer::insertAt(size_t position, std::string_view content)
{
    if (position > pImpl->buffer.size())
    {
        PrintError("Writer::insertAt", pImpl->filename, "插入位置超出缓冲区范围");
    }
    else
    {
        pImpl->buffer.insert(position, content);
    }
    return *this;
}

My::File::Writer &My::File::Writer::insertBeforeLine(size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0)
    {
        PrintError("Writer::insertBeforeLine", pImpl->filename, "行号必须从1开始");
        return *this;
    }

    if (const auto pos = FindLineStart(pImpl->buffer, lineNumber))
    {
        pImpl->buffer.insert(*pos, content);
    }
    return *this;
}

My::File::Writer &My::File::Writer::insertAfterLine(size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0)
    {
        PrintError("Writer::insertAfterLine", pImpl->filename, "行号必须从1开始");
        return *this;
    }

    // 插入点 = 第 lineNumber+1 行的起始偏移
    auto pos = FindLineStart(pImpl->buffer, lineNumber + 1);
    if (!pos)
    {
        // 目标行之后没有更多行：仅当 lineNumber 是最后一行时允许，插入到缓冲区末尾
        // （修复：原实现在缓冲区末尾无 '\n' 时会静默丢弃这次插入）
        if (!FindLineStart(pImpl->buffer, lineNumber))
        {
            return *this;
        }
        pos = pImpl->buffer.size();
    }
    pImpl->buffer.insert(*pos, content);
    return *this;
}

// 静态工厂方法
My::File::Writer My::File::write(std::string_view filename, bool appendMode)
{
    return Writer(filename, appendMode);
}

My::File::Writer My::File::insert(std::string_view filename)
{
    Writer writer(filename, false);
    if (auto content = readall(filename))
    {
        writer.pImpl->buffer = std::move(*content);
    }
    else if (exists(filename))
    {
        // 文件存在但读取失败 → 标记污染，阻止 commit() 截断原文件
        writer.pImpl->poisoned = true;
        PrintError("insert", filename, "文件存在但读取失败，Writer 已标记为不可提交");
    }
    return writer;
}

My::File::Writer &My::File::Writer::setAutoCommit(bool enable)
{
    autoCommit_ = enable;
    return *this;
}

// ==================== 文件监听 ====================

struct My::FileWatcher::Impl
{
    std::thread worker;
    std::atomic<bool> running{false};
    My::FileWatcher::Callback callback;
#ifdef _WIN32
    HANDLE dirHandle = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE overlapEvent = nullptr;
#elif defined(__linux__)
    int inotifyFd = -1;
#endif
    ~Impl()
    {
        if (running && worker.joinable())
            worker.join();
    }
};

My::FileWatcher::FileWatcher() : pImpl_(std::make_unique<Impl>()) {}
My::FileWatcher::~FileWatcher() { stop(); }

bool My::FileWatcher::isWatching() const { return pImpl_ && pImpl_->running; }

void My::FileWatcher::stop()
{
    if (!pImpl_ || !pImpl_->running)
        return;
    pImpl_->running = false;
#ifdef _WIN32
    // 向停止事件发信号，唤醒 WaitForMultipleObjects
    if (pImpl_->stopEvent)
        SetEvent(pImpl_->stopEvent);
    // 取消挂起的 ReadDirectoryChangesW，使工作线程安全退出
    if (pImpl_->dirHandle != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(pImpl_->dirHandle, nullptr);
    }
#elif defined(__linux__)
    if (pImpl_->inotifyFd >= 0)
    {
        ::close(pImpl_->inotifyFd);
        pImpl_->inotifyFd = -1;
    }
#endif
    if (pImpl_->worker.joinable())
        pImpl_->worker.join();
#ifdef _WIN32
    // 工作线程已退出，安全关闭句柄
    if (pImpl_->dirHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pImpl_->dirHandle);
        pImpl_->dirHandle = INVALID_HANDLE_VALUE;
    }
    if (pImpl_->stopEvent)
    {
        CloseHandle(pImpl_->stopEvent);
        pImpl_->stopEvent = nullptr;
    }
    if (pImpl_->overlapEvent)
    {
        CloseHandle(pImpl_->overlapEvent);
        pImpl_->overlapEvent = nullptr;
    }
#endif
}

bool My::FileWatcher::start(std::string_view path, Callback callback, bool recursive)
{
    if (isWatching())
        return false;
    pImpl_->callback = std::move(callback);
    pImpl_->running = true;
    const std::filesystem::path dirPath = ToPath(path);

#if defined(_WIN32)
    pImpl_->dirHandle = CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (pImpl_->dirHandle == INVALID_HANDLE_VALUE)
    {
        pImpl_->running = false;
        return false;
    }
    pImpl_->stopEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pImpl_->overlapEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    auto *impl = pImpl_.get();
    pImpl_->worker = std::thread([impl, recursive]()
                                 {
        constexpr size_t kBufSize = 8192;
        alignas(FILE_NOTIFY_INFORMATION) char buf[kBufSize];
        OVERLAPPED ov{};
        ov.hEvent = impl->overlapEvent;
        while (impl->running)
        {
            ResetEvent(impl->overlapEvent);
            DWORD bytesReturned = 0;
            if (!ReadDirectoryChangesW(impl->dirHandle, buf, kBufSize, recursive ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytesReturned, &ov, nullptr)) break;

            HANDLE waits[2] = { impl->overlapEvent, impl->stopEvent };
            DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr == WAIT_OBJECT_0 + 1 || wr == WAIT_FAILED) break;
            if (!impl->running) break;

            if (!GetOverlappedResult(impl->dirHandle, &ov, &bytesReturned, FALSE)) break;
            if (bytesReturned == 0) continue; // 缓冲区溢出/事件丢失，重新发起读取

            char *p = buf;
            while (impl->running)
            {
                auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(p);
                std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
                const int len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), static_cast<int>(wname.size()),
                                                    nullptr, 0, nullptr, nullptr);
                std::string name(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), static_cast<int>(wname.size()),
                                    name.data(), len, nullptr, nullptr);
                FileEvent ev = FileEvent::Modified;
                switch (info->Action)
                {
                case FILE_ACTION_ADDED: ev = FileEvent::Created; break;
                case FILE_ACTION_REMOVED: ev = FileEvent::Deleted; break;
                default: ev = FileEvent::Modified; break;
                }
                if (impl->callback) impl->callback(name, ev);
                if (info->NextEntryOffset == 0) break;
                p += info->NextEntryOffset;
            }
        } });
#elif defined(__linux__)
    // 注意: inotify 本身不支持递归，此处 recursive 参数仅影响 MOVED 标志位。
    // 子目录变更通知需要调用方自行处理，或未来实现子目录 watch descriptor 表。
    pImpl_->inotifyFd = inotify_init1(IN_NONBLOCK);
    if (pImpl_->inotifyFd < 0)
    {
        pImpl_->running = false;
        return false;
    }
    const int wd = inotify_add_watch(pImpl_->inotifyFd, dirPath.c_str(),
                                     IN_CREATE | IN_MODIFY | IN_DELETE | (recursive ? IN_MOVED_FROM | IN_MOVED_TO : 0));
    if (wd < 0)
    {
        ::close(pImpl_->inotifyFd);
        pImpl_->running = false;
        return false;
    }

    auto *impl = pImpl_.get();
    pImpl_->worker = std::thread([impl]()
                                 {
        constexpr size_t kBufSize = 4096;
        alignas(struct inotify_event) char buf[kBufSize];
        struct pollfd pfd{};
        while (impl->running)
        {
            pfd.fd = impl->inotifyFd; pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) <= 0) continue;
            const ssize_t len = read(impl->inotifyFd, buf, kBufSize);
            if (len <= 0) break;
            for (char *p = buf; p < buf + len && impl->running; )
            {
                auto *event = reinterpret_cast<struct inotify_event *>(p);
                if (event->len > 0 && impl->callback)
                {
                    FileEvent ev = FileEvent::Modified;
                    if (event->mask & IN_CREATE) ev = FileEvent::Created;
                    else if (event->mask & IN_DELETE) ev = FileEvent::Deleted;
                    impl->callback(event->name, ev);
                }
                p += sizeof(struct inotify_event) + event->len;
            }
        } });
#else
    // macOS/其他平台: FileWatcher 暂不支持
    pImpl_->running = false;
    return false;
#endif
    return true;
}
