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

### 2. 行索引缓存(LineIndex)—— 已完成(2026-09-10)

**方案落地**:新增 `LineIndex` 类,构造时一次遍历记录每行起始字节偏移,
`readLine(filename, n, index)` 经二分查找 O(log N) 定位 + 单次 seek+read。
记录 fileSize/mtime,`validate()` 可检测文件外部变更使索引失效。

**实测**:5 万行文件 1000 次随机行访问,LineIndex 加速 2~4 倍(2454ms → 604ms)。

### 3. mmap 读取开关 —— 已完成(2026-09-10)

**方案落地**:新增 `MemoryMappedFile` 类 + `File::readMapped(filename)` API。
Windows 用 `CreateFileMappingW` + `MapViewOfFile`;Linux/macOS 用 `mmap`/`munmap`。
移动语义支持,禁止拷贝。空文件/映射失败返回空,调用方可回退 `readall`。

**实测**:10MB 文件反复 mmap 读取 ~22500 MB/s vs readall ~1750 MB/s(12.8 倍提升)。

### 4. 错误回调定制 —— 已完成(2026-09-10)

**方案落地**:全局 `setErrorHandler(ErrorHandler)` / `getErrorHandler()`,
`ErrorHandler` 为函数指针类型 `void(*)(string_view func, string_view filename, string_view message)`。
默认打印 stderr;传 `nullptr` 恢复默认。全部 `PrintError` 调用点均经回调分派。
注意 `setErrorHandler` 非线程安全,应在程序启动时调用一次。

---

## 中优先级

### 5. SIMD 换行扫描 —— 已完成(2026-09-10)

**方案落地**:`lineCount` 改用自实现 SSE2/AVX2 换行扫描 + 运行时分派
(CPUID 检测)。后续优化为 `CountNl` 单次遍历计数(AVX2 每次处理 32 字节 + popcnt 统计匹配位数),
替代原先逐个 `FindNl` 查找循环,消除每换行符一次函数调用的开销。
`lineCount` 实测 ~3150 MB/s(较原 FindNl 循环 ~2548 MB/s 提升约 24%)。

### 6. 异步 IO —— 已完成(2026-09-10)

**方案落地**:内部固定大小线程池(`hardware_concurrency` 个线程),
`asyncReadall` / `asyncWriteAll` 返回 `std::future`。
压测:64 个并发异步读 ~7ms,64 个并发异步写 ~43ms。

### 7. Writer RAII 自动提交 —— 已完成(2026-09-10)

**方案落地**:`Writer::setAutoCommit(true)` 启用析构自动提交。
默认保持禁用(显式提交),避免异常路径下意外落盘。

---

## 低优先级(API 扩展)

### 8. 目录遍历 —— 已完成(2026-09-10)

**方案落地**:`listFiles(path)` 列出直接子文件(不含子目录),
`walk(path, callback)` 递归遍历,callback 接收路径和 isDir 标志,
`globFiles(path, pattern)` 支持 `*.txt` 等简单通配符匹配。

### 9. 文件哈希 —— 已完成(2026-09-10)

**方案落地**:`fileHash(filename)` 返回 SHA-256 小写十六进制字符串。
自实现 SHA-256(无外部依赖),分块读取大文件友好。

### 10. 文件监听 —— 已完成(2026-09-10)

**方案落地**:`FileWatcher` 类,`start(path, callback, recursive)` 启动监听,
`stop()` 停止。Windows 用 `ReadDirectoryChangesW` + OVERLAPPED 异步模式;
Linux 用 `inotify`。回调接收路径和 `FileEvent`(Created/Modified/Deleted)。

---

## 已完成

- [x] **2026-09-10 代码审查修复(阶段 1+2: 正确性 + 跨平台)**——
  - LineIndex 语义对齐: 空文件 0 行, 末尾 `\n` 后不计幻影空行, 新增 `valid()` 方法;
  - 索引版 readLine 支持超长行(循环读取) + 读错误返回 nullopt;
  - UTF-8 路径修复: listFiles/walk/globFiles/Writer 析构全部改 `u8string()`, 避免 Windows ANSI 代码页破坏;
  - copy/copyLarge 错误经 PrintError 回调分派, 不再直接 `std::cerr`;
  - SIMD 门控: `MYFILE_HAS_SIMD` 宏 + GCC `__attribute__((target))`, 非 x86 平台回 memchr;
  - inotify 门控: `#elif defined(__linux__)`, macOS 返回 false;
  - POSIX recursive 文档化(注释说明 inotify 不递归);
  - 新增 4 个回归测试(LineIndexSemantics/LongLine, FileHashNistVectors, ChinesePathDirectoryList)。
