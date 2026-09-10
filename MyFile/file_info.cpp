#include "detail/internal.h"

using namespace My::detail;

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
            return std::nullopt;
        }
        if (r == 0)
            break;
        const auto n = static_cast<size_t>(r);
        totalRead += n;
        const char *p = buffer.get();
        const char *const end = buffer.get() + n;
        count += CountNl(p, static_cast<size_t>(end - p));
        lastByte = buffer[n - 1];
    }
    if (totalRead > 0 && lastByte != '\n')
        ++count;
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
    bool result = std::filesystem::copy_file(ToPath(src), ToPath(dest),
                                             std::filesystem::copy_options::overwrite_existing, ec);
    if (!result || ec)
    {
        PrintError("copy", src, ec ? ec.message().c_str() : "复制失败");
        return false;
    }
    return true;
}

bool My::File::copyLarge(std::string_view src, std::string_view dest, size_t /*bufferSize*/)
{
    std::error_code ec;
    const bool result = std::filesystem::copy_file(ToPath(src), ToPath(dest),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
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
    return true;
}

bool My::File::move(std::string_view src, std::string_view dest)
{
#ifdef _WIN32
    for (int attempt = 0;; ++attempt)
    {
        if (::MoveFileExW(ToPath(src).c_str(), ToPath(dest).c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            return true;
        if (attempt >= 4)
            break;
        ::Sleep(10 << attempt);
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
