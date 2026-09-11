#ifndef MY_FILE_H
#define MY_FILE_H

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string_view>
#include <chrono>
#include <functional>
#include <future>

namespace My
{
    // ==================== 错误回调定制 ====================
    // 全局错误处理回调。默认为打印 stderr；传 nullptr 恢复默认。
    // 注意：setErrorHandler 非线程安全，应在程序启动时调用一次。
    using ErrorHandler = void (*)(std::string_view func,
                                  std::string_view filename,
                                  std::string_view message);
    void setErrorHandler(ErrorHandler handler);
    ErrorHandler getErrorHandler();

    // ==================== 行索引缓存 ====================
    // 一次遍历记录行起始字节偏移，后续 readLine 经寻址定位。
    // 支持稀疏模式：granularity > 1 时每 N 行存一个锚点，内存降低 N 倍，定位时锚内短扫描。
    // 构造时记录 size/mtime/首尾内容摘要，validate() 可检测文件外部变更。
    // 行数语义与 File::lineCount 一致：空文件 0 行，末尾 \n 后不计空行。
    class LineIndex
    {
    public:
        explicit LineIndex(std::string_view filename, size_t granularity = 1);
        size_t lineCount() const;
        std::optional<std::uintmax_t> lineStart(size_t lineNumber) const;
        bool validate(std::string_view filename) const;
        bool valid() const; // 构造是否成功（文件能打开且读取无错）
        size_t granularity() const { return granularity_; }

    private:
        std::vector<std::uintmax_t> offsets_;
        std::uintmax_t fileSize_ = 0;
        std::chrono::system_clock::time_point mtime_;
        uint64_t headHash_ = 0; // 首 64 字节 FNV-1a 摘要
        uint64_t tailHash_ = 0; // 尾 64 字节 FNV-1a 摘要
        size_t totalLines_ = 0; // 实际总行数（稀疏模式下 != offsets_.size()）
        size_t granularity_ = 1;
        bool valid_ = false;
    };

    // ==================== mmap 内存映射文件 ====================
    // 零拷贝读取。mmap 失败（空文件/特殊文件/32 位地址空间不足）时返回空。
    class MemoryMappedFile
    {
    public:
        MemoryMappedFile() = default;
        explicit MemoryMappedFile(std::string_view filename);
        ~MemoryMappedFile();
        MemoryMappedFile(const MemoryMappedFile &) = delete;
        MemoryMappedFile &operator=(const MemoryMappedFile &) = delete;
        MemoryMappedFile(MemoryMappedFile &&) noexcept;
        MemoryMappedFile &operator=(MemoryMappedFile &&) noexcept;

        const char *data() const;
        size_t size() const;
        bool isMapped() const;
        std::string_view view() const;

    private:
#ifdef _WIN32
        void *mappingHandle_ = nullptr;
        void *viewBase_ = nullptr;
#else
        void *mappedAddr_ = nullptr;
#endif
        size_t mappedSize_ = 0;
        void unmap();
    };

    // ==================== 增量哈希 ====================
    // 流式计算哈希：多次 update() 后调用 finalize() 获取十六进制结果。
    // 支持 SHA-256、CRC32、xxHash64 三种算法。
    // finalize() 后内部状态重置，后续 update 开始新的哈希计算（不是拼接追加）。
    class Hasher
    {
    public:
        enum class Algorithm : uint8_t
        {
            Sha256,
            Crc32,
            XxHash64
        };
        explicit Hasher(Algorithm algo = Algorithm::Sha256);
        void update(const char *data, size_t len);
        void update(std::string_view data) { update(data.data(), data.size()); }
        std::string finalize(); // 返回小写十六进制字符串，调用后内部状态重置

    private:
        Algorithm algo_;
        // SHA-256 状态
        uint32_t shaState_[8]{};
        uint8_t shaPending_[64]{};
        size_t shaPendingLen_ = 0;
        uint64_t shaTotalBytes_ = 0;
        // CRC32 状态
        uint32_t crcValue_ = 0xFFFFFFFF;
        // xxHash64 状态
        uint64_t xxState_[4]{}; // accumulators
        uint64_t xxTotalLen_ = 0;
        uint8_t xxBuffer_[32]{};
        size_t xxBufferLen_ = 0;

