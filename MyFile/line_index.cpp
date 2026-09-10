#include "detail/internal.h"

using namespace My::detail;

// ==================== 行索引缓存 ====================

My::LineIndex::LineIndex(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    Fd fd(OpenRead(path));
    if (!fd) return;
    fileSize_ = FdSize(fd.get()).value_or(0);
    std::error_code ec;
    mtime_ = std::chrono::clock_cast<std::chrono::system_clock>(
        std::filesystem::last_write_time(path, ec));
    if (fileSize_ == 0) { valid_ = true; return; }
    offsets_.reserve(static_cast<size_t>(std::min<std::uintmax_t>(fileSize_ / 40 + 1, 1000000)));
    offsets_.push_back(0);
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    std::uintmax_t offset = 0;
    bool lastByteIsNl = false;
    bool readError = false;
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0) { readError = true; break; }
        if (r == 0) break;
        const char *p = buffer.get();
        size_t remaining = static_cast<size_t>(r);
        lastByteIsNl = (remaining > 0 && p[remaining - 1] == '\n');
        while (remaining > 0)
        {
            const char *nl = FindNl(p, remaining);
            if (!nl) break;
            offset += static_cast<std::uintmax_t>(nl - p) + 1;
            offsets_.push_back(offset);
            remaining -= static_cast<size_t>(nl - p) + 1;
            p = nl + 1;
        }
        if (remaining > 0) offset += remaining;
    }
    if (lastByteIsNl && !readError && offsets_.size() > 0) offsets_.pop_back();
    valid_ = !readError;
}

size_t My::LineIndex::lineCount() const { return offsets_.empty() ? 0 : offsets_.size(); }

std::optional<std::uintmax_t> My::LineIndex::lineStart(size_t lineNumber) const
{
    if (lineNumber == 0 || lineNumber > offsets_.size()) return std::nullopt;
    return offsets_[lineNumber - 1];
}

bool My::LineIndex::validate(std::string_view filename) const
{
    if (!valid_) return false;
    std::error_code ec;
    const auto path = ToPath(filename);
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz != fileSize_) return false;
    auto ft = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    return std::chrono::clock_cast<std::chrono::system_clock>(ft) == mtime_;
}

bool My::LineIndex::valid() const { return valid_; }
