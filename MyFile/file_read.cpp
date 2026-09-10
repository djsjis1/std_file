#include "detail/internal.h"

using namespace My::detail;

// ==================== 文件读取 ====================

std::optional<std::string> My::File::readall(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd) { PrintError("readall", filename, "无法打开文件"); return std::nullopt; }
    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt) { PrintError("readall", filename, "无法获取文件大小"); return std::nullopt; }
    const auto size = *sizeOpt;
    if (size > std::string().max_size()) { PrintError("readall", filename, "文件过大，超出内存上限"); return std::nullopt; }
    std::string data;
    if (size > 0)
        data.resize_and_overwrite(static_cast<size_t>(size), [&](char *buf, size_t n) { return ReadFull(fd.get(), buf, n); });
    if (data.size() != size) { PrintError("readall", filename, "读取不完整"); return std::nullopt; }
    return data;
}

std::optional<std::vector<uint8_t>> My::File::readBytes(std::string_view filename)
{
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd) { PrintError("readBytes", filename, "无法打开文件"); return std::nullopt; }
    const auto sizeOpt = FdSize(fd.get());
    if (!sizeOpt) { PrintError("readBytes", filename, "无法获取文件大小"); return std::nullopt; }
    const auto size = *sizeOpt;
    if (size > static_cast<std::uintmax_t>((std::numeric_limits<size_t>::max)())) { PrintError("readBytes", filename, "文件过大"); return std::nullopt; }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && ReadFull(fd.get(), reinterpret_cast<char *>(buffer.data()), buffer.size()) != buffer.size())
    { PrintError("readBytes", filename, "读取不完整"); return std::nullopt; }
    return buffer;
}

std::optional<std::string> My::File::readLine(std::string_view filename, size_t lineNumber)
{
    if (lineNumber == 0) { PrintError("readLine", filename, "行号必须从1开始"); return std::nullopt; }
    LineReader reader;
    if (!reader.open(ToPath(filename))) { PrintError("readLine", filename, "无法打开文件"); return std::nullopt; }
    std::string line;
    for (size_t currentLine = 1; currentLine <= lineNumber; ++currentLine)
    {
        if (!reader.next(line))
        {
            if (reader.error()) PrintError("readLine", filename, "读取文件失败");
            else PrintError("readLine", filename, "行号超出范围");
            return std::nullopt;
        }
    }
    return line;
}

std::optional<std::string> My::File::readLine(std::string_view filename, size_t lineNumber, const LineIndex &index)
{
    if (!index.valid()) return std::nullopt;
    if (lineNumber == 0) { PrintError("readLine", filename, "行号必须从1开始"); return std::nullopt; }
    if (lineNumber > index.lineCount()) { PrintError("readLine", filename, "行号超出范围"); return std::nullopt; }
    if (!index.validate(filename)) return std::nullopt;
    const auto offset = index.lineStart(lineNumber);
    if (!offset) return std::nullopt;
    Fd fd(OpenRead(ToPath(filename)));
    if (!fd) return std::nullopt;
    if (!SeekFd(fd.get(), static_cast<long long>(*offset))) return std::nullopt;
    std::string result;
    const auto buffer = std::make_unique<char[]>(kIoBlockSize);
    for (;;)
    {
        const auto r = ReadFd(fd.get(), buffer.get(), kIoBlockSize);
        if (r < 0) return std::nullopt;
        if (r == 0) break;
        const char *nl = static_cast<const char *>(std::memchr(buffer.get(), '\n', static_cast<size_t>(r)));
        if (nl) { result.append(buffer.get(), static_cast<size_t>(nl - buffer.get())); break; }
        result.append(buffer.get(), static_cast<size_t>(r));
    }
    return result;
}

std::optional<std::vector<std::string>> My::File::readLines(std::string_view filename, size_t startLine, size_t endLine)
{
    if (startLine == 0 || endLine == 0 || startLine > endLine) { PrintError("readLines", filename, "无效的行号范围"); return std::nullopt; }
    LineReader reader;
    if (!reader.open(ToPath(filename))) { PrintError("readLines", filename, "无法打开文件"); return std::nullopt; }
    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(std::min<std::uintmax_t>(endLine - startLine + 1, 1024)));
    std::string line;
    size_t currentLine = 0;
    while (reader.next(line))
    {
        ++currentLine;
        if (currentLine >= startLine && currentLine <= endLine) result.push_back(std::move(line));
        if (currentLine >= endLine) break;
    }
    if (reader.error()) { PrintError("readLines", filename, "读取文件失败"); return std::nullopt; }
    if (result.size() != endLine - startLine + 1) { PrintError("readLines", filename, "行号范围超出文件范围"); return std::nullopt; }
    return result;
}

std::optional<std::vector<std::string>> My::File::readLines(std::string_view filename, const std::vector<size_t> &lineNumbers)
{
    if (lineNumbers.empty()) return std::vector<std::string>{};
    for (size_t num : lineNumbers)
        if (num == 0) { PrintError("readLines", filename, "行号必须从1开始"); return std::nullopt; }
    LineReader reader;
    if (!reader.open(ToPath(filename))) { PrintError("readLines", filename, "无法打开文件"); return std::nullopt; }
    std::vector<std::pair<size_t, size_t>> sortedLines;
    sortedLines.reserve(lineNumbers.size());
    for (size_t i = 0; i < lineNumbers.size(); ++i) sortedLines.emplace_back(lineNumbers[i], i);
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
        if (currentLine >= maxLine) break;
    }
    if (reader.error()) { PrintError("readLines", filename, "读取文件失败"); return std::nullopt; }
    if (nextIdx < sortedLines.size()) { PrintError("readLines", filename, "部分行号超出文件范围"); return std::nullopt; }
    return result;
}

std::optional<std::vector<std::string>> My::File::readAllLines(std::string_view filename)
{
    LineReader reader;
    if (!reader.open(ToPath(filename))) { PrintError("readAllLines", filename, "无法打开文件"); return std::nullopt; }
    std::vector<std::string> lines;
    std::string line;
    while (reader.next(line)) lines.push_back(std::move(line));
    if (reader.error()) { PrintError("readAllLines", filename, "读取文件失败"); return std::nullopt; }
    return lines;
}

std::optional<My::MemoryMappedFile> My::File::readMapped(std::string_view filename)
{
    My::MemoryMappedFile mmf(filename);
    if (!mmf.isMapped()) return std::nullopt;
    return mmf;
}

bool My::File::forEachLine(std::string_view filename,
                           const std::function<bool(size_t, std::string_view)> &handler)
{
    LineReader reader;
    if (!reader.open(ToPath(filename))) { PrintError("forEachLine", filename, "无法打开文件"); return false; }
    std::string_view line;
    size_t lineNumber = 0;
    while (reader.nextView(line))
    {
        ++lineNumber;
        if (!handler(lineNumber, line)) return true;
    }
    if (reader.error()) { PrintError("forEachLine", filename, "读取文件失败"); return false; }
    return true;
}
