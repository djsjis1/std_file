#include "file.h"
#include <iostream>


int main(int argc, char const *argv[])
{
// FileWatcher 监视的是【目录】，不是单个文件
// 回调签名：void(std::string_view path, FileEvent event)
My::FileWatcher watcher;
watcher.start("D:\\code\\.std\\file", [](std::string_view path, My::FileEvent event) {

    std::cout << "File changed: " << path << std::endl;
}, true /* recursive */);


My::File::insert("D:\\code\\.std\\file\\test.txt")
    .insertBeforeLine(5, "这是插入的内容\n")
    .commit();

// 主线程等待，否则 main 直接退出、watcher 析构就停止监听了
std::cin.get();
watcher.stop(); // 停止监听 
}
