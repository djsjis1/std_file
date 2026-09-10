#include "detail/internal.h"

using namespace My::detail;

// ==================== 文件监听 ====================

struct My::FileWatcher::Impl
{
    std::thread worker;
    std::atomic<bool> running{false};
    My::FileWatcher::Callback callback;
#ifdef _WIN32
    HANDLE dirHandle = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE overlapEvent = nullptr;
#elif defined(__linux__)
    int inotifyFd = -1;
#endif
    ~Impl()
    {
        if (running && worker.joinable())
            worker.join();
    }
};

My::FileWatcher::FileWatcher() : pImpl_(std::make_unique<Impl>()) {}
My::FileWatcher::~FileWatcher() { stop(); }

bool My::FileWatcher::isWatching() const { return pImpl_ && pImpl_->running; }

void My::FileWatcher::stop()
{
    if (!pImpl_ || !pImpl_->running) return;
    pImpl_->running = false;
#ifdef _WIN32
    if (pImpl_->stopEvent) SetEvent(pImpl_->stopEvent);
    if (pImpl_->dirHandle != INVALID_HANDLE_VALUE) CancelIoEx(pImpl_->dirHandle, nullptr);
#elif defined(__linux__)
    if (pImpl_->inotifyFd >= 0) { ::close(pImpl_->inotifyFd); pImpl_->inotifyFd = -1; }
#endif
    if (pImpl_->worker.joinable()) pImpl_->worker.join();
#ifdef _WIN32
    if (pImpl_->dirHandle != INVALID_HANDLE_VALUE) { CloseHandle(pImpl_->dirHandle); pImpl_->dirHandle = INVALID_HANDLE_VALUE; }
    if (pImpl_->stopEvent) { CloseHandle(pImpl_->stopEvent); pImpl_->stopEvent = nullptr; }
    if (pImpl_->overlapEvent) { CloseHandle(pImpl_->overlapEvent); pImpl_->overlapEvent = nullptr; }
#endif
}

bool My::FileWatcher::start(std::string_view path, Callback callback, bool recursive)
{
    if (isWatching()) return false;
    pImpl_->callback = std::move(callback);
    pImpl_->running = true;
    const std::filesystem::path dirPath = ToPath(path);

#if defined(_WIN32)
    pImpl_->dirHandle = CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (pImpl_->dirHandle == INVALID_HANDLE_VALUE) { pImpl_->running = false; return false; }
    pImpl_->stopEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pImpl_->overlapEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    auto *impl = pImpl_.get();
    pImpl_->worker = std::thread([impl, recursive]() {
        constexpr size_t kBufSize = 8192;
        alignas(FILE_NOTIFY_INFORMATION) char buf[kBufSize];
        OVERLAPPED ov{};
        ov.hEvent = impl->overlapEvent;
        while (impl->running)
        {
            ResetEvent(impl->overlapEvent);
            DWORD bytesReturned = 0;
            if (!ReadDirectoryChangesW(impl->dirHandle, buf, kBufSize, recursive ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytesReturned, &ov, nullptr)) break;

            HANDLE waits[2] = { impl->overlapEvent, impl->stopEvent };
            DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wr == WAIT_OBJECT_0 + 1 || wr == WAIT_FAILED) break;
            if (!impl->running) break;

            if (!GetOverlappedResult(impl->dirHandle, &ov, &bytesReturned, FALSE)) break;
            if (bytesReturned == 0) continue;

            char *p = buf;
            while (impl->running)
            {
                auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(p);
                std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
                const int len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), static_cast<int>(wname.size()),
                                                    nullptr, 0, nullptr, nullptr);
                std::string name(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), static_cast<int>(wname.size()),
                                    name.data(), len, nullptr, nullptr);
                FileEvent ev = FileEvent::Modified;
                switch (info->Action)
                {
                case FILE_ACTION_ADDED: ev = FileEvent::Created; break;
                case FILE_ACTION_REMOVED: ev = FileEvent::Deleted; break;
                default: ev = FileEvent::Modified; break;
                }
                if (impl->callback) impl->callback(name, ev);
                if (info->NextEntryOffset == 0) break;
                p += info->NextEntryOffset;
            }
        } });
#elif defined(__linux__)
    pImpl_->inotifyFd = inotify_init1(IN_NONBLOCK);
    if (pImpl_->inotifyFd < 0) { pImpl_->running = false; return false; }
    const int wd = inotify_add_watch(pImpl_->inotifyFd, dirPath.c_str(),
                                     IN_CREATE | IN_MODIFY | IN_DELETE | (recursive ? IN_MOVED_FROM | IN_MOVED_TO : 0));
    if (wd < 0) { ::close(pImpl_->inotifyFd); pImpl_->running = false; return false; }

    auto *impl = pImpl_.get();
    pImpl_->worker = std::thread([impl]() {
        constexpr size_t kBufSize = 4096;
        alignas(struct inotify_event) char buf[kBufSize];
        struct pollfd pfd{};
        while (impl->running)
        {
            pfd.fd = impl->inotifyFd; pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) <= 0) continue;
            const ssize_t len = read(impl->inotifyFd, buf, kBufSize);
            if (len <= 0) break;
            for (char *p = buf; p < buf + len && impl->running; )
            {
                auto *event = reinterpret_cast<struct inotify_event *>(p);
                if (event->len > 0 && impl->callback)
                {
                    FileEvent ev = FileEvent::Modified;
                    if (event->mask & IN_CREATE) ev = FileEvent::Created;
                    else if (event->mask & IN_DELETE) ev = FileEvent::Deleted;
                    impl->callback(event->name, ev);
                }
                p += sizeof(struct inotify_event) + event->len;
            }
        } });
#else
    pImpl_->running = false;
    return false;
#endif
    return true;
}
