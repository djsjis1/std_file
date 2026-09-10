#include "detail/internal.h"

using namespace My::detail;

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
