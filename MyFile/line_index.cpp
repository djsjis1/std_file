#include "detail/internal.h"

using namespace My::detail;

namespace
{
    // FNV-1a 64-bit 哈希（轻量内容摘要，用于 validate 兜底校验）
    uint64_t Fnv1a64(const char *data, size_t len)
    {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; ++i)
        {
            hash ^= static_cast<uint8_t>(data[i]);
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    // 读取文件首尾各 64 字节的内容摘要
    void ComputeHeadTailHash(int fd, std::uintmax_t fileSize, uint64_t &headHash, uint64_t &tailHash)
    {
        headHash = 0;
        tailHash = 0;
        if (fileSize == 0)
            return;

        constexpr size_t kHashLen = 64;
        char buf[kHashLen];

        // 首部哈希
        if (!SeekFd(fd, 0))
            return;
        const size_t headLen = static_cast<size_t>(std::min<std::uintmax_t>(fileSize, kHashLen));
        if (ReadFull(fd, buf, headLen) == headLen)
            headHash = Fnv1a64(buf, headLen);

        // 尾部哈希（文件 <= 128 字节时首尾可能重叠，独立读取）
        if (fileSize > kHashLen)
        {
            const std::uintmax_t tailStart = fileSize - kHashLen;
            if (SeekFd(fd, static_cast<long long>(tailStart)))
            {
                if (ReadFull(fd, buf, kHashLen) == kHashLen)
                    tailHash = Fnv1a64(buf, kHashLen);
            }
        }
        else if (fileSize > 0)
        {
            // 文件 <= 64 字节，尾部就是整个文件内容（与首部相同）
            tailHash = headHash;
        }
    }
} // namespace

// ==================== 行索引缓存 ====================

My::LineIndex::LineIndex(std::string_view filename, size_t granularity)
    : granularity_(granularity < 1 ? 1 : granularity)
{
    const std::filesystem::path path = ToPath(filename);
    Fd fd(OpenRead(path));
    if (!fd)
        return;
    fileSize_ = FdSize(fd.get()).value_or(0);
    std::error_code ec;
    mtime_ = std::chrono::clock_cast<std::chrono::system_clock>(
        std::filesystem::last_write_time(path, ec));

    // 计算首尾内容摘要
    ComputeHeadTailHash(fd.get(), fileSize_, headHash_, tailHash_);

    if (fileSize_ == 0)
    {
        valid_ = true;
        return;
    }

    // 摘要计算后 fd 位置已变，重置到起点
    SeekFd(fd.get(), 0);

    // 预估锚点数量
    const size_t estimatedLines = static_cast<size_t>(std::min<std::uintmax_t>(fileSize_ / 40 + 1, 1000000));
    const size_t estimatedAnchors = (estimatedLines + granularity_ - 1) / granularity_;
    offsets_.reserve((std::min)(estimatedAnchors, size_t(1000000)));

    offsets_.push_back(0); // 第 1 行的锚点（偏移 0）
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    std::uintmax_t offset = 0;
    size_t currentLine = 1; // 当前已找到的换行数 + 1 = 下一个行号
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
            ++currentLine; // currentLine 现在是下一个行号
            // 每 granularity_ 行存一个锚点：行 1, G+1, 2G+1, ...
            if ((currentLine - 1) % granularity_ == 0)
                offsets_.push_back(offset);
            remaining -= static_cast<size_t>(nl - p) + 1;
            p = nl + 1;
        }
        if (remaining > 0)
            offset += remaining;
    }

    // 计算总行数
    // currentLine - 1 = 换行符总数
    // 如果末尾不是 \n，最后一行也要计入
    totalLines_ = currentLine - 1;
    if (!lastByteIsNl && !readError && totalLines_ >= 1)
        ++totalLines_;

    // 末尾多余锚点清理：文件以 \n 结尾且行数恰好是 granularity 倍数时，
    // 最后一个锚点指向 EOF（无实际行内容），移除
    if (lastByteIsNl && !offsets_.empty() && offsets_.back() >= fileSize_)
        offsets_.pop_back();

    valid_ = !readError;
}

size_t My::LineIndex::lineCount() const { return totalLines_; }

std::optional<std::uintmax_t> My::LineIndex::lineStart(size_t lineNumber) const
{
    if (lineNumber == 0 || lineNumber > totalLines_)
        return std::nullopt;

    if (granularity_ == 1)
    {
        // 稠密模式：直接 O(1) 寻址
        return offsets_[lineNumber - 1];
    }

    // 稀疏模式：找到 lineNumber 之前最近的锚点
    // 锚点 i 对应行号 i * granularity_ + 1
    const size_t anchorIdx = (lineNumber - 1) / granularity_;
    return offsets_[anchorIdx];
}

bool My::LineIndex::validate(std::string_view filename) const
{
    if (!valid_)
        return false;
    std::error_code ec;
    const auto path = ToPath(filename);

    // 1. 检查文件大小
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz != fileSize_)
        return false;

    // 2. 检查 mtime
    auto ft = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    const auto currentMtime = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    if (currentMtime != mtime_)
        return false;

    // 3. 内容摘要兜底（mtime 同秒修改也能检出）
    if (fileSize_ > 0)
    {
        Fd fd(OpenRead(path));
        if (!fd)
            return false;
        uint64_t headHash = 0, tailHash = 0;
        ComputeHeadTailHash(fd.get(), fileSize_, headHash, tailHash);
        if (headHash != headHash_ || tailHash != tailHash_)
            return false;
    }

    return true;
}

bool My::LineIndex::valid() const { return valid_; }
