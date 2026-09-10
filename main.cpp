#include "file.h"
#include <iostream>
#include <iomanip> // std::put_time
#include <chrono>
#include <ctime> // std::tm / time_t

int main(int argc, char const *argv[])
{
    // 使用相对路径，方便跨平台测试
    My::File::write("main.txt").write("Hello World").commit();
    My::File::insert("main.txt").writeLine("asdw").commit();

    auto data = My::File::readall("main.txt");

    if (data.has_value())
    {
        std::cout << data.value() << std::endl;
    }

    // 获取文件信息
    auto info = My::File::lastModified("main.txt");
    if (info.has_value())
    {
        const auto t = std::chrono::system_clock::to_time_t(info.value());
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t); // 线程安全版本
#else
        localtime_r(&t, &tmBuf);
#endif
        std::cout << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << '\n';
    }

    // ===== 新功能演示 =====

    // 1. 原子写入：避免写入中途崩溃损坏原文件
    My::File::writeAllAtomic("atomic.txt", "atomic content\n");

    // 2. 流式逐行遍历（大文件友好）
    My::File::forEachLine("main.txt", [](size_t num, std::string_view line)
                          {
        std::cout << "line " << num << ": " << line << '\n';
        return true; });

    // 3. 一次性读取全部行
    if (auto lines = My::File::readAllLines("main.txt"))
    {
        std::cout << "total lines: " << lines->size() << '\n';
    }

    // 4. 移动/重命名
    My::File::move("atomic.txt", "atomic_moved.txt");

    // 5. touch（不存在则创建空文件）
    My::File::touch("touched.txt");

    // 6. 递归删除目录
    My::File::createDirectories("tmp_dir/sub");
    My::File::removeDirectory("tmp_dir");
}
