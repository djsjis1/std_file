#include "detail/internal.h"

using namespace My::detail;

namespace
{
    // 流式查找第 lineNumber 行（1 起）的起始字节偏移，内存 O(1)。
    std::optional<std::uintmax_t> FindLineStartFdImpl(int fd, size_t lineNumber)
    {
        if (lineNumber == 0) return std::nullopt;
        if (lineNumber == 1) return 0;
        if (!SeekFd(fd, 0)) return std::nullopt;

        const auto buffer = std::make_unique<char[]>(kIoBlockSize);
        std::uintmax_t blockOffset = 0;
        size_t newlinesNeeded = lineNumber - 1;
        for (;;)
        {
            const auto r = ReadFd(fd, buffer.get(), kIoBlockSize);
            if (r <= 0) return std::nullopt;
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
                return blockOffset + static_cast<std::uintmax_t>(p - buffer.get());
            blockOffset += n;
        }
    }

    // 流式范围重写：把原文件 [start, end) 区间替换为 replacement
    bool RewriteRangeImpl(const std::filesystem::path &path, std::uintmax_t start, std::uintmax_t end,
                          std::string_view replacement, std::string_view apiName, std::string_view displayName)
    {
        Fd in(OpenRead(path));
        if (!in) { PrintError(apiName, displayName, "无法打开文件"); return false; }
        const auto sizeOpt = FdSize(in.get());
        if (!sizeOpt) { PrintError(apiName, displayName, "无法获取文件大小"); return false; }
        const std::uintmax_t fileSize = *sizeOpt;
        if (start > end || end > fileSize) { PrintError(apiName, displayName, "编辑区间超出文件范围"); return false; }

        const std::filesystem::path tempPath = MakeTempPath(path);
        Fd out(OpenWriteTrunc(tempPath));
        if (!out) { PrintError(apiName, displayName, "无法创建临时文件"); return false; }
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
            if (r <= 0) return fail("读取原文件失败");
            if (!WriteFull(out.get(), buffer.get(), static_cast<size_t>(r))) return fail("写入临时文件失败");
            remaining -= static_cast<std::uintmax_t>(r);
        }

        // 2) 写入替换内容
        if (!replacement.empty() && !WriteFull(out.get(), replacement.data(), replacement.size()))
            return fail("写入临时文件失败");

        // 3) 复制 [end, fileSize) 剩余部分
        if (end < fileSize)
        {
            if (!SeekFd(in.get(), static_cast<long long>(end))) return fail("定位原文件失败");
            for (;;)
            {
                const auto r = ReadFd(in.get(), buffer.get(), kIoBlockSize);
                if (r < 0) return fail("读取原文件失败");
                if (r == 0) break;
                if (!WriteFull(out.get(), buffer.get(), static_cast<size_t>(r))) return fail("写入临时文件失败");
            }
        }

        if (CloseFd(out.release()) != 0) return fail("关闭临时文件失败");
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

// 导出到 My::detail 命名空间供其他 TU 使用
std::optional<std::uintmax_t> My::detail::FindLineStartFd(int fd, size_t lineNumber)
{
    return FindLineStartFdImpl(fd, lineNumber);
}

bool My::detail::RewriteRange(const std::filesystem::path &path, std::uintmax_t start, std::uintmax_t end,
                              std::string_view replacement, std::string_view apiName, std::string_view displayName)
{
    return RewriteRangeImpl(path, start, end, replacement, apiName, displayName);
}

// ==================== 插入方法 ====================

bool My::File::insertAt(std::string_view filename, size_t position, std::string_view content)
{
    return RewriteRange(ToPath(filename), position, position, content, "insertAt", filename);
}

bool My::File::insertBeforeLine(std::string_view filename, size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0) { PrintError("insertBeforeLine", filename, "行号必须从1开始"); return false; }
    const std::filesystem::path path = ToPath(filename);
    std::optional<std::uintmax_t> pos;
    {
        Fd in(OpenRead(path));
        if (!in) { PrintError("insertBeforeLine", filename, "无法打开文件"); return false; }
        pos = FindLineStartFd(in.get(), lineNumber);
    }
    if (!pos) { PrintError("insertBeforeLine", filename, "行号超出范围"); return false; }
    return RewriteRange(path, *pos, *pos, content, "insertBeforeLine", filename);
}

bool My::File::insertAfterLine(std::string_view filename, size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0) { PrintError("insertAfterLine", filename, "行号必须从1开始"); return false; }
    const std::filesystem::path path = ToPath(filename);
    std::optional<std::uintmax_t> pos;
    {
        Fd in(OpenRead(path));
        if (!in) { PrintError("insertAfterLine", filename, "无法打开文件"); return false; }
        pos = FindLineStartFd(in.get(), lineNumber + 1);
        if (!pos)
        {
            if (!FindLineStartFd(in.get(), lineNumber))
            {
                PrintError("insertAfterLine", filename, "行号超出范围");
                return false;
            }
            pos = FdSize(in.get());
            if (!pos) { PrintError("insertAfterLine", filename, "无法获取文件大小"); return false; }
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
        if (!in) { PrintError("deleteLines", filename, "无法打开文件"); return false; }
        startPos = FindLineStartFd(in.get(), startLine);
        if (!startPos) { PrintError("deleteLines", filename, "起始行号超出范围"); return false; }
        endPos = FindLineStartFd(in.get(), endLine + 1);
        if (!endPos)
        {
            if (!FindLineStartFd(in.get(), endLine))
            {
                PrintError("deleteLines", filename, "结束行号超出范围");
                return false;
            }
            endPos = FdSize(in.get());
            if (!endPos) { PrintError("deleteLines", filename, "无法获取文件大小"); return false; }
        }
    }
    return RewriteRange(path, *startPos, *endPos, std::string_view{}, "deleteLines", filename);
}
