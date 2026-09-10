# ROADMAP —— My::File 改进跟踪

本文件记录库的后续改进方向,状态分为:待实施 / 进行中 / 已完成。
每次更新优化后在此记录结论与日期。

---

## 高优先级

### 1. 大文件原地行编辑 —— 已完成(2026-09-08)

**方案落地**:采用方案 A(临时文件流式重写)。`insertAt` / `insertBeforeLine` /
`insertAfterLine` / `deleteLine(s)` 统一走 `RewriteRange`:原文件分块复制到同目录
临时文件,途中在目标区间改写/插入/删除,最后原子 rename 替换。行定位用流式
`FindLineStartFd`(memchr 扫描,不加载文件)。内存 O(1),跨块大文件行编辑有回归用例。

### 2. 行索引缓存(LineIndex)—— 待实施

**现状**:每次 `readLine` / `readLines` 都从文件头顺序扫描到目标行,单次 O(N)。
对"反复随机读行"的场景(如日志查看器),N 次访问 = O(N²)。

**方案**:新增 `LineIndex` 类(或 `File::buildLineIndex(filename)`)——一次遍历记录
每行起始偏移(二进制搜索用),持有文件句柄;后续 `readLine(n)` 经 seek + 局部读取
实现 O(log N) 定位。注意文件可能被外部修改,可记录 size/mtime 做失效校验,
或在索引失效时回退顺序扫描。

**收益**:随机行访问从 O(N) 降到 O(log N);`readLines` 多行场景可合并邻近读取。

### 3. mmap 读取开关 —— 待实施

**现状**:`readall` 用 read 系统调用,页面缓存 → 用户缓冲区多一次拷贝。
mmap 零拷贝,且超大文件按页惰性加载,可避免一次性分配全部物理内存。

**方案**:`readall` 增加可选 mmap 路径(或新 API `readMapped`)——
llvm::MemoryBuffer 的做法:mmap 成功则用之,失败(空文件/特殊文件/32 位地址空间
不足)回退 read。注意:

- 并发截断触发 SIGBUS,需安装信号处理或接受"映射期间文件不可变"的契约;
- Windows 用 CreateFileMappingW + MapViewOfFile,解锁必须 UnmapViewOfFile;
- 返回类型需携带 unmap 责任(如返回带 deleter 的自定义 view)。

**收益**:readall 再省一次全量拷贝,超 1GB 文件物理内存占用显著下降。

### 4. 错误回调定制 —— 待实施

**现状**:所有错误统一 `PrintError` 打印 stderr。嵌入 GUI/日志系统的调用方无法
捕获错误,也不能静默。

**方案**:全局 `setErrorHandler(std::function<void(std::string_view func, std::string_view filename, std::string_view message)>)`,
默认为打 stderr;传 nullptr 恢复默认。同步修改所有 `PrintError` 调用点。

**收益**:库可嵌入无控制台环境;错误可进调用方日志。

---

## 中优先级

### 5. SIMD 换行扫描 —— 待实施

**现状**:`lineCount` / `LineReader` 用 `memchr`(CRT 已 SIMD 优化)。
当前 `lineCount` ~2.2GB/s,已非系统瓶颈(NVMe 读约 3GB/s)。

**方案**:自实现 SSE2/AVX2 的 `find_'\n'`(folly::findFirstOf 同思路:按 16/32 字节
向量比较 + 位掩码),避免 `memchr` 的逐次调用开销,并支持一次扫描计数全部换行
(当前 `lineCount` 每次 memchr 只找到一个,多次调用)。需按 CPU 特性运行时分派。

**收益**:理论 2~3 倍,但仅在"内存速度远超磁盘速度"的缓存命中场景有意义。
**备注**:优先级低于 1-4,列入待办以便某天磁盘更快时启用。

### 6. 异步 IO —— 待实施

**现状**:同步阻塞 IO。批量小文件、高并发场景(数据库、日志分发)受限于
同步等待与线程开销。

