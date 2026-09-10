#include "detail/internal.h"

using namespace My::detail;

// ==================== 直接写入 ====================

bool My::File::writeAll(std::string_view filename, std::string_view content)
{
    Fd fd(OpenWriteTrunc(ToPath(filename)));
    if (!fd) { PrintError("writeAll", filename, "无法创建文件"); return false; }
    if (!WriteFull(fd.get(), content.data(), content.size())) { PrintError("writeAll", filename, "写入失败"); return false; }
    if (CloseFd(fd.release()) != 0) { PrintError("writeAll", filename, "关闭文件失败"); return false; }
    return true;
}

bool My::File::writeBytes(std::string_view filename, const std::vector<uint8_t> &data)
{
    Fd fd(OpenWriteTrunc(ToPath(filename)));
    if (!fd) { PrintError("writeBytes", filename, "无法创建文件"); return false; }
    if (!WriteFull(fd.get(), reinterpret_cast<const char *>(data.data()), data.size())) { PrintError("writeBytes", filename, "写入失败"); return false; }
    if (CloseFd(fd.release()) != 0) { PrintError("writeBytes", filename, "关闭文件失败"); return false; }
    return true;
}

bool My::File::appendAll(std::string_view filename, std::string_view content)
{
    Fd fd(OpenWriteAppend(ToPath(filename)));
    if (!fd) { PrintError("appendAll", filename, "无法打开文件"); return false; }
    if (!WriteFull(fd.get(), content.data(), content.size())) { PrintError("appendAll", filename, "写入失败"); return false; }
    if (CloseFd(fd.release()) != 0) { PrintError("appendAll", filename, "关闭文件失败"); return false; }
    return true;
}

bool My::File::writeAllAtomic(std::string_view filename, std::string_view content, bool durable)
{
    const std::filesystem::path destPath = ToPath(filename);
    const std::filesystem::path tempPath = MakeTempPath(destPath);

    Fd fd(OpenWriteTrunc(tempPath));
    if (!fd) { PrintError("writeAllAtomic", filename, "无法创建临时文件"); return false; }
    if (!WriteFull(fd.get(), content.data(), content.size()))
    {
        PrintError("writeAllAtomic", filename, "写入临时文件失败");
        std::error_code ignore;
        std::filesystem::remove(tempPath, ignore);
        return false;
    }
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
