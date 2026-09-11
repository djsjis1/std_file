# My::File API 参考

> 本文件是 My::File 库的完整公共 API 参考，供使用者查阅。所有接口均定义在 `file.h` 中，命名空间为 `My`。

## 目录

- [集成方式](#集成方式)
- [错误回调](#错误回调)
- [File 类 — 文件信息](#file-类--文件信息)
- [File 类 — 文件管理](#file-类--文件管理)
- [File 类 — 读取](#file-类--读取)
- [File 类 — 写入](#file-类--写入)
- [File 类 — 行编辑](#file-类--行编辑)
- [File 类 — 目录遍历与哈希](#file-类--目录遍历与哈希)
- [File 类 — 异步 IO](#file-类--异步-io)
- [Writer 链式写入构建器](#writer-链式写入构建器)
- [Hasher 增量哈希](#hasher-增量哈希)
- [LineIndex 行索引缓存](#lineindex-行索引缓存)
- [MemoryMappedFile 内存映射文件](#memorymappedfile-内存映射文件)
- [FileWatcher 文件监听](#filewatcher-文件监听)
- [跨平台与语义约定](#跨平台与语义约定)

---

## 集成方式

```cpp
#include "file.h"
// 链接目标：MyFile::MyFile
```

```cmake
add_subdirectory(path/to/MyFile)
target_link_libraries(your_target PRIVATE MyFile::MyFile)
```

要求：C++23（MSVC 17.x / GCC 13+ / Clang 16+）、CMake ≥ 3.15。

---

## 错误回调

全局错误处理回调，默认打印到 stderr。所有 API 在遇到错误时通过此回调报告，不抛异常。

```cpp
namespace My;

using ErrorHandler = void (*)(std::string_view func,
                              std::string_view filename,
                              std::string_view message);

void setErrorHandler(ErrorHandler handler);
ErrorHandler getErrorHandler();
```

| 函数 | 说明 |
| --- | --- |
| `setErrorHandler(handler)` | 设置自定义错误回调；传 `nullptr` 恢复默认（stderr） |
| `getErrorHandler()` | 获取当前错误回调 |

> `setErrorHandler` 非线程安全，应在程序启动时调用一次。回调参数：`func` 为出错的 API 名称，`filename` 为相关文件路径，`message` 为错误描述。

---

## File 类 — 文件信息

```cpp
namespace My;
class File {
    static bool exists(std::string_view filename);
    static std::optional<std::uintmax_t> size(std::string_view filename);
    static std::optional<std::chrono::system_clock::time_point> lastModified(std::string_view filename);
    static std::optional<size_t> lineCount(std::string_view filename);
};
```

| 函数 | 返回值 | 说明 |
| --- | --- | --- |
| `exists(filename)` | `bool` | 文件是否存在 |
| `size(filename)` | `optional<uintmax_t>` | 文件大小（字节），与 `std::filesystem::file_size` 返回类型一致，避免 32 位平台截断 |
| `lastModified(filename)` | `optional<system_clock::time_point>` | 最后修改时间，经 `clock_cast` 转换为 `system_clock` |
| `lineCount(filename)` | `optional<size_t>` | 文件总行数。SIMD 加速（SSE2/AVX2 自动分派），分块 128KB 扫描，GB/s 级吞吐 |

**行计数语义**：按 `\n` 计行。空文件 0 行；末尾无 `\n` 时最后一行计一行；末尾有 `\n` 时其后不再计空行（与 `wc -l` 一致）。

---

## File 类 — 文件管理

```cpp
static bool remove(std::string_view filename);
static bool copy(std::string_view src, std::string_view dest);
static bool copyLarge(std::string_view src, std::string_view dest, size_t bufferSize = 64 * 1024);
static bool createDirectory(std::string_view path);
static bool createDirectories(std::string_view path);
static bool removeDirectory(std::string_view path);
static bool move(std::string_view src, std::string_view dest);
static bool touch(std::string_view filename);
```

| 函数 | 说明 |
| --- | --- |
| `remove(filename)` | 删除文件。文件不存在返回 `true`（幂等） |
| `copy(src, dest)` | 复制文件，委托 `std::filesystem::copy_file`（Windows 走 `CopyFileW`，Linux 走 `copy_file_range`），目标已存在则覆盖 |
| `copyLarge(src, dest, bufferSize)` | 同 `copy`，`bufferSize` 仅为接口兼容保留，实际不使用 |
| `createDirectory(path)` | 创建单级目录 |
| `createDirectories(path)` | 递归创建目录（含中间层级） |
| `removeDirectory(path)` | 递归删除目录及其全部内容。目录不存在也视为成功（幂等） |
| `move(src, dest)` | 移动/重命名。Windows 下可覆盖已存在的目标文件（含重试逻辑） |
| `touch(filename)` | 更新 mtime 为当前时间；文件不存在则创建空文件 |

---

## File 类 — 读取

```cpp
static std::optional<std::string> readall(std::string_view filename);
static std::optional<std::vector<uint8_t>> readBytes(std::string_view filename);
static std::optional<std::string> readLine(std::string_view filename, size_t lineNumber);
static std::optional<std::string> readLine(std::string_view filename, size_t lineNumber, const LineIndex &index);
static std::optional<std::vector<std::string>> readLines(std::string_view filename, size_t startLine, size_t endLine);
static std::optional<std::vector<std::string>> readLines(std::string_view filename, const std::vector<size_t> &lineNumbers);
static std::optional<std::vector<std::string>> readAllLines(std::string_view filename);
static bool forEachLine(std::string_view filename, const std::function<bool(size_t, std::string_view)> &handler);
static std::optional<MemoryMappedFile> readMapped(std::string_view filename);
static std::optional<std::vector<std::string>> head(std::string_view filename, size_t n);
static std::optional<std::vector<std::string>> tail(std::string_view filename, size_t n);
static std::optional<std::string> readRange(std::string_view filename, std::uintmax_t offset, size_t len = 0);
```

| 函数 | 说明 |
| --- | --- |
| `readall(filename)` | 读取整个文件为 `string`。使用 `resize_and_overwrite`（C++23）直接在目标缓冲区上读，消除双重写入 |
| `readBytes(filename)` | 读取整个文件为 `vector<uint8_t>`（二进制安全） |
| `readLine(filename, n)` | 读取第 `n` 行（从 1 起）。流式扫描，`n` 为 0 或超出范围返回 `nullopt` |
| `readLine(filename, n, index)` | 使用 `LineIndex` 快速读取第 `n` 行。O(1) 直接寻址 + 单次 seek&read |
| `readLines(filename, start, end)` | 读取连续行区间 `[startLine, endLine]`（含两端） |
| `readLines(filename, {1,5,10})` | 读取非连续多行，按原始请求顺序返回。内部排序后单次扫描 |
| `readAllLines(filename)` | 读取全部行到 `vector<string>` |
| `forEachLine(filename, handler)` | 流式逐行遍历，大文件友好。回调签名 `bool(size_t lineNumber, string_view line)`，返回 `false` 提前终止。**零拷贝**：`line` 直接引用内部块缓冲区，仅在回调返回前有效，需保留请自行拷贝 |
| `readMapped(filename)` | mmap 零拷贝读取。返回 `optional<MemoryMappedFile>`，空文件或映射失败返回 `nullopt` |
| `head(filename, n)` | 读取文件前 `n` 行。内存 O(n)，文件内容不变 |
| `tail(filename, n)` | 读取文件后 `n` 行。从文件尾倒序块扫描 `\n`，内存 O(n)。末尾 `\n` 不产生额外空行 |
| `readRange(filename, offset, len)` | 部分读取：从字节偏移 `offset` 起读 `len` 字节。`len=0` 表示读到文件末尾。偏移超出范围返回 `nullopt` |

---

## File 类 — 写入

```cpp
static bool writeAll(std::string_view filename, std::string_view content);
static bool writeBytes(std::string_view filename, const std::vector<uint8_t> &data);
static bool appendAll(std::string_view filename, std::string_view content);
static bool writeAllAtomic(std::string_view filename, std::string_view content, bool durable = true);
static Writer write(std::string_view filename, bool appendMode = false);
static Writer insert(std::string_view filename);
```

| 函数 | 说明 |
| --- | --- |
| `writeAll(filename, content)` | 覆盖写整个文件 |
| `writeBytes(filename, data)` | 覆盖写二进制数据 |
| `appendAll(filename, content)` | 追加写 |
| `writeAllAtomic(filename, content, durable)` | 原子覆盖写。先写同目录临时文件，成功后原子 rename 替换目标。`durable=true`（默认）内容先刷盘再替换，掉电安全；`false` 仅保留 rename 层面的原子性 |
| `write(filename, appendMode)` | 返回链式 `Writer`。`appendMode=false`（默认）覆盖，`true` 追加 |
| `insert(filename)` | 返回 `Writer`，预加载原文件内容到缓冲区，供链式插入操作使用。文件存在但读取失败时 Writer 被标记为 `poisoned`，拒绝提交 |

---

## File 类 — 行编辑

行编辑采用**流式临时文件重写**：原文件分块复制到同目录临时文件，途中完成编辑，最后原子 rename 替换。内存 O(1)（一块 128KB 缓冲），GB 级文件行编辑不会把整个文件读进内存。

```cpp
static bool insertAt(std::string_view filename, size_t position, std::string_view content);
static bool insertBeforeLine(std::string_view filename, size_t lineNumber, std::string_view content);
static bool insertAfterLine(std::string_view filename, size_t lineNumber, std::string_view content);
static bool deleteLine(std::string_view filename, size_t lineNumber);
static bool deleteLines(std::string_view filename, size_t startLine, size_t endLine);
```

| 函数 | 说明 |
| --- | --- |
| `insertAt(filename, pos, content)` | 在字节位置 `pos` 处插入内容 |
| `insertBeforeLine(filename, n, content)` | 在第 `n` 行之前插入内容。`n` 从 1 起 |
| `insertAfterLine(filename, n, content)` | 在第 `n` 行之后插入内容。`n` 为最后一行时追加到文件末尾 |
| `deleteLine(filename, n)` | 删除第 `n` 行 |
| `deleteLines(filename, start, end)` | 删除连续行区间 `[startLine, endLine]`（含两端） |

> 行号从 1 开始，传 0 会报错。行号超出范围返回 `false`。

---

## File 类 — 目录遍历与哈希

```cpp
static std::vector<std::string> listFiles(std::string_view path);
static bool walk(std::string_view path, const std::function<bool(std::string_view path, bool isDir)> &callback);
static std::vector<std::string> globFiles(std::string_view path, std::string_view pattern);
static std::optional<std::string> fileHash(std::string_view filename);     // SHA-256
static std::optional<std::string> fileCrc32(std::string_view filename);    // CRC32
static std::optional<std::string> fileXxHash64(std::string_view filename); // xxHash64
static bool filesEqual(std::string_view file1, std::string_view file2);
```

| 函数 | 说明 |
| --- | --- |
| `listFiles(path)` | 列出目录下直接子文件（不含子目录），返回文件名列表（UTF-8） |
| `walk(path, callback)` | 递归遍历目录树。回调接收 `(path, isDir)`，返回 `false` 提前终止。遍历成功返回 `true` |
| `globFiles(path, pattern)` | 简单通配符匹配（支持 `*` 和 `?`），仅匹配直接子文件 |
| `fileHash(filename)` | 计算文件 SHA-256 哈希，返回 64 字符小写十六进制字符串 |
| `fileCrc32(filename)` | 计算文件 CRC32 哈希，返回 8 字符小写十六进制字符串。比 SHA-256 快一个数量级，适合文件校验 |
| `fileXxHash64(filename)` | 计算文件 xxHash64 哈希，返回 16 字符小写十六进制字符串。极快非加密哈希 |
| `filesEqual(file1, file2)` | 比较两个文件内容是否相同。先比 size，再块比较，比哈希快。任一文件不存在返回 `false` |

---

## File 类 — 异步 IO

内部使用线程池（大小为 `max(2, hardware_concurrency)`），首次调用时惰性创建。

```cpp
static std::future<std::optional<std::string>> asyncReadall(std::string_view filename);
static std::future<bool> asyncWriteAll(std::string_view filename, std::string_view content);
```

| 函数 | 说明 |
| --- | --- |
| `asyncReadall(filename)` | 异步读取整个文件，返回 `future<optional<string>>` |
| `asyncWriteAll(filename, content)` | 异步覆盖写，返回 `future<bool>` |

> 参数在提交到线程池前拷贝（`string`），确保调用方生命周期安全。

---

## Writer 链式写入构建器

```cpp
class File::Writer {
    Writer &write(std::string_view content);
    Writer &writeLine(std::string_view content = "");
    Writer &writeBytes(const std::vector<uint8_t> &data);
    Writer &insertAt(size_t position, std::string_view content);
    Writer &insertBeforeLine(size_t lineNumber, std::string_view content);
    Writer &insertAfterLine(size_t lineNumber, std::string_view content);
    bool commit();
    std::string_view view() const;
    std::string str() const;
    Writer &clear();
    Writer &reserve(size_t size);
    Writer &setAutoCommit(bool enable);
    bool isPoisoned() const;
};
```

### 写入方法

| 方法 | 说明 |
| --- | --- |
| `write(content)` | 追加文本到缓冲区 |
| `writeLine(content)` | 追加文本 + `\n` 到缓冲区。`content` 默认为空（仅写换行） |
| `writeBytes(data)` | 追加二进制数据到缓冲区 |

### 插入方法（仅 `insert` 模式有效）

| 方法 | 说明 |
| --- | --- |
| `insertAt(position, content)` | 在缓冲区字节位置插入。超出范围时报错但不中断链式调用 |
| `insertBeforeLine(n, content)` | 在缓冲区第 `n` 行之前插入 |
| `insertAfterLine(n, content)` | 在缓冲区第 `n` 行之后插入 |

### 控制方法

| 方法 | 说明 |
| --- | --- |
| `commit()` | 将缓冲区内容写入文件。`poisoned` 状态拒绝提交。成功后清空缓冲区 |
| `view()` | 返回缓冲区只读视图（零拷贝 `string_view`） |
| `str()` | 返回缓冲区内容拷贝 |
| `clear()` | 清空缓冲区 |
| `reserve(size)` | 预分配缓冲区大小 |
| `setAutoCommit(true)` | 启用析构自动提交，`~Writer()` 时自动调用 `commit()` |
| `isPoisoned()` | 查询 Writer 是否已污染（`insert` 模式下文件读取失败时置 `true`） |

### 使用示例

```cpp
// 覆盖写
My::File::write("f.txt")
    .write("abc").writeLine("def")
    .reserve(4096)
    .commit();

// 插入模式
My::File::insert("f.txt")
    .insertBeforeLine(2, "new line\n")
    .commit();

// 自动提交
{
    auto w = My::File::write("f.txt");
    w.setAutoCommit(true).write("safe");
} // 析构时自动 commit
```

> Writer 禁止拷贝，允许移动。析构时若 `autoCommit` 启用且缓冲区非空，自动提交。

---

## Hasher 增量哈希

流式计算哈希：多次 `update()` 后调用 `finalize()` 获取十六进制结果。支持 SHA-256、CRC32、xxHash64 三种算法。`finalize()` 后内部状态**重置为初始值**，后续 `update()` 开始一轮**全新的哈希计算**（不是在上次结果上拼接追加）。

```cpp
class Hasher {
    enum class Algorithm : uint8_t { Sha256, Crc32, XxHash64 };
    explicit Hasher(Algorithm algo = Algorithm::Sha256);
    void update(const char *data, size_t len);
    void update(std::string_view data);
    std::string finalize(); // 返回小写十六进制字符串，调用后内部状态重置
};
```

| 方法 | 说明 |
| --- | --- |
| `Hasher(algo)` | 构造指定算法的哈希计算器 |
| `update(data, len)` | 喂入数据，可多次调用 |
| `update(string_view)` | 便捷重载 |
| `finalize()` | 计算并返回十六进制结果。调用后内部状态重置为初始值，可开始新一轮独立哈希（非拼接追加） |

```cpp
// 增量计算 SHA-256
My::Hasher h(My::Hasher::Algorithm::Sha256);
h.update("hello ");
h.update("world");
std::string hex = h.finalize(); // 与 fileHash 结果一致
```

> 与 `fileHash`/`fileCrc32`/`fileXxHash64` 结果完全一致——文件哈希函数内部使用相同算法实现。`Hasher` 适用于流式场景（边接收网络数据边算哈希）。

---

## LineIndex 行索引缓存

一次遍历记录行起始字节偏移，后续 `readLine` 经 O(1) 直接寻址定位。支持稀疏模式：`granularity > 1` 时每 N 行存一个锚点，内存降低 N 倍。构造时记录 `size + mtime + 首尾 64 字节内容摘要`，`validate()` 可检测文件外部变更。`validate()` 内部有 1 秒短时缓存，高频 `readLine` 场景不会每次都触发系统调用。

```cpp
class LineIndex {
    explicit LineIndex(std::string_view filename, size_t granularity = 1);
    size_t lineCount() const;
    std::optional<std::uintmax_t> lineStart(size_t lineNumber) const;
    bool validate(std::string_view filename) const;
    bool valid() const;
    size_t granularity() const;
};
```

| 方法 | 说明 |
| --- | --- |
| `LineIndex(filename, granularity)` | 构造时遍历文件，记录行偏移。`granularity=1`（默认）稠密模式，每行一个锚点；`granularity=N` 稀疏模式，每 N 行一个锚点，内存降低 N 倍 |
| `lineCount()` | 返回总行数 |
| `lineStart(n)` | 返回第 `n` 行（从 1 起）的字节偏移。稀疏模式下返回最近锚点偏移，`readLine` 自动锚内短扫描 |
| `validate(filename)` | 检查文件 `size + mtime + 首尾 64 字节内容摘要` 是否与构造时一致。内部有 1 秒短时缓存，1 秒内重复调用直接返回上次结果，避免每次 `readLine` 触发 5 个系统调用 |
| `valid()` | 构造是否成功（文件能打开且读取无错） |
| `granularity()` | 返回当前稀疏粒度 |

> **失效检测增强**：除 `size + mtime` 外，还校验文件首尾各 64 字节的 FNV-1a 摘要，同尺寸同 mtime 但内容已变的场景也能检出。`validate()` 结果缓存 1 秒——对高频 `readLine`（每次调用都 validate）而言，随机读行不再每次付出 open + stat + read 的系统调用开销。文件变更后最多 1 秒即可检出。

---

## MemoryMappedFile 内存映射文件

零拷贝读取。mmap 失败（空文件/特殊文件/32 位地址空间不足）时返回空。

```cpp
class MemoryMappedFile {
    MemoryMappedFile() = default;
    explicit MemoryMappedFile(std::string_view filename);
    ~MemoryMappedFile();
    // 禁止拷贝，允许移动
    const char *data() const;
    size_t size() const;
    bool isMapped() const;
    std::string_view view() const;
};
```

| 方法 | 说明 |
| --- | --- |
| `MemoryMappedFile(filename)` | 构造时执行 mmap 映射。Windows 使用 `CreateFileMappingW` + `MapViewOfFile`，POSIX 使用 `mmap` |
| `data()` | 返回映射内存起始指针 |
| `size()` | 返回映射大小（字节） |
| `isMapped()` | 是否成功映射 |
| `view()` | 返回 `string_view{data(), size()}`，直接引用映射内存 |

> **SIGBUS 契约**：`view()` 在 `MemoryMappedFile` 存活期间有效。若映射期间文件被外部进程截断或删除，访问映射内存可能触发 SIGBUS（POSIX）或访问违规（Windows）。调用方需确保映射期间无外部截断操作，否则应使用 `readall`。

---

## FileWatcher 文件监听

监视目录变化，支持递归。回调在内部工作线程执行。

```cpp
enum class FileEvent : uint8_t { Created, Modified, Deleted };

class FileWatcher {
    using Callback = std::function<void(std::string_view path, FileEvent event)>;
    FileWatcher();
    ~FileWatcher();
    // 禁止拷贝
    bool start(std::string_view path, Callback callback, bool recursive = true);
    void stop();
    bool isWatching() const;
};
```

| 方法 | 说明 |
| --- | --- |
| `start(path, callback, recursive)` | 开始监听目录。`recursive=true`（默认）递归监视子目录。成功返回 `true` |
| `stop()` | 停止监听并等待工作线程退出 |
| `isWatching()` | 是否正在监听 |

| 平台 | 后端 | 说明 |
| --- | --- | --- |
| Windows | `ReadDirectoryChangesW` | 支持递归，监视文件创建/修改/删除/重命名 |
| Linux | `inotify` | 监视指定目录。`recursive` 参数仅影响 `MOVED_FROM`/`MOVED_TO` 事件 |
| macOS | 桩实现 | `start()` 返回 `false`，暂不支持 |

> **去抖提示**：操作系统可能为单次保存操作触发多次通知（编辑器写内容 + 更新元数据）。建议在回调层实现去抖逻辑。

---

## 跨平台与语义约定

| 项目 | 约定 |
| --- | --- |
| **路径编码** | 调用方传入 UTF-8 字符串；Windows 内部转宽字符，中文路径正常 |
| **行号** | 从 1 开始，按 `\n` 计行（不处理 `\r\n` 的 `\r`） |
| **行计数** | 空文件 0 行；末尾无 `\n` 最后一行计一行；末尾有 `\n` 其后不再计空行 |
| **错误处理** | 通过 `ErrorHandler` 回调报告，不抛异常；失败返回 `false` / `nullopt` |
| **线程安全** | 各 API 调用彼此独立，可并发调用不同文件；同一文件的并发写入由调用方保证互斥 |
