#include "detail/internal.h"

using namespace My::detail;

// ==================== 链式写入构建器 ====================

class My::File::Writer::Impl
{
public:
    std::filesystem::path filename;
    bool appendMode;
    std::string buffer;
    bool poisoned = false;

    Impl(std::string_view fname, bool append) : filename(ToPath(fname)), appendMode(append) {}
};

My::File::Writer::Writer(std::string_view filename, bool appendMode)
    : pImpl(std::make_unique<Impl>(filename, appendMode)) {}

My::File::Writer::~Writer()
{
    if (autoCommit_ && pImpl && !pImpl->buffer.empty())
    {
        if (!commit())
        {
            const std::u8string u8name = pImpl->filename.u8string();
            PrintError("Writer::~Writer",
                       std::string_view(reinterpret_cast<const char *>(u8name.data()), u8name.size()),
                       "自动提交失败");
        }
    }
}

My::File::Writer::Writer(Writer &&) noexcept = default;
My::File::Writer &My::File::Writer::operator=(Writer &&) noexcept = default;

My::File::Writer &My::File::Writer::write(std::string_view content) { pImpl->buffer.append(content); return *this; }
My::File::Writer &My::File::Writer::writeLine(std::string_view content) { pImpl->buffer.append(content); pImpl->buffer += '\n'; return *this; }
My::File::Writer &My::File::Writer::writeBytes(const std::vector<uint8_t> &data) { pImpl->buffer.append(reinterpret_cast<const char *>(data.data()), data.size()); return *this; }

bool My::File::Writer::commit()
{
    if (pImpl->poisoned)
    {
        PrintError("Writer::commit", pImpl->filename, "Writer 已污染(文件存在但读取失败)，拒绝提交以防数据丢失");
        return false;
    }
    Fd fd(pImpl->appendMode ? OpenWriteAppend(pImpl->filename) : OpenWriteTrunc(pImpl->filename));
    if (!fd) { PrintError("Writer::commit", pImpl->filename, "无法打开文件"); return false; }
    if (!WriteFull(fd.get(), pImpl->buffer.data(), pImpl->buffer.size()))
    {
        PrintError("Writer::commit", pImpl->filename, "写入失败");
        return false;
    }
    if (CloseFd(fd.release()) != 0) { PrintError("Writer::commit", pImpl->filename, "关闭文件失败"); return false; }
    pImpl->buffer.clear();
    return true;
}

std::string_view My::File::Writer::view() const { return pImpl->buffer; }
std::string My::File::Writer::str() const { return pImpl->buffer; }
My::File::Writer &My::File::Writer::clear() { pImpl->buffer.clear(); return *this; }
My::File::Writer &My::File::Writer::reserve(size_t size) { pImpl->buffer.reserve(size); return *this; }

My::File::Writer &My::File::Writer::insertAt(size_t position, std::string_view content)
{
    if (position > pImpl->buffer.size())
        PrintError("Writer::insertAt", pImpl->filename, "插入位置超出缓冲区范围");
    else
        pImpl->buffer.insert(position, content);
    return *this;
}

My::File::Writer &My::File::Writer::insertBeforeLine(size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0) { PrintError("Writer::insertBeforeLine", pImpl->filename, "行号必须从1开始"); return *this; }
    if (const auto pos = FindLineStart(pImpl->buffer, lineNumber))
        pImpl->buffer.insert(*pos, content);
    return *this;
}

My::File::Writer &My::File::Writer::insertAfterLine(size_t lineNumber, std::string_view content)
{
    if (lineNumber == 0) { PrintError("Writer::insertAfterLine", pImpl->filename, "行号必须从1开始"); return *this; }
    auto pos = FindLineStart(pImpl->buffer, lineNumber + 1);
    if (!pos)
    {
        if (!FindLineStart(pImpl->buffer, lineNumber)) return *this;
        pos = pImpl->buffer.size();
    }
    pImpl->buffer.insert(*pos, content);
    return *this;
}

// 静态工厂方法
My::File::Writer My::File::write(std::string_view filename, bool appendMode) { return Writer(filename, appendMode); }

My::File::Writer My::File::insert(std::string_view filename)
{
    Writer writer(filename, false);
    if (auto content = readall(filename))
        writer.pImpl->buffer = std::move(*content);
    else if (exists(filename))
    {
        writer.pImpl->poisoned = true;
        PrintError("insert", filename, "文件存在但读取失败，Writer 已标记为不可提交");
    }
    return writer;
}

My::File::Writer &My::File::Writer::setAutoCommit(bool enable) { autoCommit_ = enable; return *this; }
bool My::File::Writer::isPoisoned() const { return pImpl->poisoned; }
