# My::File —— 跨平台文件操作库

基于 C++23 的轻量文件操作库,提供文件信息查询、读写、行操作、目录管理、原子写入等能力。仅依赖标准库,支持 Windows(MSVC)/ Linux / macOS。

## 引入到你的项目

库位于 [MyFile/](MyFile/) 目录,自带 CMakeLists,把整个 `MyFile` 文件夹拷贝到你的项目即可一键引入:

```cmake
# 方式 1:add_subdirectory(推荐)
add_subdirectory(path/to/MyFile)
target_link_libraries(your_target PRIVATE MyFile::MyFile)

# 方式 2:FetchContent
include(FetchContent)
FetchContent_Declare(MyFile SOURCE_DIR path/to/MyFile)
FetchContent_MakeAvailable(MyFile)
target_link_libraries(your_target PRIVATE MyFile::MyFile)

# 方式 3:安装后 find_package
cmake --install build --prefix <prefix>
# 在你的 CMakeLists 中:
find_package(MyFile CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE MyFile::MyFile)
```

- 头文件:`#include "file.h"`(include 路径已随目标自动传播,无需手动配置)
- C++23 要求已通过 `target_compile_features(PUBLIC cxx_std_23)` 自动传播
- MSVC 的 `/utf-8` 已在库内部配置,中文注释不会引起编码问题

## 快速开始

```cpp
#include "file.h"

// 写文件(链式)
My::File::write("a.txt").writeLine("hello").writeLine("world").commit();

// 读文件
auto content = My::File::readall("a.txt");           // 整个文件 -> std::string
auto lines   = My::File::readAllLines("a.txt");      // 所有行
auto line    = My::File::readLine("a.txt", 2);       // 第 2 行

// 大文件逐行流式处理(不一次性载入内存,回调返回 false 可提前终止)
My::File::forEachLine("big.log", [](size_t n, std::string_view line) {
    // 处理第 n 行
    return true;
});

// 原子写入(先写临时文件再原子替换,崩溃不损坏原文件)
My::File::writeAllAtomic("config.json", newContent);

// 目录与移动
My::File::createDirectories("a/b/c");
My::File::removeDirectory("a");                       // 递归删除,幂等
My::File::move("old.txt", "new.txt");                 // 移动/重命名
```

## API 一览

### 文件信息

| 函数 | 说明 |
|---|---|
| `exists(filename)` | 文件是否存在 |
| `size(filename)` | 文件大小(字节,`uintmax_t`) |
| `lastModified(filename)` | 最后修改时间(`system_clock::time_point`) |
| `lineCount(filename)` | 文件总行数(分块扫描,GB/s 级) |

### 文件管理

| 函数 | 说明 |
|---|---|
| `remove(filename)` | 删除文件 |
| `copy(src, dest)` | 复制文件(委托 `std::filesystem::copy_file`,平台最优) |
| `copyLarge(src, dest, bufferSize)` | 同 copy,`bufferSize` 仅为接口兼容保留 |
| `createDirectory(path)` / `createDirectories(path)` | 创建目录 / 递归创建 |
| `removeDirectory(path)` | 递归删除目录及内容(目录不存在也视为成功) |
| `move(src, dest)` | 移动/重命名(Windows 下可覆盖目标) |
| `touch(filename)` | 更新 mtime,不存在则创建空文件 |

### 读取

| 函数 | 说明 |
|---|---|
| `readall(filename)` | 读取整个文件 |
| `readBytes(filename)` | 读取整个文件(二进制) |
| `readLine(filename, n)` | 读取第 n 行(1 起) |
| `readLines(filename, start, end)` | 读取连续行区间 |
| `readLines(filename, {1,5,10})` | 读取非连续多行(按原顺序返回) |
| `readAllLines(filename)` | 读取全部行 |
| `forEachLine(filename, fn)` | 流式逐行遍历,大文件友好 |

### 写入

| 函数 | 说明 |
|---|---|
| `write(filename)` | 返回链式 `Writer`(默认覆盖,`appendMode=true` 追加) |
| `insert(filename)` | 返回 `Writer`,预加载原文件内容供链式插入 |
| `writeAll(filename, content)` | 覆盖写 |
| `writeBytes(filename, data)` | 覆盖写二进制 |
| `appendAll(filename, content)` | 追加写 |
| `writeAllAtomic(filename, content, durable=true)` | 原子覆盖写;`durable=true` 内容先刷盘再替换,掉电安全,`false` 仅保留 rename 原子性 |
| `insertAt(filename, pos, content)` | 按字节位置插入 |
| `insertBeforeLine` / `insertAfterLine` | 按行插入 |
| `deleteLine` / `deleteLines` | 删除行 |

