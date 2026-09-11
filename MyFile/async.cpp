#include "detail/internal.h"

using namespace My::detail;

// ==================== 线程池实现 ====================

My::detail::ThreadPool::ThreadPool(size_t threads)
{
    for (size_t i = 0; i < threads; ++i)
    {
        workers_.emplace_back([this]
                              {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            } });
    }
}

My::detail::ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_)
        w.join();
}

ThreadPool &My::detail::GetPool()
{
    static ThreadPool pool((std::max)(2u, std::thread::hardware_concurrency()));
    return pool;
}

// ==================== 异步 IO ====================

std::future<std::optional<std::string>> My::File::asyncReadall(std::string_view filename)
{
    std::string fn(filename);
    return GetPool().Submit([fn]()
                            { return readall(fn); });
}

std::future<bool> My::File::asyncWriteAll(std::string_view filename, std::string_view content)
{
    std::string fn(filename), ct(content);
    return GetPool().Submit([fn = std::move(fn), ct = std::move(ct)]()
                            { return writeAll(fn, ct); });
}
