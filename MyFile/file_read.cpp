#include "detail/internal.h"

using namespace My::detail;

// ==================== 文件读取 ====================

std::optional<std::string> My::File::readall(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    if (IsDirectory(path))
    {
        PrintError("readall", filename, "目标是目录，不是文件");
        return std::nullopt;
    }
    Fd fd(OpenRead(path));
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
    std::string data;
    if (size > 0)
        data.resize_and_overwrite(static_cast<size_t>(size), [&](char *buf, size_t n)
                                  { return ReadFull(fd.get(), buf, n); });
    if (data.size() != size)
    {
        PrintError("readall", filename, "读取不完整");
        return std::nullopt;
    }
    return data;
}

std::optional<std::vector<uint8_t>> My::File::readBytes(std::string_view filename)
{
    const std::filesystem::path path = ToPath(filename);
    if (IsDirectory(path))
    {
        PrintError("readBytes", filename, "目标是目录，不是文件");
        return std::nullopt;
    }
    Fd fd(OpenRead(path));
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
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && ReadFull(fd.get(), reinterpret_cast<char *>(buffer.data()), buffer.size()) != buffer.size())
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
                PrintError("readLine", filename, "读取文件失败");
            else
                PrintError("readLine", filename, "行号超出范围");
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
        return std::nullopt;
    const auto anchorOffset = index.lineStart(lineNumber);
    if (!anchorOffset)
        return std::nullopt;
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd)
        return std::nullopt;
    if (!SeekFd(fd.get(), static_cast<long long>(*anchorOffset)))
        return std::nullopt;

    // 稀疏模式：从锚点扫描到目标行
    const size_t granularity = index.granularity();
    if (granularity > 1)
    {
        const size_t anchorLine = ((lineNumber - 1) / granularity) * granularity + 1;
        const size_t linesToSkip = lineNumber - anchorLine;
        if (linesToSkip > 0)
        {
            const auto buffer = std::make_unique<char[]>(kIoBlockSize);
            size_t linesSkipped = 0;
            std::uintmax_t blockStart = *anchorOffset; // 当前块的文件偏移
            std::uintmax_t targetPos = blockStart;     // 目标行起始位置

            while (linesSkipped < linesToSkip)
            {
                const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
                if (r <= 0)
                    return std::nullopt;
                const char *p = buffer.get();
                size_t remaining = static_cast<size_t>(r);
                while (remaining > 0 && linesSkipped < linesToSkip)
                {
                    const char *nl = static_cast<const char *>(std::memchr(p, '\n', remaining));
                    if (!nl)
                        break;
                    ++linesSkipped;
                    const size_t advance = static_cast<size_t>(nl - p) + 1;
                    targetPos += advance;
                    remaining -= advance;
                    p = nl + 1;
                }
                blockStart += static_cast<std::uintmax_t>(r);
            }
            // seek 到目标行起点（跳行后 fd 在块尾，必须回退）
            if (!SeekFd(fd.get(), static_cast<long long>(targetPos)))
                return std::nullopt;
        }
    }

    // 读取目标行内容
    std::string result;
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0)
            return std::nullopt;
        if (r == 0)
            break;
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
    result.reserve(static_cast<size_t>(std::min<std::uintmax_t>(endLine - startLine + 1, 1024)));
    std::string line;
    size_t currentLine = 0;
    while (reader.next(line))
    {
        ++currentLine;
        if (currentLine >= startLine && currentLine <= endLine)
            result.push_back(std::move(line));
        if (currentLine >= endLine)
            break;
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
        return std::vector<std::string>{};
    for (size_t num : lineNumbers)
        if (num == 0)
        {
            PrintError("readLines", filename, "行号必须从1开始");
            return std::nullopt;
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
        sortedLines.emplace_back(lineNumbers[i], i);
    std::sort(sortedLines.begin(), sortedLines.end());
    size_t maxLine = sortedLines.back().first;
    std::vector<std::string> result(lineNumbers.size());
    std::string line;
    size_t currentLine = 0, nextIdx = 0;
    while (reader.next(line) && nextIdx < sortedLines.size())
    {
        ++currentLine;
        while (nextIdx < sortedLines.size() && sortedLines[nextIdx].first == currentLine)
        {
            const bool unique = (nextIdx + 1 >= sortedLines.size() || sortedLines[nextIdx + 1].first != currentLine);
            result[sortedLines[nextIdx].second] = unique ? std::move(line) : line;
            ++nextIdx;
        }
        if (currentLine >= maxLine)
            break;
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
        lines.push_back(std::move(line));
    if (reader.error())
    {
        PrintError("readAllLines", filename, "读取文件失败");
        return std::nullopt;
    }
    return lines;
}

std::optional<My::MemoryMappedFile> My::File::readMapped(std::string_view filename)
{
    My::MemoryMappedFile mmf(filename);
    if (!mmf.isMapped())
        return std::nullopt;
    return mmf;
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
    std::string_view line;
    size_t lineNumber = 0;
    while (reader.nextView(line))
    {
        ++lineNumber;
        if (!handler(lineNumber, line))
            return true;
    }
    if (reader.error())
    {
        PrintError("forEachLine", filename, "读取文件失败");
        return false;
    }
    return true;
}

// ==================== head / tail / readRange ====================

std::optional<std::vector<std::string>> My::File::head(std::string_view filename, size_t n)
{
    if (n == 0)
        return std::vector<std::string>{};
    LineReader reader;
    if (!reader.open(ToPath(filename)))
    {
        PrintError("head", filename, "无法打开文件");
        return std::nullopt;
    }
    std::vector<std::string> result;
    result.reserve((std::min)(n, size_t(1024)));
    std::string line;
    while (result.size() < n && reader.next(line))
        result.push_back(std::move(line));
    if (reader.error())
    {
        PrintError("head", filename, "读取文件失败");
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<std::string>> My::File::tail(std::string_view filename, size_t n)
{
    if (n == 0)
        return std::vector<std::string>{};
    const std::filesystem::path path = ToPath(filename);
    if (IsDirectory(path))
    {
        PrintError("tail", filename, "目标是目录，不是文件");
        return std::nullopt;
    }
    Fd fd(OpenRead(path));
    if (!fd)
    {
        PrintError("tail", filename, "无法打开文件");
        return std::nullopt;
    }
    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt)
    {
        PrintError("tail", filename, "无法获取文件大小");
        return std::nullopt;
    }
    const std::uintmax_t fileSize = *sizeOpt;
    if (fileSize == 0)
        return std::vector<std::string>{};

    // 从文件尾倒序块扫描，收集 n 个 \n 后停止
    std::vector<std::string> lines;
    const size_t blockSize = kIoBlockSize;
    auto buffer = std::make_unique<char[]>(blockSize);
    std::uintmax_t pos = fileSize;
    // 记录最后 n 行的起始偏移
    std::vector<std::uintmax_t> lineStarts;
    bool lastChunk = true;
    bool fileEndsWithNl = false;

    while (pos > 0 && lineStarts.size() <= n)
    {
        const size_t toRead = static_cast<size_t>(std::min<std::uintmax_t>(pos, blockSize));
        pos -= toRead;
        if (!SeekFd(fd.get(), static_cast<long long>(pos)))
        {
            PrintError("tail", filename, "定位失败");
            return std::nullopt;
        }
        if (ReadFull(fd.get(), buffer.get(), toRead) != toRead)
        {
            PrintError("tail", filename, "读取失败");
            return std::nullopt;
        }
        if (lastChunk)
        {
            fileEndsWithNl = (toRead > 0 && buffer[toRead - 1] == '\n');
            lastChunk = false;
        }
        // 从块尾向块头扫描 \n
        bool skippedTrailingNl = false;
        for (size_t i = toRead; i > 0;)
        {
            --i;
            if (buffer[i] == '\n')
            {
                // 文件以 \n 结尾时，末尾 \n 只是行终止符，不产生新行
                if (fileEndsWithNl && !skippedTrailingNl)
                {
                    skippedTrailingNl = true;
                    continue;
                }
                // 记录该行起始位置（\n 之后）
                lineStarts.push_back(pos + i + 1);
                if (lineStarts.size() > n)
                    break;
            }
        }
    }

    // 如果 pos == 0 且文件开头未被 \n 分隔符录入，补录首行
    if (pos == 0 && lineStarts.size() <= n)
    {
        bool hasZero = false;
        for (auto &s : lineStarts)
            if (s == 0)
            {
                hasZero = true;
                break;
            }
        if (!hasZero)
            lineStarts.push_back(0);
    }

    // 截取前 n 个（lineStarts 是倒序的）
    while (lineStarts.size() > n)
        lineStarts.pop_back();
    // 反转为正序
    std::reverse(lineStarts.begin(), lineStarts.end());

    // 按偏移顺序读取每行内容
    lines.reserve(lineStarts.size());
    const auto readBuf = std::make_unique<char[]>(blockSize);
    for (size_t i = 0; i < lineStarts.size(); ++i)
    {
        if (!SeekFd(fd.get(), static_cast<long long>(lineStarts[i])))
            return std::nullopt;
        std::string line;
        bool done = false;
        while (!done)
        {
            const auto r = ReadFd(fd.get(), readBuf.get(), blockSize);
            if (r < 0)
                return std::nullopt;
            if (r == 0)
                break;
            const char *nl = static_cast<const char *>(std::memchr(readBuf.get(), '\n', static_cast<size_t>(r)));
            if (nl)
            {
                line.append(readBuf.get(), static_cast<size_t>(nl - readBuf.get()));
                done = true;
            }
            else
                line.append(readBuf.get(), static_cast<size_t>(r));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<std::string> My::File::readRange(std::string_view filename, std::uintmax_t offset, size_t len)
{
    const std::filesystem::path path = ToPath(filename);
    if (IsDirectory(path))
    {
        PrintError("readRange", filename, "目标是目录，不是文件");
        return std::nullopt;
    }
    Fd fd(OpenRead(path));
    if (!fd)
    {
        PrintError("readRange", filename, "无法打开文件");
        return std::nullopt;
    }
    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt)
    {
        PrintError("readRange", filename, "无法获取文件大小");
        return std::nullopt;
    }
    const std::uintmax_t fileSize = *sizeOpt;
    if (offset > fileSize)
    {
        PrintError("readRange", filename, "偏移超出文件范围");
        return std::nullopt;
    }

    // len=0 表示读到末尾
    std::uintmax_t remaining = (len == 0) ? (fileSize - offset) : static_cast<std::uintmax_t>(len);
    if (offset + remaining > fileSize)
        remaining = fileSize - offset;
    if (remaining > std::string().max_size())
    {
        PrintError("readRange", filename, "读取范围过大");
        return std::nullopt;
    }

    if (!SeekFd(fd.get(), static_cast<long long>(offset)))
    {
        PrintError("readRange", filename, "定位失败");
        return std::nullopt;
    }
    std::string data(static_cast<size_t>(remaining), '\0');
    if (remaining > 0 && ReadFull(fd.get(), data.data(), data.size()) != data.size())
    {
        PrintError("readRange", filename, "读取不完整");
        return std::nullopt;
    }
    return data;
}
