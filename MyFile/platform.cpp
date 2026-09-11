#include "detail/internal.h"

namespace My::detail
{
    // ==================== 底层文件 IO 实现 ====================
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
            return std::nullopt;
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
            return std::nullopt;
        return static_cast<std::uintmax_t>(st.st_size);
    }
#endif

    size_t ReadFull(int fd, char *buf, size_t n)
    {
        size_t total = 0;
        while (total < n)
        {
            const auto r = ReadFd(fd, buf + total, n - total);
            if (r <= 0)
                break;
            total += static_cast<size_t>(r);
        }
        return total;
    }

    bool WriteFull(int fd, const char *buf, size_t n)
    {
        size_t total = 0;
        while (total < n)
        {
            const auto w = WriteFd(fd, buf + total, n - total);
            if (w <= 0)
                return false;
            total += static_cast<size_t>(w);
        }
        return true;
    }

    bool SeekFd(int fd, long long offset)
    {
#ifdef _WIN32
        return _lseeki64(fd, offset, SEEK_SET) == offset;
#else
        return ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset);
#endif
    }

    bool FlushToDisk(int fd)
    {
#ifdef _WIN32
        const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        return handle != INVALID_HANDLE_VALUE && ::FlushFileBuffers(handle) != 0;
#else
        return ::fsync(fd) == 0;
#endif
    }

    std::filesystem::path ToPath(std::string_view s)
    {
#ifdef _WIN32
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
        return std::filesystem::path(s);
#endif
    }

    bool IsDirectory(const std::filesystem::path &p)
    {
#ifdef _WIN32
        const DWORD attr = ::GetFileAttributesW(p.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
        struct stat st;
        return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }

    std::chrono::system_clock::time_point FileClockToSystem(std::filesystem::file_time_type ft)
    {
        // 跨编译器兼容方案：用两个 clock 的 now() tick 差值做 epoch 偏移
        // 不依赖 C++20 clock_cast（GCC 13 / Clang 18 的 libstdc++ 尚未实现）
        // 精度取决于 duration 分辨率，对文件 mtime 比较完全够用
        using namespace std::chrono;
        const auto fileNow = std::filesystem::file_time_type::clock::now();
        const auto sysNow = system_clock::now();
        // 用各自 duration 的 tick 数计算 epoch 偏移（避免跨 clock 减法）
        const auto offsetTicks = sysNow.time_since_epoch().count()
                               - fileNow.time_since_epoch().count();
        // 将 ft 转为 system_clock 的 duration 分辨率
        const auto ftTicks = duration_cast<system_clock::duration>(
            ft.time_since_epoch()).count();
        return system_clock::time_point(
            system_clock::duration(ftTicks + offsetTicks));
    }

    // ==================== 错误报告 ====================
    void DefaultErrorHandler(std::string_view func, std::string_view filename, std::string_view message)
    {
        std::cerr << "[File::" << func << "] " << filename << ": " << message << '\n';
    }

    My::ErrorHandler g_errorHandler = DefaultErrorHandler;

    void PrintError(std::string_view func, std::string_view filename, const std::error_code &ec)
    {
        PrintError(func, filename, ec.message());
    }

    void PrintError(std::string_view func, std::string_view filename, std::string_view message)
    {
        if (g_errorHandler)
            g_errorHandler(func, filename, message);
    }

    void PrintError(std::string_view func, const std::filesystem::path &filename, std::string_view message)
    {
        const std::u8string u8name = filename.u8string();
        PrintError(func, std::string_view(reinterpret_cast<const char *>(u8name.data()), u8name.size()), message);
    }

    // ==================== 辅助工具 ====================
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

    bool ReplaceAtomically(const std::filesystem::path &tempPath, const std::filesystem::path &destPath,
                           bool writeThrough, std::string_view apiName, std::string_view displayName)
    {
#ifdef _WIN32
        const DWORD flags = MOVEFILE_REPLACE_EXISTING | (writeThrough ? MOVEFILE_WRITE_THROUGH : 0);
        for (int attempt = 0;; ++attempt)
        {
            if (::MoveFileExW(tempPath.c_str(), destPath.c_str(), flags))
                return true;
            if (attempt >= 4)
                break;
            ::Sleep(10 << attempt);
        }
#else
        std::error_code ec;
        std::filesystem::rename(tempPath, destPath, ec);
        if (!ec)
            return true;
#endif
        PrintError(apiName, displayName, "原子替换失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }

    // ==================== 行定位（内存版） ====================
    std::optional<size_t> FindLineStart(std::string_view data, size_t lineNumber)
    {
        if (lineNumber == 0)
            return std::nullopt;
        if (lineNumber == 1)
            return 0;
        size_t pos = 0;
        for (size_t line = 1; line < lineNumber; ++line)
        {
            const auto newline = data.find('\n', pos);
            if (newline == std::string_view::npos)
                return std::nullopt;
            pos = newline + 1;
        }
        return pos;
    }

    // ==================== SIMD 换行扫描分派定义 ====================
#if MYFILE_HAS_SIMD
    const int kSimdAvx2 = 2, kSimdSse2 = 1, kSimdScalar = 0;

    int DetectSimdLevel()
    {
#ifdef __AVX2__
        return kSimdAvx2;
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

    // ---- FindNl ----
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

    // ---- CountNl ----
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

#else  // 非 x86
    FindNlFn FindNl = [](const char *data, size_t len) -> const char *
    {
        return static_cast<const char *>(std::memchr(data, '\n', len));
    };

    CountNlFn CountNl = [](const char *data, size_t len) -> size_t
    {
        size_t count = 0;
        const char *p = data;
        while ((p = static_cast<const char *>(std::memchr(p, '\n', len - (p - data)))) != nullptr)
        {
            ++count;
            ++p;
        }
        return count;
    };
#endif // MYFILE_HAS_SIMD

} // namespace My::detail