        void sha256Transform(uint32_t state[8], const uint8_t block[64]);
        std::string sha256Final();
        std::string crc32Final();
        std::string xxHash64Final();
    };

    // ==================== 文件监听 ====================
    // Windows: ReadDirectoryChangesW (支持递归); Linux: inotify (仅监视指定目录，不递归);
    // macOS: 暂不支持 (start() 返回 false)。回调在内部线程执行，回调内避免长时间阻塞。
    enum class FileEvent : uint8_t
    {
        Created,
        Modified,
        Deleted
    };
    class FileWatcher
    {
    public:
        using Callback = std::function<void(std::string_view path, FileEvent event)>;
        FileWatcher();
        ~FileWatcher();
        FileWatcher(const FileWatcher &) = delete;
        FileWatcher &operator=(const FileWatcher &) = delete;

        bool start(std::string_view path, Callback callback, bool recursive = true);
        void stop();
        bool isWatching() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> pImpl_;
    };

    class File
    {
    public:
        // 文件信息
        static bool exists(std::string_view filename);
        static std::optional<std::uintmax_t> size(std::string_view filename);                                // 与 std::filesystem::file_size 返回类型一致，避免 32 位平台截断
        static std::optional<std::chrono::system_clock::time_point> lastModified(std::string_view filename); // 获取最后修改系统时间
        static std::optional<size_t> lineCount(std::string_view filename);                                   // 获取文件总行数

        // 文件管理
        static bool remove(std::string_view filename);
        static bool copy(std::string_view src, std::string_view dest);
        // 大文件复制：内部委托 std::filesystem::copy_file（Windows 走 CopyFileW、Linux 走 copy_file_range，
        // 均优于用户态分块循环）；bufferSize 参数仅为接口兼容保留
        static bool copyLarge(std::string_view src, std::string_view dest, size_t bufferSize = 64 * 1024);
        static bool createDirectory(std::string_view path);            // 创建目录
        static bool createDirectories(std::string_view path);          // 递归创建目录
        static bool removeDirectory(std::string_view path);            // 递归删除目录及内容
        static bool move(std::string_view src, std::string_view dest); // 移动/重命名
        static bool touch(std::string_view filename);                  // 更新最后修改时间，文件不存在则创建空文件

        // 文件读取操作
        static std::optional<std::string> readall(std::string_view filename);

        static std::optional<std::vector<uint8_t>> readBytes(std::string_view filename);

        static std::optional<std::string> readLine(std::string_view filename, size_t lineNumber);

        // 读取指定范围的行（从 startLine 到 endLine，包含两端）
        static std::optional<std::vector<std::string>> readLines(std::string_view filename, size_t startLine, size_t endLine);

        // 读取指定的多行（非连续行号，如 {1, 5, 10, 20}）
        static std::optional<std::vector<std::string>> readLines(std::string_view filename, const std::vector<size_t> &lineNumbers);

        // 一次性读取全部行
        static std::optional<std::vector<std::string>> readAllLines(std::string_view filename);

        // 流式逐行遍历（大文件友好，不一次性载入内存）。
        // 回调签名：bool(lineNumber /*从1开始*/, std::string_view line)，返回 false 可提前终止。
        // 零拷贝：line 直接引用内部块缓冲区，仅在回调返回前有效，需保留请自行拷贝。
        static bool forEachLine(std::string_view filename,
                                const std::function<bool(size_t, std::string_view)> &handler);

        // 链式写入构建器
        class Writer;
        static Writer write(std::string_view filename, bool appendMode = false); // 从空开始写（默认覆盖，appendMode=true 追加）
        static Writer insert(std::string_view filename);                         // 插入模式（预加载原文件内容）

        // 直接写入（非链式）
        static bool writeAll(std::string_view filename, std::string_view content);
        static bool writeBytes(std::string_view filename, const std::vector<uint8_t> &data);
        static bool appendAll(std::string_view filename, std::string_view content);
        // 原子写入：先写同目录临时文件，成功后原子替换目标，避免写入中途崩溃损坏原文件。
        // durable=true（默认）时内容先强制刷盘再替换，掉电也不会出现"新文件存在但内容为空/不完整"；
        // 性能敏感场景可传 false，仅保留替换（rename）层面的原子性
        static bool writeAllAtomic(std::string_view filename, std::string_view content, bool durable = true);