- [x] **2026-09-10 代码审查修复(阶段 3: 健壮性)**——
  - FileWatcher stop() 改 `CancelIoEx` 取消挂起 IO, 先 join 线程再关闭句柄, 消除 UB;
  - 删除 FileWatcher Impl 中从未使用的 `completionPort` 成员及死代码;
  - FileWatcher `bytesReturned==0` 改 `continue`(缓冲区溢出/事件丢失时重新读取而非终止监听);
  - move() 添加退避重试(10/20/40/80ms), 与 ReplaceAtomically 同策略, 抗杀毒软件 sharing violation;
  - walk 已使用 `it.increment(ec)` 非抛异常遍历。
- [x] **2026-09-10 代码审查修复(阶段 4: 架构整理)**——
  - 错误模型统一: 索引版 readLine 行号 0/越界补充 PrintError 调用, 与非索引版一致;
  - file.cpp 拆分评估: 2361 行单文件库, “拷贝 MyFile/ 即用” 是核心集成方式, 拆分增加复杂度, 保持现状;
  - README 语义契约: 已文档化(行号从 1 开始, `\n` 计行, 末尾 `\n` 后无空行, 与 `wc -l` 一致);
  - LineIndex 稀疏模式、file.cpp 拆分多 TU 记入后续方向。
- [x] **2026-09-10 代码审查修复(阶段 5: 正确性收尾)**——
  - P0-1 File::insert() 数据丢失: Writer::Impl 新增 `poisoned` 标记, insert() 读取失败且文件存在时置标记, commit() 拒绝提交防截断原文件;
  - P0-2 FileWatcher 测试平台门控: `#if defined(_WIN32) || defined(__linux__)` 包裹两个 watcher 测试, macOS CI 不再必红;
  - P1-3 stop()/~Impl() join 前查 `worker.joinable()`, 线程构造失败路径不再 terminate;
  - P1-4 Writer::insertAt/insertBeforeLine/insertAfterLine 非法参数走 PrintError, 与 File:: 同名函数一致;
  - 琐碎: file.h LineIndex 注释修正(“二分查找 O(log N)” → “O(1) 直接寻址”); asyncWriteAll lambda 改移动捕获省一半内存峰值。
- [x] **2026-09-10 契约补全 + 质量安全网(阶段 6)**——
  - README 契约: FileWatcher 平台矩阵表(Win/Linux ✓, macOS ✗); mmap SIGBUS 契约(映射期间文件被外部截断会崩); readMapped 空文件语义; LineIndex mtime 粒度局限(1~2s 文件系统无法检出同尺寸同 mtime 内容变更); 性能数据标注“页缓存命中”;
  - CI: 新增 Linux ASan+UBSan job(`.github/workflows/ci.yml`);
  - Writer::isPoisoned() 公共查询接口, 调用方可主动检测而非仅靠 commit 失败;
  - insert() TOCTOU 窗口注释(固有风险, 不值得修);
  - poison 路径回归用例: InsertPoisonedOnDirectory(对目录路径 insert → poisoned → commit 拒绝); 测试数量 48 → 49。
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
      48 个用例(功能/边界/性能/压测/混合),`BUILD_TESTS` 开关控制。
- [x] **2026-08-16 forEachLine 零拷贝视图**——`LineReader::nextView` 直接引用
      内部块缓冲区,消除每行 `std::string` 拷贝,吞吐 ~1350 → ~1550 MB/s。
- [x] **2026-08-16 IO 核心重构**——iostream → fd 直读直写 + 128KB 大块 +
      memchr 拆行 + resize_and_overwrite;readall ~2500+ MB/s。
- [x] **2026-08-15 修复 insertAfterLine 无尾换行边界 bug**——"在最后一行后插入"
      原实现会失败;已用 FindLineStart 统一行定位逻辑,并有回归用例。
- [x] **2026-08-15 copyLarge 死循环修复**——bufferSize=0 时死循环;改为委托
      std::filesystem::copy_file(平台最优实现)。