**方案**:Windows IOCP(或线程池 + OVERLAPPED)/ Linux io_uring。复杂度高,
建议先做 API 设计(如 `Future` 接口),再逐平台实现。**暂不建议动工**,
除非出现明确的多文件并发瓶颈。

### 7. Writer RAII 自动提交 —— 待实施

**现状**:`Writer` 析构不提交,忘记 `commit()` 会静默丢数据。

**方案**:`Writer` 增加可选开关(构造参数或成员 `setAutoCommit(true)`):
析构时若未提交且缓冲区非空则自动 commit。默认保持现状(显式提交),
避免异常路径下意外落盘。

---

## 低优先级(API 扩展)

### 8. 目录遍历 —— 待实施

`listFiles(path, recursive=false)` / `walk(path, callback)` ——
薄封装 `std::filesystem::recursive_directory_iterator`,过滤符号链接、
排序可选。可顺带支持 glob 通配符匹配(`*.cpp`、`dir/**`)。

### 9. 文件哈希 —— 待实施

MD5/SHA256。按"不重复造轮子"原则,直接集成现成实现(OpenSSL EVP /
PicoSHA2 header-only),不自行实现算法。

### 10. 文件监听 —— 待实施

目录变更通知:Windows `ReadDirectoryChangesW` / Linux `inotify`。
平台差异大,建议做成独立头文件/可选模块,不影响核心库依赖。

---

## 已完成

- [x] **2026-09-08 原子写掉电安全**——`writeAllAtomic` 新增 `durable` 参数(默认 true):
      写完临时文件后 `FlushFileBuffers`/`fsync` 刷盘再替换,消除"rename 成功但内容尚未落盘"
      的掉电窗口;`false` 保留原性能特征。临时文件名加 PID,多进程写同一目标不再撞名。
- [x] **2026-09-08 Windows rename 抗干扰**——`MoveFileExW` 替换失败时按 10~80ms 退避重试,
      规避杀毒软件扫描句柄造成的偶发 sharing violation。
- [x] **2026-09-08 读错误不再静默当 EOF**——`LineReader` 记录 I/O 错误状态,
      `readLine` / `readLines` / `readAllLines` / `forEachLine` / `lineCount` 遇读错误
      返回失败,不再把截断数据当完整数据返回。
- [x] **2026-09-08 缓冲区堆分配**——`LineReader` 与 `lineCount` 的 128KB 缓冲从栈改堆,
      小栈工作线程/深递归场景无栈溢出风险。
- [x] **2026-09-08 三平台 CI**——GitHub Actions 矩阵(Windows/Linux/macOS)执行
      配置 + 构建 + ctest,POSIX 分支持续被编译验证。
- [x] **2026-09-08 工程化**——顶层 CMakeLists 去重(C++23 与 /utf-8 由库目标传播);
      googletest 优先用 third/ 内置副本、缺失时回退 FetchContent;git 仓库初始化 + .gitignore。
- [x] **2026-08-16 基准测试常驻**——googletest 集成(third/)+ tests/test_file.cpp
      23 个用例(功能/边界/性能),`BUILD_TESTS` 开关控制。
- [x] **2026-08-16 forEachLine 零拷贝视图**——`LineReader::nextView` 直接引用
      内部块缓冲区,消除每行 `std::string` 拷贝,吞吐 ~1350 → ~1550 MB/s。
- [x] **2026-08-16 IO 核心重构**——iostream → fd 直读直写 + 128KB 大块 +
      memchr 拆行 + resize_and_overwrite;readall ~2500+ MB/s。
- [x] **2026-08-15 修复 insertAfterLine 无尾换行边界 bug**——"在最后一行后插入"
      原实现会失败;已用 FindLineStart 统一行定位逻辑,并有回归用例。
- [x] **2026-08-15 copyLarge 死循环修复**——bufferSize=0 时死循环;改为委托
      std::filesystem::copy_file(平台最优实现)。