        // 在指定位置插入内容（字节位置）
        static bool insertAt(std::string_view filename, size_t position, std::string_view content);

        // 在指定行之前/之后插入内容
        static bool insertBeforeLine(std::string_view filename, size_t lineNumber, std::string_view content);
        static bool insertAfterLine(std::string_view filename, size_t lineNumber, std::string_view content);

        // 删除指定行
        static bool deleteLine(std::string_view filename, size_t lineNumber);
        static bool deleteLines(std::string_view filename, size_t startLine, size_t endLine); // 删除连续多行

        // 使用行索引快速读取（O(log N) 定位 + 单次 seek+read）
        static std::optional<std::string> readLine(std::string_view filename, size_t lineNumber, const LineIndex &index);

        // mmap 零拷贝读取；mmap 失败时返回空，调用方可回退 readall
        static std::optional<MemoryMappedFile> readMapped(std::string_view filename);

        // 读取前 N 行 / 后 N 行
        static std::optional<std::vector<std::string>> head(std::string_view filename, size_t n);
        static std::optional<std::vector<std::string>> tail(std::string_view filename, size_t n);

        // 部分读取：从字节偏移 offset 起读 len 字节。len=0 表示读到文件末尾。
        static std::optional<std::string> readRange(std::string_view filename, std::uintmax_t offset, size_t len = 0);

        // 目录遍历
        static std::vector<std::string> listFiles(std::string_view path); // 列出直接子文件（不含子目录）
        static bool walk(std::string_view path,
                         const std::function<bool(std::string_view path, bool isDir)> &callback);
        static std::vector<std::string> globFiles(std::string_view path,
                                                  std::string_view pattern); // 简单 glob 通配符

        // 文件哈希（SHA-256 / CRC32 / xxHash64，返回小写十六进制字符串）
        static std::optional<std::string> fileHash(std::string_view filename);     // SHA-256
        static std::optional<std::string> fileCrc32(std::string_view filename);    // CRC32
        static std::optional<std::string> fileXxHash64(std::string_view filename); // xxHash64

        // 文件内容比较（先比 size，再块比较，比哈希快）
        static bool filesEqual(std::string_view file1, std::string_view file2);

        // 异步 IO（内部线程池，返回 std::future）
        static std::future<std::optional<std::string>> asyncReadall(std::string_view filename);
        static std::future<bool> asyncWriteAll(std::string_view filename, std::string_view content);
    };

    // 链式写入构建器
    class File::Writer
    {
    public:
        explicit Writer(std::string_view filename, bool appendMode = false);
        ~Writer();

        // 禁止拷贝，允许移动
        Writer(const Writer &) = delete;
        Writer &operator=(const Writer &) = delete;
        Writer(Writer &&) noexcept;
        Writer &operator=(Writer &&) noexcept;

        // 链式方法
        Writer &write(std::string_view content);
        Writer &writeLine(std::string_view content = "");
        Writer &writeBytes(const std::vector<uint8_t> &data);

        // 链式插入方法（仅 insert 模式有效）
        Writer &insertAt(size_t position, std::string_view content);
        Writer &insertBeforeLine(size_t lineNumber, std::string_view content);
        Writer &insertAfterLine(size_t lineNumber, std::string_view content);

        // 提交写入（返回是否成功）
        bool commit();

        // 获取当前缓冲区内容（只读视图，零拷贝）
        std::string_view view() const;

        // 获取缓冲区内容拷贝（需要修改时用）
        std::string str() const;

        // 清空缓冲区
        Writer &clear();

        // 预分配缓冲区大小
        Writer &reserve(size_t size);

        // 启用/禁用析构自动提交（默认禁用）
        Writer &setAutoCommit(bool enable);

        // 查询 Writer 是否已污染（insert 读取失败时置 true，commit 会拒绝）
        bool isPoisoned() const;

    private:
        class Impl;
        std::unique_ptr<Impl> pImpl;
        bool autoCommit_ = false;

        // 允许 File 类访问私有成员
        friend class File;
    };
} // namespace My

#endif // MY_FILE_H