> 行编辑(`insertAt` / `insertBeforeLine` / `insertAfterLine` / `deleteLine(s)`)采用**流式临时文件重写**:
> 原文件分块复制到同目录临时文件,途中完成编辑,最后原子 rename 替换。内存 O(1)(一块 128KB 缓冲),
> GB 级文件行编辑不再把整个文件读进内存(旧实现峰值内存 = 文件大小 × 2)。

### Writer(链式构建器)

```cpp
My::File::write("f.txt")
    .write("abc").writeLine("def")
    .writeBytes(bytes)
    .insertBeforeLine(2, "inserted\n")   // 仅 insert 模式有意义
    .reserve(4096)                        // 预分配缓冲区
    .commit();                            // 提交(失败保留缓冲区,可重试)
```

## 性能设计

核心原则:**绕开 iostream,直接使用操作系统文件描述符 + 大块 IO**。

iostream(`fstream`) 每一字节都经过 `streambuf` 虚函数层次与多层缓冲,业界高性能文件库(folly::File、fast_io、llvm::MemoryBuffer)均直接使用系统调用,通常比 iostream 快 2~10 倍。本库的 IO 核心:

1. **fd 直读直写**——Windows 走 `_wopen/_read/_write`(宽字符路径,中文目录无碍),POSIX 走 `open/read/write`,通过 `#ifdef` 双分支,API 完全一致。
2. **128KB 大块 IO**——系统调用次数降到最低。
3. **`memchr` 拆行 + 零拷贝视图**——`LineReader` 内部大块读 + `memchr`(CRT SIMD 实现)扫描 `\n`,取代 `std::getline` 的逐字符扫描;`forEachLine` 回调直接引用内部块缓冲区(仅在回调内有效,需保留请自行拷贝),消除每行一次 `std::string` 拷贝;支持跨块超长行。
4. **`resize_and_overwrite`(C++23)**——`readall` 直接在目标缓冲区上读,消除"先清零再读入"的双重内存写入。
5. **`WriteFull`/`ReadFull`**——循环处理短读短写,兼容全部平台语义。
6. **错误检查到 close**——写入后显式 `close` 并检查,捕获磁盘满等延迟错误;读中途的 I/O 错误不会被当成 EOF 静默吞掉,读取类 API 会返回失败而不是返回截断的数据。
7. **行编辑流式重写**——见上节,内存 O(1)。

### 实测数据(Windows 10, NVMe, Release, 20MB / 20 万行文件)

| 操作 | 吞吐 |
|---|---|
| `readall` | **~3000 MB/s** |
| `lineCount` | ~2200 MB/s |
| `forEachLine` | ~1500 MB/s(零拷贝) |
| `writeAll` | ~600~1100 MB/s(受磁盘缓存影响,波动较大) |
| `readLine`(第 10 万行) | ~7 ms |

> 数据由 `tests/test_file.cpp` 的 `FileTest.PerformanceReadAndWrite` 用例输出,仅供参考;读取路径已接近页面缓存带宽上限。

## 测试

集成了 [googletest](third/googletest-main)(`third/` 目录,`BUILD_TESTS=OFF` 可关闭):

```bash
cmake -S . -B build
cmake --build build --config Release --target file_tests
ctest --test-dir build -C Release --output-on-failure
# 或直接运行: build/Release/file_tests.exe
```

24 个用例覆盖:全部 API 的正常路径、负路径与边界(空文件、行号 0/越界、重复行号、无尾换行、跨 128KB 块的超长行与跨块大文件行编辑、UTF-8 中文路径、commit 失败重试、原子写入无临时文件残留),以及带吞吐下限断言的性能用例。历史 bug 均有用例回归保护(如"无尾换行文件最后一行后插入")。

CI(GitHub Actions)在 Windows / Linux / macOS 三平台矩阵上执行同一套构建 + 测试,保证跨平台分支持续被编译验证(见 [.github/workflows/ci.yml](.github/workflows/ci.yml))。

## 跨平台说明

- **路径编码**:调用方传入 UTF-8 字符串;Windows 内部经 `u8path` 转宽字符,中文路径正常。
- **源码编码**:源文件保存为 UTF-8 with BOM;MSVC 下 CMakeLists 已配置 `/utf-8`(见 `target_compile_options`)。
- **行号语义**:统一按 `\n` 计行(不处理 `\r\n` 的 `\r`,与 POSIX 工具一致);行号从 1 开始。
- **换行语义**:文件末尾无 `\n` 时,最后一行计一行;末尾有 `\n` 时其后不再有空行(与 `wc -l` 一致)。

## 后续改进方向

改进跟踪已独立成文件,见 [ROADMAP.md](ROADMAP.md)(含状态、方案、收益分析,持续更新)。

## 构建

```bash
cmake -S . -B build
cmake --build build --config Release
```

要求:C++23(MSVC 17.x / GCC 13+ / Clang 16+)、CMake ≥ 3.15。
