#include <gtest/gtest.h>
#include "file.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr const char *kTestDir = "test_tmp";

    // 拼接测试目录下的路径
    std::string P(const std::string &name)
    {
        return std::string(kTestDir) + "/" + name;
    }

    // 写入并断言成功
    void WriteOk(const std::string &name, const std::string &content)
    {
        ASSERT_TRUE(My::File::writeAll(P(name), content)) << name;
    }

    double MbPerSec(double bytes, double ms)
    {
        return (bytes / 1024.0 / 1024.0) / (ms / 1000.0);
    }

    // ==================== 测试基类 ====================
    class FileTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            std::error_code ec;
            std::filesystem::create_directories(kTestDir, ec);
        }

        static void TearDownTestSuite()
        {
            std::error_code ec;
            std::filesystem::remove_all(kTestDir, ec);
        }

        void SetUp() override
        {
            // 每个用例从干净目录开始
            std::error_code ec;
            std::filesystem::remove_all(kTestDir, ec);
            std::filesystem::create_directories(kTestDir, ec);
        }
    };

    // ==================== 文件信息 ====================

    TEST_F(FileTest, ExistsSizeLastModified)
    {
        EXPECT_FALSE(My::File::exists(P("nope.txt")));

        WriteOk("info.txt", "hello");
        EXPECT_TRUE(My::File::exists(P("info.txt")));

        auto size = My::File::size(P("info.txt"));
        ASSERT_TRUE(size.has_value());
        EXPECT_EQ(*size, 5u);

        auto mt = My::File::lastModified(P("info.txt"));
        ASSERT_TRUE(mt.has_value());
        // 修改时间在最近 1 小时内(合理性检查)
        const auto now = std::chrono::system_clock::now();
        EXPECT_LT(now - *mt, std::chrono::hours(1));

        EXPECT_FALSE(My::File::size(P("nope.txt")).has_value());
        EXPECT_FALSE(My::File::lastModified(P("nope.txt")).has_value());
    }

    TEST_F(FileTest, LineCountEdgeCases)
    {
        EXPECT_FALSE(My::File::lineCount(P("nope.txt")).has_value());

        // 空文件 = 0 行
        WriteOk("empty.txt", "");
        EXPECT_EQ(*My::File::lineCount(P("empty.txt")), 0u);

        // 无尾换行、有尾换行
        WriteOk("a.txt", "abc");
        EXPECT_EQ(*My::File::lineCount(P("a.txt")), 1u);
        WriteOk("b.txt", "abc\n");
        EXPECT_EQ(*My::File::lineCount(P("b.txt")), 1u);

        // 多行
        WriteOk("c.txt", "a\nb\nc");
        EXPECT_EQ(*My::File::lineCount(P("c.txt")), 3u);
        WriteOk("d.txt", "a\nb\nc\n");
        EXPECT_EQ(*My::File::lineCount(P("d.txt")), 3u);

        // 纯换行
        WriteOk("n1.txt", "\n");
        EXPECT_EQ(*My::File::lineCount(P("n1.txt")), 1u);
        WriteOk("n2.txt", "\n\n");
        EXPECT_EQ(*My::File::lineCount(P("n2.txt")), 2u);
    }

    TEST_F(FileTest, Touch)
    {
        ASSERT_TRUE(My::File::touch(P("t.txt")));
        EXPECT_TRUE(My::File::exists(P("t.txt")));
        EXPECT_EQ(*My::File::size(P("t.txt")), 0u);

        // 已存在的文件再次 touch
        ASSERT_TRUE(My::File::touch(P("t.txt")));
        EXPECT_TRUE(My::File::exists(P("t.txt")));
    }

    // ==================== 文件管理 ====================

    TEST_F(FileTest, Remove)
    {
        WriteOk("r.txt", "x");
        ASSERT_TRUE(My::File::remove(P("r.txt")));
        EXPECT_FALSE(My::File::exists(P("r.txt")));
        EXPECT_FALSE(My::File::remove(P("r.txt"))); // 已不存在
    }

    TEST_F(FileTest, CopyAndCopyLarge)
    {
        const std::string payload(100000, 'z');
        WriteOk("src.bin", payload);

        ASSERT_TRUE(My::File::copy(P("src.bin"), P("dst.bin")));
        ASSERT_TRUE(My::File::copyLarge(P("src.bin"), P("dst2.bin"), 0)); // bufferSize 仅为兼容参数
        EXPECT_EQ(*My::File::readall(P("dst.bin")), payload);
        EXPECT_EQ(*My::File::readall(P("dst2.bin")), payload);

        // 覆盖已存在目标
        ASSERT_TRUE(My::File::copy(P("src.bin"), P("dst.bin")));
        EXPECT_EQ(*My::File::readall(P("dst.bin")), payload);

        // 源不存在
        EXPECT_FALSE(My::File::copy(P("nope.bin"), P("x.bin")));
        EXPECT_FALSE(My::File::copyLarge(P("nope.bin"), P("x.bin")));
    }

    TEST_F(FileTest, DirectoryOps)
    {
        ASSERT_TRUE(My::File::createDirectories(P("d1/d2/d3")));
        ASSERT_TRUE(My::File::createDirectory(P("d1/d4")));
        ASSERT_TRUE(My::File::removeDirectory(P("d1")));
        EXPECT_FALSE(std::filesystem::exists(P("d1")));
        // 幂等:目录不存在也成功
        ASSERT_TRUE(My::File::removeDirectory(P("d1")));
    }

    TEST_F(FileTest, Move)
    {
        WriteOk("m1.txt", "old");
        WriteOk("m2.txt", "new");

        ASSERT_TRUE(My::File::move(P("m1.txt"), P("m2.txt")));
        EXPECT_FALSE(My::File::exists(P("m1.txt")));
        EXPECT_EQ(*My::File::readall(P("m2.txt")), "old");

        // 移动到新名字
        ASSERT_TRUE(My::File::move(P("m2.txt"), P("m3.txt")));
        EXPECT_EQ(*My::File::readall(P("m3.txt")), "old");
    }

    // ==================== 读取 ====================

    TEST_F(FileTest, ReadAllAndReadBytes)
    {
        // 正常内容
        WriteOk("f.txt", "hello world");
        auto s = My::File::readall(P("f.txt"));
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(*s, "hello world");

        // 空文件
        WriteOk("e.txt", "");
        auto es = My::File::readall(P("e.txt"));
        ASSERT_TRUE(es.has_value());
        EXPECT_TRUE(es->empty());

        // 二进制往返(含 0x00、0xFF)
        const std::vector<uint8_t> bin = {0x00, 0x01, 0xFF, 0x7F, 0x80, 0x00};
        ASSERT_TRUE(My::File::writeBytes(P("b.bin"), bin));
        auto rb = My::File::readBytes(P("b.bin"));
        ASSERT_TRUE(rb.has_value());
        EXPECT_EQ(*rb, bin);

        // 不存在
        EXPECT_FALSE(My::File::readall(P("nope.txt")).has_value());
        EXPECT_FALSE(My::File::readBytes(P("nope.bin")).has_value());
    }

    TEST_F(FileTest, ReadLine)
    {
        WriteOk("l.txt", "one\ntwo\nthree"); // 无尾换行

        EXPECT_EQ(*My::File::readLine(P("l.txt"), 1), "one");
        EXPECT_EQ(*My::File::readLine(P("l.txt"), 3), "three");

        // 行号 0 或超出范围
        EXPECT_FALSE(My::File::readLine(P("l.txt"), 0).has_value());
        EXPECT_FALSE(My::File::readLine(P("l.txt"), 4).has_value());

        // 空行
        WriteOk("l2.txt", "a\n\nb\n");
        EXPECT_EQ(*My::File::readLine(P("l2.txt"), 2), "");
    }

    TEST_F(FileTest, ReadLinesRange)
    {
        WriteOk("r.txt", "1\n2\n3\n4\n5\n");

        auto lines = My::File::readLines(P("r.txt"), 2, 4);
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), 3u);
        EXPECT_EQ((*lines)[0], "2");
        EXPECT_EQ((*lines)[1], "3");
        EXPECT_EQ((*lines)[2], "4");

        // 范围包含最后一行(无尾换行)
        WriteOk("r2.txt", "a\nb\nc");
        auto tail = My::File::readLines(P("r2.txt"), 2, 3);
        ASSERT_TRUE(tail.has_value());
        ASSERT_EQ(tail->size(), 2u);
        EXPECT_EQ((*tail)[1], "c");

        // 无效范围与超出范围
        EXPECT_FALSE(My::File::readLines(P("r.txt"), 0, 3).has_value());
        EXPECT_FALSE(My::File::readLines(P("r.txt"), 4, 3).has_value());
        EXPECT_FALSE(My::File::readLines(P("r.txt"), 4, 9).has_value());
    }

    TEST_F(FileTest, ReadLinesMultiple)
    {
        WriteOk("r.txt", "1\n2\n3\n4\n5\n");

        // 乱序输入,按输入顺序返回
        auto lines = My::File::readLines(P("r.txt"), {5, 1, 3});
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), 3u);
        EXPECT_EQ((*lines)[0], "5");
        EXPECT_EQ((*lines)[1], "1");
        EXPECT_EQ((*lines)[2], "3");

        // 重复行号
        auto dup = My::File::readLines(P("r.txt"), {1, 1, 2});
        ASSERT_TRUE(dup.has_value());
        ASSERT_EQ(dup->size(), 3u);
        EXPECT_EQ((*dup)[0], "1");
        EXPECT_EQ((*dup)[1], "1");
        EXPECT_EQ((*dup)[2], "2");

        // 空输入、非法行号、超出范围
        auto empty = My::File::readLines(P("r.txt"), {});
        ASSERT_TRUE(empty.has_value());
        EXPECT_TRUE(empty->empty());
        EXPECT_FALSE(My::File::readLines(P("r.txt"), {0}).has_value());
        EXPECT_FALSE(My::File::readLines(P("r.txt"), {6}).has_value());
    }

    TEST_F(FileTest, ReadAllLines)
    {
        WriteOk("a.txt", "x\ny\nz");
        auto lines = My::File::readAllLines(P("a.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), 3u);
        EXPECT_EQ((*lines)[0], "x");
        EXPECT_EQ((*lines)[2], "z");

        WriteOk("e.txt", "");
        auto empty = My::File::readAllLines(P("e.txt"));
        ASSERT_TRUE(empty.has_value());
        EXPECT_TRUE(empty->empty());
    }

    TEST_F(FileTest, ForEachLine)
    {
        WriteOk("f.txt", "a\nb\nc\n");

        size_t count = 0;
        size_t lastNum = 0;
        std::string joined;
        const bool ok = My::File::forEachLine(P("f.txt"), [&](size_t num, std::string_view line)
                                              {
            ++count;
            lastNum = num;
            joined += std::string(line);
            return true; });
        EXPECT_TRUE(ok);
        EXPECT_EQ(count, 3u);
        EXPECT_EQ(lastNum, 3u);
        EXPECT_EQ(joined, "abc");

        // 提前终止:只处理前 2 行
        count = 0;
        My::File::forEachLine(P("f.txt"), [&](size_t, std::string_view)
                              {
            ++count;
            return count < 2; });
        EXPECT_EQ(count, 2u);

        // 空文件:不回调
        WriteOk("e.txt", "");
        count = 0;
        EXPECT_TRUE(My::File::forEachLine(P("e.txt"), [&](size_t, std::string_view)
                                          {
            ++count;
            return true; }));
        EXPECT_EQ(count, 0u);

        // 不存在的文件
        EXPECT_FALSE(My::File::forEachLine(P("nope.txt"), [&](size_t, std::string_view)
                                           { return true; }));

        // 跨块超长行(>128KB)+ 无尾换行
        const std::string longLine(200 * 1024, 'L');
        WriteOk("long.txt", longLine + "\nend");
        count = 0;
        std::string first;
        My::File::forEachLine(P("long.txt"), [&](size_t, std::string_view lv)
                              {
            ++count;
            if (count == 1)
            {
                first.assign(lv.data(), lv.size());
            }
            return true; });
        EXPECT_EQ(count, 2u);
        EXPECT_EQ(first, longLine);
    }

    TEST_F(FileTest, LongLineAcrossBlocks)
    {
        // 一行 300KB(超过 LineReader 内部 128KB 块),验证跨块拼接
        const std::string longLine(300 * 1024, 'L');
        const std::string content = longLine + "\nsecond\n";
        WriteOk("long.txt", content);

        auto line = My::File::readLine(P("long.txt"), 1);
        ASSERT_TRUE(line.has_value());
        EXPECT_EQ(*line, longLine);

        auto lines = My::File::readAllLines(P("long.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), 2u);
        EXPECT_EQ((*lines)[0], longLine);
        EXPECT_EQ((*lines)[1], "second");

        // 无尾换行的超长行(整行都在 overflow 中)
        WriteOk("long2.txt", longLine);
        auto whole = My::File::readall(P("long2.txt"));
        ASSERT_TRUE(whole.has_value());
        EXPECT_EQ(*whole, longLine);
    }

    // ==================== 写入 ====================

    TEST_F(FileTest, WriteAllAppendAll)
    {
        ASSERT_TRUE(My::File::writeAll(P("w.txt"), "first"));
        ASSERT_TRUE(My::File::appendAll(P("w.txt"), "second"));
        EXPECT_EQ(*My::File::readall(P("w.txt")), "firstsecond");

        // 覆盖写
        ASSERT_TRUE(My::File::writeAll(P("w.txt"), "reset"));
        EXPECT_EQ(*My::File::readall(P("w.txt")), "reset");

        // 追加到不存在的文件(创建)
        ASSERT_TRUE(My::File::appendAll(P("w2.txt"), "new"));
        EXPECT_EQ(*My::File::readall(P("w2.txt")), "new");
    }

    TEST_F(FileTest, WriteAllAtomic)
    {
        // 目标不存在
        ASSERT_TRUE(My::File::writeAllAtomic(P("a.txt"), "v1"));
        EXPECT_EQ(*My::File::readall(P("a.txt")), "v1");

        // 目标已存在,内容变长/变短
        ASSERT_TRUE(My::File::writeAllAtomic(P("a.txt"), "v2-longer"));
        EXPECT_EQ(*My::File::readall(P("a.txt")), "v2-longer");
        ASSERT_TRUE(My::File::writeAllAtomic(P("a.txt"), "v3"));
        EXPECT_EQ(*My::File::readall(P("a.txt")), "v3");

        // durable=false（跳过强制刷盘）语义等价，仅持久性保证不同
        ASSERT_TRUE(My::File::writeAllAtomic(P("a.txt"), "v4-noflush", false));
        EXPECT_EQ(*My::File::readall(P("a.txt")), "v4-noflush");

        // 无残留临时文件
        for (const auto &entry : std::filesystem::directory_iterator(kTestDir))
        {
            EXPECT_EQ(entry.path().string().find(".tmp"), std::string::npos)
                << "残留临时文件: " << entry.path();
        }
    }

    TEST_F(FileTest, LineEditLargeFile)
    {
        // 约 2MB（远超 128KB 单块）大文件的行编辑：行编辑走流式临时文件路径（内存 O(1)），
        // 此用例验证跨块正确性与无临时文件残留
        constexpr size_t kLines = 20000; // 每行 100 字节
        {
            My::File::Writer w = My::File::write(P("big.txt"));
            const std::string line(99, 'x');
            for (size_t i = 0; i < kLines; ++i)
            {
                w.writeLine(line);
            }
            ASSERT_TRUE(w.commit());
        }

        // 中间行前插入（跨多个块定位）
        ASSERT_TRUE(My::File::insertBeforeLine(P("big.txt"), 10000, "inserted\n"));
        auto lines = My::File::readAllLines(P("big.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), kLines + 1u);
        EXPECT_EQ((*lines)[9999], "inserted");
        EXPECT_EQ((*lines)[10000].size(), 99u);

        // 删除包含插入行的一段区间
        ASSERT_TRUE(My::File::deleteLines(P("big.txt"), 9999, 10001));
        lines = My::File::readAllLines(P("big.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), kLines - 2u);
        EXPECT_EQ((*lines)[0].size(), 99u);

        // 中间行后插入
        ASSERT_TRUE(My::File::insertAfterLine(P("big.txt"), 1, "after\n"));
        lines = My::File::readAllLines(P("big.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), kLines - 1u);
        EXPECT_EQ((*lines)[1], "after");

        // 字节位置插入（区间与块边界对齐）
        ASSERT_TRUE(My::File::insertAt(P("big.txt"), 0, "head\n"));
        lines = My::File::readAllLines(P("big.txt"));
        ASSERT_TRUE(lines.has_value());
        ASSERT_EQ(lines->size(), kLines);
        EXPECT_EQ((*lines)[0], "head");

        // 无残留临时文件
        for (const auto &entry : std::filesystem::directory_iterator(kTestDir))
        {
            EXPECT_EQ(entry.path().string().find(".tmp"), std::string::npos)
                << "残留临时文件: " << entry.path();
        }
    }

    TEST_F(FileTest, WriterChain)
    {
        // 基本链式
        My::File::Writer w = My::File::write(P("w.txt"));
        w.write("a").writeLine("b").write("c");
        ASSERT_TRUE(w.commit());
        EXPECT_EQ(*My::File::readall(P("w.txt")), "ab\nc");
        EXPECT_TRUE(w.view().empty()); // commit 后缓冲区清空

        // append 模式
        ASSERT_TRUE(My::File::write(P("w.txt"), true).write("x").commit());
        EXPECT_EQ(*My::File::readall(P("w.txt")), "ab\ncx");

        // insert 模式:预加载 + 行前插入
        ASSERT_TRUE(My::File::insert(P("w.txt")).insertBeforeLine(1, "h\n").commit());
        EXPECT_EQ(*My::File::readall(P("w.txt")), "h\nab\ncx");

        // insert 模式:行后插入(文件无尾换行,验证末尾插入)
        ASSERT_TRUE(My::File::insert(P("w.txt")).insertAfterLine(3, "\nz").commit());
        EXPECT_EQ(*My::File::readall(P("w.txt")), "h\nab\ncx\nz");

        // 移动语义
        My::File::Writer w2 = My::File::write(P("mv.txt"));
        w2.write("data");
        My::File::Writer w3 = std::move(w2);
        ASSERT_TRUE(w3.commit());
        EXPECT_EQ(*My::File::readall(P("mv.txt")), "data");

        // clear / reserve / view / str
        My::File::Writer w4 = My::File::write(P("cr.txt"));
        w4.reserve(128).write("abc");
        EXPECT_EQ(w4.view(), "abc");
        EXPECT_EQ(w4.str(), "abc");
        w4.clear();
        EXPECT_TRUE(w4.view().empty());
    }

    TEST_F(FileTest, CommitFailureKeepsBuffer)
    {
        // 目录不存在 → 打开失败 → commit 失败,缓冲区保留可重试
        My::File::Writer w = My::File::write(P("no_such_dir/file.txt"));
        w.write("content");
        EXPECT_FALSE(w.commit());
        EXPECT_EQ(w.view(), "content");
    }

    // insert() 污染路径回归: 对目录路径调 insert, readall 失败 + exists 为真 → poisoned
    TEST_F(FileTest, InsertPoisonedOnDirectory)
    {
        // kTestDir 是一个目录, readall 会失败, exists 返回 true
        My::File::Writer w = My::File::insert(kTestDir);
        EXPECT_TRUE(w.isPoisoned());
        EXPECT_FALSE(w.commit()); // 拒绝提交, 不截断目录
    }

    // ==================== 行编辑 ====================

    TEST_F(FileTest, InsertAt)
    {
        WriteOk("i.txt", "abc");
        ASSERT_TRUE(My::File::insertAt(P("i.txt"), 1, "X"));
        EXPECT_EQ(*My::File::readall(P("i.txt")), "aXbc");

        // 末尾与越界
        ASSERT_TRUE(My::File::insertAt(P("i.txt"), 4, "Y"));
        EXPECT_EQ(*My::File::readall(P("i.txt")), "aXbcY");
        EXPECT_FALSE(My::File::insertAt(P("i.txt"), 99, "Z"));
    }

    TEST_F(FileTest, InsertBeforeAfterLine)
    {
        // 无尾换行:在最后一行之后插入(回归:原实现此处会失败)
        WriteOk("i.txt", "a\nb\nc");
        ASSERT_TRUE(My::File::insertAfterLine(P("i.txt"), 3, "\nd"));
        EXPECT_EQ(*My::File::readall(P("i.txt")), "a\nb\nc\nd");

        // 第一行之前插入
        ASSERT_TRUE(My::File::insertBeforeLine(P("i.txt"), 1, "h\n"));
        EXPECT_EQ(*My::File::readall(P("i.txt")), "h\na\nb\nc\nd");

        // 中间行插入
        WriteOk("i2.txt", "1\n2\n3\n");
        ASSERT_TRUE(My::File::insertBeforeLine(P("i2.txt"), 3, "x\n"));
        EXPECT_EQ(*My::File::readall(P("i2.txt")), "1\n2\nx\n3\n");
        ASSERT_TRUE(My::File::insertAfterLine(P("i2.txt"), 2, "y\n"));
        EXPECT_EQ(*My::File::readall(P("i2.txt")), "1\n2\ny\nx\n3\n");

        // 非法行号
        EXPECT_FALSE(My::File::insertBeforeLine(P("i2.txt"), 0, "z"));
        EXPECT_FALSE(My::File::insertAfterLine(P("i2.txt"), 0, "z"));
        EXPECT_FALSE(My::File::insertBeforeLine(P("i2.txt"), 99, "z"));
        EXPECT_FALSE(My::File::insertAfterLine(P("i2.txt"), 99, "z"));
    }

    TEST_F(FileTest, DeleteLines)
    {
        WriteOk("d.txt", "1\n2\n3\n4\n");
        ASSERT_TRUE(My::File::deleteLine(P("d.txt"), 2));
        EXPECT_EQ(*My::File::readall(P("d.txt")), "1\n3\n4\n");

        ASSERT_TRUE(My::File::deleteLines(P("d.txt"), 1, 2));
        EXPECT_EQ(*My::File::readall(P("d.txt")), "4\n");

        // 无尾换行,删除最后一行(回归:原实现的边界分支)
        WriteOk("d2.txt", "a\nb\nc");
        ASSERT_TRUE(My::File::deleteLines(P("d2.txt"), 2, 3));
        EXPECT_EQ(*My::File::readall(P("d2.txt")), "a\n");

        // 删除尾换行文件的最后一行
        WriteOk("d3.txt", "abc\n");
        ASSERT_TRUE(My::File::deleteLine(P("d3.txt"), 1));
        EXPECT_EQ(*My::File::readall(P("d3.txt")), "");

        // 空文件删除第 1 行
        WriteOk("d4.txt", "");
        ASSERT_TRUE(My::File::deleteLine(P("d4.txt"), 1));
        EXPECT_EQ(*My::File::readall(P("d4.txt")), "");

        // 非法范围与超出范围
        EXPECT_FALSE(My::File::deleteLines(P("d.txt"), 0, 1));
        EXPECT_FALSE(My::File::deleteLines(P("d.txt"), 3, 2));
        EXPECT_FALSE(My::File::deleteLines(P("d.txt"), 1, 99));
    }

    // ==================== 平台相关 ====================

    TEST_F(FileTest, ChinesePathUtf8)
    {
        // UTF-8 中文路径与中文内容
        ASSERT_TRUE(My::File::writeAll(P("测试文件.txt"), "中文内容"));
        auto data = My::File::readall(P("测试文件.txt"));
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(*data, "中文内容");

        ASSERT_TRUE(My::File::remove(P("测试文件.txt")));
        EXPECT_FALSE(My::File::exists(P("测试文件.txt")));
    }

    // ==================== 性能(宽松阈值,防止慢机器误报) ====================

    TEST_F(FileTest, PerformanceReadAndWrite)
    {
        // 生成 20 万行、约 20MB 的文本文件
        constexpr size_t kLines = 200000;
        constexpr size_t kFileSize = kLines * 100;
        {
            const std::string line(99, 'x');
            My::File::Writer w = My::File::write(P("perf.txt"));
            for (size_t i = 0; i < kLines; ++i)
            {
                w.writeLine(line);
            }
            ASSERT_TRUE(w.commit());
        }

        // readall
        auto t0 = std::chrono::steady_clock::now();
        auto data = My::File::readall(P("perf.txt"));
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(data->size(), kFileSize);
        EXPECT_GT(MbPerSec(kFileSize, ms), 200.0);
        std::cout << "[perf] readall: " << MbPerSec(kFileSize, ms) << " MB/s\n";

        // lineCount
        t0 = std::chrono::steady_clock::now();
        auto n = My::File::lineCount(P("perf.txt"));
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(n.has_value());
        EXPECT_EQ(*n, kLines);
        EXPECT_GT(MbPerSec(kFileSize, ms), 100.0);
        std::cout << "[perf] lineCount: " << MbPerSec(kFileSize, ms) << " MB/s\n";

        // forEachLine
        t0 = std::chrono::steady_clock::now();
        size_t total = 0;
        const bool ok = My::File::forEachLine(P("perf.txt"), [&](size_t, std::string_view lv)
                                              {
            total += lv.size();
            return true; });
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_TRUE(ok);
        EXPECT_EQ(total, kFileSize - kLines); // 换行符不计入
        EXPECT_GT(MbPerSec(kFileSize, ms), 100.0);
        std::cout << "[perf] forEachLine: " << MbPerSec(kFileSize, ms) << " MB/s\n";

        // writeAll
        const std::string big(20 * 1024 * 1024, 'y');
        t0 = std::chrono::steady_clock::now();
        const bool wok = My::File::writeAll(P("perf.bin"), big);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_TRUE(wok);
        EXPECT_GT(MbPerSec(big.size(), ms), 50.0);
        std::cout << "[perf] writeAll: " << MbPerSec(big.size(), ms) << " MB/s\n";

        // readLine 中间行(顺序扫描)
        t0 = std::chrono::steady_clock::now();
        auto mid = My::File::readLine(P("perf.txt"), kLines / 2);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(mid.has_value());
        EXPECT_EQ(mid->size(), 99u);
        std::cout << "[perf] readLine(mid): " << ms << " ms\n";
    }

    // ==================== 新功能测试 ====================

    // #4 错误回调定制
    TEST_F(FileTest, ErrorHandler)
    {
        // 默认 handler 不为空
        EXPECT_NE(My::getErrorHandler(), nullptr);

        // 设置自定义 handler
        int callCount = 0;
        My::ErrorHandler custom = [](std::string_view, std::string_view, std::string_view) {};
        My::setErrorHandler(custom);
        EXPECT_EQ(My::getErrorHandler(), custom);

        // 触发错误时不打印到 stderr（由自定义 handler 接管）
        EXPECT_FALSE(My::File::readall(P("no_such_file.txt")).has_value());

        // 恢复默认
        My::setErrorHandler(nullptr);
        EXPECT_EQ(My::getErrorHandler(), nullptr);

        // nullptr 恢复默认后，错误仍正常返回
        EXPECT_FALSE(My::File::readall(P("no_such_file.txt")).has_value());

        // 清理：恢复默认 handler 以免影响其他用例
        My::setErrorHandler(My::ErrorHandler{});
        // 实际恢复默认（非 null）
        My::setErrorHandler([](std::string_view, std::string_view, std::string_view) {});
    }

    // #2 行索引缓存
    TEST_F(FileTest, LineIndexBasic)
    {
        WriteOk("li.txt", "aaa\nbbb\nccc\nddd");
        My::LineIndex idx(P("li.txt"));
        EXPECT_EQ(idx.lineCount(), 4u);

        // 随机行访问
        auto l1 = My::File::readLine(P("li.txt"), 1, idx);
        ASSERT_TRUE(l1.has_value());
        EXPECT_EQ(*l1, "aaa");

        auto l3 = My::File::readLine(P("li.txt"), 3, idx);
        ASSERT_TRUE(l3.has_value());
        EXPECT_EQ(*l3, "ccc");

        auto l4 = My::File::readLine(P("li.txt"), 4, idx);
        ASSERT_TRUE(l4.has_value());
        EXPECT_EQ(*l4, "ddd");

        // 行号越界
        EXPECT_FALSE(My::File::readLine(P("li.txt"), 0, idx).has_value());
        EXPECT_FALSE(My::File::readLine(P("li.txt"), 5, idx).has_value());

        // validate: 文件未变时应有效
        EXPECT_TRUE(idx.validate(P("li.txt")));

        // 修改文件后 validate 应失败（先等缓存窗口过期）
        WriteOk("li.txt", "changed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        EXPECT_FALSE(idx.validate(P("li.txt")));
    }

    // LineIndex 语义对齐: 空文件、尾换行、构造失败
    TEST_F(FileTest, LineIndexSemantics)
    {
        // 空文件: lineCount() 应为 0
        WriteOk("empty.txt", "");
        My::LineIndex idxEmpty(P("empty.txt"));
        EXPECT_TRUE(idxEmpty.valid());
        EXPECT_EQ(idxEmpty.lineCount(), 0u);
        EXPECT_EQ(My::File::lineCount(P("empty.txt")).value_or(999), 0u);

        // 尾换行: "a\n" 应为 1 行（与 File::lineCount 一致）
        WriteOk("trail.txt", "a\n");
        My::LineIndex idxTrail(P("trail.txt"));
        EXPECT_TRUE(idxTrail.valid());
        EXPECT_EQ(idxTrail.lineCount(), 1u);
        EXPECT_EQ(*My::File::lineCount(P("trail.txt")), 1u);

        // "a\nb\n" 应为 2 行
        WriteOk("trail2.txt", "a\nb\n");
        My::LineIndex idxTrail2(P("trail2.txt"));
        EXPECT_EQ(idxTrail2.lineCount(), 2u);

        // 无尾换行: "a\nb" 应为 2 行
        WriteOk("notrail.txt", "a\nb");
        My::LineIndex idxNoTrail(P("notrail.txt"));
        EXPECT_EQ(idxNoTrail.lineCount(), 2u);

        // 构造失败: 不存在的文件
        My::LineIndex idxBad(P("no_such_file.txt"));
        EXPECT_FALSE(idxBad.valid());
        EXPECT_EQ(idxBad.lineCount(), 0u);
        EXPECT_FALSE(idxBad.validate(P("no_such_file.txt")));

        // 索引版 readLine 对无效索引应返回 nullopt
        EXPECT_FALSE(My::File::readLine(P("trail.txt"), 1, idxBad).has_value());
    }

    // LineIndex 超长行（超过 128KB 块）
    TEST_F(FileTest, LineIndexLongLine)
    {
        const std::string longLine(200 * 1024, 'L'); // 200KB
        WriteOk("long_idx.txt", longLine + "\nshort");

        My::LineIndex idx(P("long_idx.txt"));
        EXPECT_TRUE(idx.valid());
        EXPECT_EQ(idx.lineCount(), 2u);

        // 索引版读取超长行
        auto r = My::File::readLine(P("long_idx.txt"), 1, idx);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->size(), longLine.size());
        EXPECT_EQ(*r, longLine);

        // 第二行
        auto r2 = My::File::readLine(P("long_idx.txt"), 2, idx);
        ASSERT_TRUE(r2.has_value());
        EXPECT_EQ(*r2, "short");
    }

    // #3 mmap 读取
    TEST_F(FileTest, ReadMapped)
    {
        WriteOk("mm.txt", "hello mmap world");
        auto mmf = My::File::readMapped(P("mm.txt"));
        ASSERT_TRUE(mmf.has_value());
        EXPECT_TRUE(mmf->isMapped());
        EXPECT_EQ(mmf->size(), 16u);
        EXPECT_EQ(mmf->view(), "hello mmap world");

        // 空文件：mmap 应返回空
        WriteOk("empty.txt", "");
        auto empty = My::File::readMapped(P("empty.txt"));
        EXPECT_FALSE(empty.has_value());

        // 不存在的文件
        auto nope = My::File::readMapped(P("nope.txt"));
        EXPECT_FALSE(nope.has_value());
    }

    // #7 Writer 自动提交
    TEST_F(FileTest, WriterAutoCommit)
    {
        {
            My::File::Writer w = My::File::write(P("ac.txt"));
            w.setAutoCommit(true).write("auto committed");
            // 析构时自动提交
        }
        auto data = My::File::readall(P("ac.txt"));
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(*data, "auto committed");

        // 未启用 autoCommit 时，析构不提交
        {
            My::File::Writer w = My::File::write(P("nac.txt"));
            w.write("should not appear");
        }
        EXPECT_FALSE(My::File::exists(P("nac.txt")));
    }

    // #8 目录遍历
    TEST_F(FileTest, ListWalkGlob)
    {
        WriteOk("a.txt", "a");
        WriteOk("b.txt", "b");
        WriteOk("c.log", "c");
        My::File::createDirectories(P("sub"));
        WriteOk("sub/d.txt", "d");

        // listFiles: 只列直接子文件
        auto files = My::File::listFiles(P(""));
        // listFiles 使用 kTestDir 作为 path，但 P("") 返回 "test_tmp/"
        // 改用 kTestDir 直接
        files = My::File::listFiles(kTestDir);
        EXPECT_GE(files.size(), 3u); // a.txt, b.txt, c.log (sub 是目录不算)

        // globFiles: 通配符匹配
        auto txts = My::File::globFiles(kTestDir, "*.txt");
        EXPECT_GE(txts.size(), 2u);
        auto logs = My::File::globFiles(kTestDir, "*.log");
        EXPECT_EQ(logs.size(), 1u);

        // walk: 递归遍历
        size_t totalEntries = 0;
        bool foundSub = false;
        My::File::walk(kTestDir, [&](std::string_view path, bool isDir)
                       {
            ++totalEntries;
            if (isDir && path.find("sub") != std::string_view::npos) foundSub = true;
            return true; });
        EXPECT_GE(totalEntries, 4u); // a.txt, b.txt, c.log, sub, sub/d.txt
        EXPECT_TRUE(foundSub);
    }

    // #9 文件哈希
    TEST_F(FileTest, FileHash)
    {
        // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        WriteOk("empty.txt", "");
        auto h = My::File::fileHash(P("empty.txt"));
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(*h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
        WriteOk("abc.txt", "abc");
        auto h2 = My::File::fileHash(P("abc.txt"));
        ASSERT_TRUE(h2.has_value());
        EXPECT_EQ(*h2, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        // 不存在的文件
        EXPECT_FALSE(My::File::fileHash(P("nope.txt")).has_value());
    }

    // SHA-256 NIST 标准向量补充
    TEST_F(FileTest, FileHashNistVectors)
    {
        // SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        // = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
        WriteOk("nist1.txt", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        auto h1 = My::File::fileHash(P("nist1.txt"));
        ASSERT_TRUE(h1.has_value());
        EXPECT_EQ(*h1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

        // SHA-256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
        // = cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1
        WriteOk("nist2.txt", "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu");
        auto h2 = My::File::fileHash(P("nist2.txt"));
        ASSERT_TRUE(h2.has_value());
        EXPECT_EQ(*h2, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    }

    // 中文文件名目录列举
    TEST_F(FileTest, ChinesePathDirectoryList)
    {
        WriteOk("\xe6\xb5\x8b\xe8\xaf\x95.txt", "content"); // 测试.txt
        WriteOk("normal.txt", "normal");

        auto files = My::File::listFiles(kTestDir);
        EXPECT_GE(files.size(), 2u);

        // 检查中文文件名是否在列表中
        bool foundChinese = false;
        for (const auto &f : files)
        {
            if (f.find("\xe6\xb5\x8b\xe8\xaf\x95") != std::string::npos)
            {
                foundChinese = true;
                break;
            }
        }
        EXPECT_TRUE(foundChinese) << "中文文件名未在 listFiles 结果中找到";
    }

    // #6 异步 IO
    TEST_F(FileTest, AsyncReadWrite)
    {
        WriteOk("async.txt", "async content");

        // asyncReadall
        auto fut = My::File::asyncReadall(P("async.txt"));
        auto data = fut.get();
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(*data, "async content");

        // asyncWriteAll
        auto wfut = My::File::asyncWriteAll(P("async2.txt"), "written async");
        EXPECT_TRUE(wfut.get());
        auto d2 = My::File::readall(P("async2.txt"));
        ASSERT_TRUE(d2.has_value());
        EXPECT_EQ(*d2, "written async");

        // 并发读取
        std::vector<std::future<std::optional<std::string>>> futs;
        for (int i = 0; i < 8; ++i)
        {
            futs.push_back(My::File::asyncReadall(P("async.txt")));
        }
        for (auto &f : futs)
        {
            auto r = f.get();
            ASSERT_TRUE(r.has_value());
            EXPECT_EQ(*r, "async content");
        }
    }

    // #10 文件监听（仅 Windows/Linux 支持，macOS 桩返回 false）
#if defined(_WIN32) || defined(__linux__)
    TEST_F(FileTest, FileWatcherBasic)
    {
        My::FileWatcher watcher;
        EXPECT_FALSE(watcher.isWatching());

        std::mutex mtx;
        std::vector<std::pair<std::string, My::FileEvent>> events;

        ASSERT_TRUE(watcher.start(kTestDir, [&](std::string_view path, My::FileEvent ev)
                                  {
            std::lock_guard lock(mtx);
            events.emplace_back(std::string(path), ev); }));
        EXPECT_TRUE(watcher.isWatching());

        // 等待工作线程发出首次 ReadDirectoryChangesW
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 创建文件触发事件
        WriteOk("watched.txt", "hello");

        // 轮询等待事件（最多 2 秒）
        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard lock(mtx);
            if (!events.empty())
                break;
        }

        watcher.stop();
        EXPECT_FALSE(watcher.isWatching());

        // 至少应收到一个事件
        std::lock_guard lock(mtx);
        EXPECT_GE(events.size(), 1u);
    }
#endif // _WIN32 || __linux__
    // ==================== 压力测试 ====================

    // 压测：多线程并发读写多个大文件
    TEST_F(FileTest, Stress_ConcurrentLargeFileReadWrite)
    {
        constexpr int kFiles = 8;
        constexpr size_t kSize = 4 * 1024 * 1024; // 每文件 4MB

        // 准备测试文件
        const std::string payload(kSize, 'A');
        for (int i = 0; i < kFiles; ++i)
        {
            WriteOk("stress_" + std::to_string(i) + ".bin", payload);
        }

        // 并发读取全部文件
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        std::atomic<int> readOk{0};
        for (int i = 0; i < kFiles; ++i)
        {
            threads.emplace_back([&, i]()
                                 {
                for (int rep = 0; rep < 5; ++rep)
                {
                    auto data = My::File::readall(P("stress_" + std::to_string(i) + ".bin"));
                    if (data && data->size() == kSize) readOk.fetch_add(1, std::memory_order_relaxed);
                } });
        }
        for (auto &t : threads)
            t.join();
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        EXPECT_EQ(readOk.load(), kFiles * 5);
        std::cout << "[stress] concurrent read " << kFiles << " files x 5 reps: " << ms << " ms\n";

        // 并发写入全部文件
        threads.clear();
        std::atomic<int> writeOk{0};
        const std::string newPayload(kSize, 'B');
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFiles; ++i)
        {
            threads.emplace_back([&, i]()
                                 {
                bool ok = My::File::writeAll(P("stress_" + std::to_string(i) + ".bin"), newPayload);
                if (ok) writeOk.fetch_add(1, std::memory_order_relaxed); });
        }
        for (auto &t : threads)
            t.join();
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        EXPECT_EQ(writeOk.load(), kFiles);
        std::cout << "[stress] concurrent write " << kFiles << " files: " << ms << " ms\n";

        // 验证内容一致
        for (int i = 0; i < kFiles; ++i)
        {
            auto data = My::File::readall(P("stress_" + std::to_string(i) + ".bin"));
            ASSERT_TRUE(data.has_value());
            EXPECT_EQ(data->size(), kSize);
        }
    }

    // 压测：高频小文件操作（创建/读取/删除）
    TEST_F(FileTest, Stress_HighFreqSmallFile)
    {
        constexpr int kCount = 500;

        // 批量创建
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCount; ++i)
        {
            ASSERT_TRUE(My::File::writeAll(P("sf_" + std::to_string(i) + ".txt"),
                                           "content_" + std::to_string(i)));
        }
        auto createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[stress] create " << kCount << " small files: " << createMs << " ms\n";

        // 批量读取
        t0 = std::chrono::steady_clock::now();
        int readOk = 0;
        for (int i = 0; i < kCount; ++i)
        {
            auto data = My::File::readall(P("sf_" + std::to_string(i) + ".txt"));
            if (data && *data == "content_" + std::to_string(i))
                ++readOk;
        }
        auto readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(readOk, kCount);
        std::cout << "[stress] read " << kCount << " small files: " << readMs << " ms\n";

        // 批量删除
        t0 = std::chrono::steady_clock::now();
        int delOk = 0;
        for (int i = 0; i < kCount; ++i)
        {
            if (My::File::remove(P("sf_" + std::to_string(i) + ".txt")))
                ++delOk;
        }
        auto delMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(delOk, kCount);
        std::cout << "[stress] delete " << kCount << " small files: " << delMs << " ms\n";
    }

    // 压测：LineIndex 随机行访问（对比无索引性能）
    TEST_F(FileTest, Stress_LineIndexRandomAccess)
    {
        constexpr size_t kLines = 50000;
        constexpr int kAccesses = 1000;

        // 生成 5 万行文件
        {
            const std::string line(99, 'x');
            My::File::Writer w = My::File::write(P("idx_perf.txt"));
            for (size_t i = 0; i < kLines; ++i)
            {
                w.writeLine(line + std::to_string(i));
            }
            ASSERT_TRUE(w.commit());
        }

        // 生成随机行号
        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> dist(1, kLines);
        std::vector<size_t> targets;
        for (int i = 0; i < kAccesses; ++i)
        {
            targets.push_back(dist(rng));
        }

        // 无索引：顺序扫描
        auto t0 = std::chrono::steady_clock::now();
        int noIdxOk = 0;
        for (auto ln : targets)
        {
            auto r = My::File::readLine(P("idx_perf.txt"), ln);
            if (r)
                ++noIdxOk;
        }
        auto noIdxMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        // 有索引：O(log N) 定位
        My::LineIndex idx(P("idx_perf.txt"));
        auto t1 = std::chrono::steady_clock::now();
        int idxOk = 0;
        for (auto ln : targets)
        {
            auto r = My::File::readLine(P("idx_perf.txt"), ln, idx);
            if (r)
                ++idxOk;
        }
        auto idxMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

        EXPECT_EQ(noIdxOk, kAccesses);
        EXPECT_EQ(idxOk, kAccesses);
        EXPECT_LT(idxMs, noIdxMs); // 索引应更快

        std::cout << "[stress] readLine x" << kAccesses << " without index: " << noIdxMs << " ms\n";
        std::cout << "[stress] readLine x" << kAccesses << " with LineIndex: " << idxMs << " ms\n";
        std::cout << "[stress] speedup: " << (noIdxMs / std::max(idxMs, 0.1)) << "x\n";
    }

    // 压测：mmap 大文件反复读取
    TEST_F(FileTest, Stress_MmapLargeFile)
    {
        constexpr size_t kSize = 10 * 1024 * 1024; // 10MB
        const std::string payload(kSize, 'M');
        ASSERT_TRUE(My::File::writeAll(P("mmap_stress.bin"), payload));

        // 反复 mmap 读取
        constexpr int kReps = 20;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i)
        {
            auto mmf = My::File::readMapped(P("mmap_stress.bin"));
            ASSERT_TRUE(mmf.has_value());
            EXPECT_EQ(mmf->size(), kSize);
        }
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[stress] mmap read " << kReps << " x 10MB: " << ms << " ms ("
                  << MbPerSec(kSize * kReps, ms) << " MB/s)\n";

        // 对比 readall
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i)
        {
            auto data = My::File::readall(P("mmap_stress.bin"));
            ASSERT_TRUE(data.has_value());
            EXPECT_EQ(data->size(), kSize);
        }
        auto readallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[stress] readall " << kReps << " x 10MB: " << readallMs << " ms ("
                  << MbPerSec(kSize * kReps, readallMs) << " MB/s)\n";
    }

    // 压测：异步 IO 高并发
    TEST_F(FileTest, Stress_AsyncIOConcurrency)
    {
        constexpr int kFiles = 16;
        constexpr int kOpsPerFile = 4;

        // 准备文件
        for (int i = 0; i < kFiles; ++i)
        {
            WriteOk("async_s_" + std::to_string(i) + ".txt",
                    std::string(1024, 'A' + (i % 26)));
        }

        // 同时提交大量异步读
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::future<std::optional<std::string>>> futures;
        for (int i = 0; i < kFiles; ++i)
        {
            for (int j = 0; j < kOpsPerFile; ++j)
            {
                futures.push_back(My::File::asyncReadall(P("async_s_" + std::to_string(i) + ".txt")));
            }
        }

        int ok = 0;
        for (auto &f : futures)
        {
            auto r = f.get();
            if (r && r->size() == 1024)
                ++ok;
        }
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(ok, kFiles * kOpsPerFile);
        std::cout << "[stress] async read " << kFiles * kOpsPerFile << " ops: " << ms << " ms\n";

        // 并发异步写
        futures.clear();
        std::vector<std::future<bool>> wfuts;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFiles; ++i)
        {
            for (int j = 0; j < kOpsPerFile; ++j)
            {
                wfuts.push_back(My::File::asyncWriteAll(
                    P("async_w_" + std::to_string(i) + "_" + std::to_string(j) + ".txt"),
                    std::string(512, 'W')));
            }
        }
        int wok = 0;
        for (auto &f : wfuts)
        {
            if (f.get())
                ++wok;
        }
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(wok, kFiles * kOpsPerFile);
        std::cout << "[stress] async write " << kFiles * kOpsPerFile << " ops: " << ms << " ms\n";
    }

    // ==================== 混合测试 ====================

    // 混合：多线程交替读写同一文件
    TEST_F(FileTest, Mixed_ConcurrentReadWriteSameFile)
    {
        WriteOk("mixed_rw.txt", "initial");

        std::atomic<bool> stop{false};
        std::atomic<int> writeCount{0};
        std::atomic<int> readCount{0};

        // 写线程：反复覆盖写入
        std::thread writer([&]()
                           {
            for (int i = 0; i < 50; ++i)
            {
                My::File::writeAll(P("mixed_rw.txt"), "iteration_" + std::to_string(i));
                writeCount.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            stop.store(true, std::memory_order_release); });

        // 读线程：反复读取（允许读到旧内容，但不应崩溃或读到损坏数据）
        std::thread reader([&]()
                           {
            while (!stop.load(std::memory_order_acquire))
            {
                auto data = My::File::readall(P("mixed_rw.txt"));
                if (data && !data->empty()) readCount.fetch_add(1, std::memory_order_relaxed);
            } });

        writer.join();
        reader.join();

        EXPECT_EQ(writeCount.load(), 50);
        EXPECT_GT(readCount.load(), 0);
        std::cout << "[mixed] concurrent read/write: reads=" << readCount.load()
                  << " writes=" << writeCount.load() << "\n";

        // 最终文件内容应完整（最后一次写入）
        auto final_data = My::File::readall(P("mixed_rw.txt"));
        ASSERT_TRUE(final_data.has_value());
        EXPECT_EQ(*final_data, "iteration_49");
    }

    // 混合：文件监听 + 多文件写入
#if defined(_WIN32) || defined(__linux__)
    TEST_F(FileTest, Mixed_WatcherAndMultiFileWriter)
    {
        My::File::createDirectories(P("watch_mix"));

        My::FileWatcher watcher;
        std::mutex mtx;
        std::vector<std::pair<std::string, My::FileEvent>> events;

        ASSERT_TRUE(watcher.start(P("watch_mix"), [&](std::string_view path, My::FileEvent ev)
                                  {
            std::lock_guard lock(mtx);
            events.emplace_back(std::string(path), ev); }));

        // 等待工作线程发出首次 ReadDirectoryChangesW
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 在监听目录下创建多个文件
        for (int i = 0; i < 5; ++i)
        {
            ASSERT_TRUE(My::File::writeAll(P("watch_mix/f_" + std::to_string(i) + ".txt"),
                                           "data_" + std::to_string(i)));
        }

        // 等待事件
        for (int i = 0; i < 40; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard lock(mtx);
            if (events.size() >= 5)
                break;
        }

        watcher.stop();

        std::lock_guard lock(mtx);
        EXPECT_GE(events.size(), 5u);
        std::cout << "[mixed] watcher received " << events.size() << " events for 5 file creations\n";
    }
#endif // _WIN32 || __linux__

    // 混合：行索引 + 行编辑（验证索引失效与重建）
    TEST_F(FileTest, Mixed_LineIndexAndLineEdit)
    {
        // 生成文件（无尾换行，避免 LineIndex 尾部空行计数差异）
        {
            My::File::Writer w = My::File::write(P("mix_idx.txt"));
            for (int i = 0; i < 100; ++i)
            {
                if (i > 0)
                    w.write("\n");
                w.write("line_" + std::to_string(i));
            }
            ASSERT_TRUE(w.commit());
        }

        // 建立索引
        My::LineIndex idx(P("mix_idx.txt"));
        EXPECT_EQ(idx.lineCount(), 100u);
        EXPECT_TRUE(idx.validate(P("mix_idx.txt")));

        // 用索引读取
        auto r1 = My::File::readLine(P("mix_idx.txt"), 50, idx);
        ASSERT_TRUE(r1.has_value());
        EXPECT_EQ(*r1, "line_49");

        // 行编辑：在第 50 行前插入
        ASSERT_TRUE(My::File::insertBeforeLine(P("mix_idx.txt"), 50, "inserted_line\n"));

        // 索引应失效（文件已修改，等缓存窗口过期）
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        EXPECT_FALSE(idx.validate(P("mix_idx.txt")));

        // 重建索引
        My::LineIndex idx2(P("mix_idx.txt"));
        EXPECT_EQ(idx2.lineCount(), 101u);

        // 用新索引读取
        auto r2 = My::File::readLine(P("mix_idx.txt"), 50, idx2);
        ASSERT_TRUE(r2.has_value());
        EXPECT_EQ(*r2, "inserted_line");

        auto r3 = My::File::readLine(P("mix_idx.txt"), 51, idx2);
        ASSERT_TRUE(r3.has_value());
        EXPECT_EQ(*r3, "line_49");

        // 删除行后索引再次失效
        ASSERT_TRUE(My::File::deleteLine(P("mix_idx.txt"), 50));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        EXPECT_FALSE(idx2.validate(P("mix_idx.txt")));

        My::LineIndex idx3(P("mix_idx.txt"));
        EXPECT_EQ(idx3.lineCount(), 100u);
    }

    // 混合：目录遍历 + 文件操作
    TEST_F(FileTest, Mixed_DirectoryWalkAndOps)
    {
        // 创建目录结构
        My::File::createDirectories(P("walk/a"));
        My::File::createDirectories(P("walk/b/c"));
        WriteOk("walk/f1.txt", "hello");
        WriteOk("walk/f2.txt", "world");
        WriteOk("walk/a/f3.txt", "aaa");
        WriteOk("walk/b/f4.txt", "bbb");
        WriteOk("walk/b/c/f5.txt", "ccc");

        // 遍历并对每个文件计算哈希
        int fileCount = 0;
        std::vector<std::string> hashes;
        My::File::walk(P("walk"), [&](std::string_view path, bool isDir)
                       {
            if (!isDir)
            {
                ++fileCount;
                auto h = My::File::fileHash(std::string(path));
                if (h) hashes.push_back(*h);
            }
            return true; });

        EXPECT_EQ(fileCount, 5);
        EXPECT_EQ(hashes.size(), 5u);

        // 遍历并对每个文件用异步读取
        std::vector<std::future<std::optional<std::string>>> futures;
        My::File::walk(P("walk"), [&](std::string_view path, bool isDir)
                       {
            if (!isDir)
            {
                futures.push_back(My::File::asyncReadall(std::string(path)));
            }
            return true; });

        int asyncOk = 0;
        for (auto &f : futures)
        {
            if (f.get())
                ++asyncOk;
        }
        EXPECT_EQ(asyncOk, 5);

        // glob 匹配 + 读取验证
        auto txts = My::File::globFiles(P("walk"), "*.txt");
        EXPECT_GE(txts.size(), 2u); // 至少直接子文件
    }

    // 混合：原子写 + 哈希 + 异步 一致性验证
    TEST_F(FileTest, Mixed_HashAtomicAsyncConsistency)
    {
        // 原子写入 -> 立即哈希 -> 异步读取 -> 对比哈希
        const std::string content = "consistency_test_data_12345";

        ASSERT_TRUE(My::File::writeAllAtomic(P("hash_test.txt"), content));

        // 同步哈希
        auto syncHash = My::File::fileHash(P("hash_test.txt"));
        ASSERT_TRUE(syncHash.has_value());

        // 异步读取后计算哈希
        auto fut = My::File::asyncReadall(P("hash_test.txt"));
        auto asyncData = fut.get();
        ASSERT_TRUE(asyncData.has_value());
        EXPECT_EQ(*asyncData, content);

        // 再次同步读取验证一致性
        auto syncData = My::File::readall(P("hash_test.txt"));
        ASSERT_TRUE(syncData.has_value());
        EXPECT_EQ(*syncData, content);

        // 多次原子写后哈希应变化
        ASSERT_TRUE(My::File::writeAllAtomic(P("hash_test.txt"), "different_content"));
        auto newHash = My::File::fileHash(P("hash_test.txt"));
        ASSERT_TRUE(newHash.has_value());
        EXPECT_NE(*syncHash, *newHash);
    }

    // 混合：全功能综合场景
    TEST_F(FileTest, Mixed_AllFeaturesCombined)
    {
        // 先重置错误回调（前面用例可能已替换）
        static std::atomic<int> sErrorCount{0};
        sErrorCount.store(0);
        My::setErrorHandler([](std::string_view, std::string_view, std::string_view)
                            { sErrorCount.fetch_add(1, std::memory_order_relaxed); });

        // 1. Writer 链式写入大文件（无尾换行）
        {
            My::File::Writer w = My::File::write(P("all_feat.txt"));
            w.setAutoCommit(true).reserve(4096);
            for (int i = 0; i < 200; ++i)
            {
                if (i > 0)
                    w.write("\n");
                w.write("record_" + std::to_string(i));
            }
        }

        // 2. 建立行索引
        My::LineIndex idx(P("all_feat.txt"));
        EXPECT_EQ(idx.lineCount(), 200u);

        // 3. 用索引随机读取
        auto r1 = My::File::readLine(P("all_feat.txt"), 100, idx);
        ASSERT_TRUE(r1.has_value());
        EXPECT_EQ(*r1, "record_99");

        // 4. mmap 读取并验证
        auto mmf = My::File::readMapped(P("all_feat.txt"));
        ASSERT_TRUE(mmf.has_value());
        EXPECT_GT(mmf->size(), 0u);

        // 5. 文件哈希
        auto hash = My::File::fileHash(P("all_feat.txt"));
        ASSERT_TRUE(hash.has_value());
        EXPECT_EQ(hash->size(), 64u); // SHA-256 hex

        // 6. 异步读取
        auto fut = My::File::asyncReadall(P("all_feat.txt"));
        auto asyncData = fut.get();
        ASSERT_TRUE(asyncData.has_value());

        // 7. 目录遍历确认文件存在
        auto files = My::File::listFiles(kTestDir);
        EXPECT_GE(files.size(), 1u);

        // 8. 触发一些错误（不存在的文件，readall/readLine/lineCount 均会触发回调）
        EXPECT_FALSE(My::File::readall(P("nonexistent.txt")).has_value());
        EXPECT_FALSE(My::File::readLine(P("nonexistent.txt"), 1).has_value());
        EXPECT_FALSE(My::File::lineCount(P("nonexistent.txt")).has_value());
        EXPECT_GE(sErrorCount.load(), 3);

        // 恢复默认回调
        My::setErrorHandler(My::ErrorHandler{});
        My::setErrorHandler([](std::string_view, std::string_view, std::string_view) {});

        std::cout << "[mixed] all features combined: OK (errors captured: " << sErrorCount.load() << ")\n";
    }

    // 混合：Writer 自动提交 + 手动提交混合
    TEST_F(FileTest, Mixed_WriterAutoAndManualCommit)
    {
        // 自动提交
        {
            My::File::Writer w = My::File::write(P("wm1.txt"));
            w.setAutoCommit(true).write("auto");
        }
        auto d1 = My::File::readall(P("wm1.txt"));
        ASSERT_TRUE(d1.has_value());
        EXPECT_EQ(*d1, "auto");

        // 手动提交
        {
            My::File::Writer w = My::File::write(P("wm2.txt"));
            w.write("manual");
            ASSERT_TRUE(w.commit());
        }
        auto d2 = My::File::readall(P("wm2.txt"));
        ASSERT_TRUE(d2.has_value());
        EXPECT_EQ(*d2, "manual");

        // 追加模式 + 自动提交
        {
            My::File::Writer w = My::File::write(P("wm1.txt"), true);
            w.setAutoCommit(true).write("_appended");
        }
        auto d3 = My::File::readall(P("wm1.txt"));
        ASSERT_TRUE(d3.has_value());
        EXPECT_EQ(*d3, "auto_appended");

        // insert 模式 + 手动提交
        ASSERT_TRUE(My::File::insert(P("wm2.txt")).insertBeforeLine(1, "prefix_").commit());
        auto d4 = My::File::readall(P("wm2.txt"));
        ASSERT_TRUE(d4.has_value());
        EXPECT_EQ(*d4, "prefix_manual");
    }

    // ==================== v1.2.0 新 API 测试 ====================

    // head：读取前 N 行
    TEST_F(FileTest, V120_HeadBasic)
    {
        // 生成 10 行文件
        {
            My::File::Writer w = My::File::write(P("head.txt"));
            for (int i = 1; i <= 10; ++i)
                w.writeLine("line_" + std::to_string(i));
            ASSERT_TRUE(w.commit());
        }

        // 读前 3 行
        auto r = My::File::head(P("head.txt"), 3);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 3u);
        EXPECT_EQ((*r)[0], "line_1");
        EXPECT_EQ((*r)[1], "line_2");
        EXPECT_EQ((*r)[2], "line_3");

        // 读前 0 行
        auto r0 = My::File::head(P("head.txt"), 0);
        ASSERT_TRUE(r0.has_value());
        EXPECT_TRUE(r0->empty());

        // 读超过文件行数
        auto rAll = My::File::head(P("head.txt"), 100);
        ASSERT_TRUE(rAll.has_value());
        EXPECT_EQ(rAll->size(), 10u);

        // 不存在的文件
        auto rBad = My::File::head(P("no_such.txt"), 5);
        EXPECT_FALSE(rBad.has_value());
    }

    // tail：读取后 N 行
    TEST_F(FileTest, V120_TailBasic)
    {
        // 生成 10 行文件（每行以 \n 结尾）
        {
            My::File::Writer w = My::File::write(P("tail.txt"));
            for (int i = 1; i <= 10; ++i)
                w.writeLine("line_" + std::to_string(i));
            ASSERT_TRUE(w.commit());
        }

        // 读后 3 行
        auto r = My::File::tail(P("tail.txt"), 3);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 3u);
        EXPECT_EQ((*r)[0], "line_8");
        EXPECT_EQ((*r)[1], "line_9");
        EXPECT_EQ((*r)[2], "line_10");

        // 读后 0 行
        auto r0 = My::File::tail(P("tail.txt"), 0);
        ASSERT_TRUE(r0.has_value());
        EXPECT_TRUE(r0->empty());

        // 读超过文件行数
        auto rAll = My::File::tail(P("tail.txt"), 100);
        ASSERT_TRUE(rAll.has_value());
        EXPECT_EQ(rAll->size(), 10u);

        // 空文件
        WriteOk("empty.txt", "");
        auto rEmpty = My::File::tail(P("empty.txt"), 5);
        ASSERT_TRUE(rEmpty.has_value());
        EXPECT_TRUE(rEmpty->empty());
    }

    // tail：文件不以换行结尾
    TEST_F(FileTest, V120_TailNoTrailingNewline)
    {
        // 写入无尾换行的内容
        WriteOk("tail_nonl.txt", "aaa\nbbb\nccc");

        auto r = My::File::tail(P("tail_nonl.txt"), 2);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 2u);
        EXPECT_EQ((*r)[0], "bbb");
        EXPECT_EQ((*r)[1], "ccc");

        // 读全部
        auto rAll = My::File::tail(P("tail_nonl.txt"), 10);
        ASSERT_TRUE(rAll.has_value());
        ASSERT_EQ(rAll->size(), 3u);
        EXPECT_EQ((*rAll)[0], "aaa");
    }

    // readRange：部分读取
    TEST_F(FileTest, V120_ReadRangeBasic)
    {
        WriteOk("range.txt", "0123456789ABCDEF");

        // 从偏移 5 读 4 字节
        auto r = My::File::readRange(P("range.txt"), 5, 4);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, "5678");

        // 从偏移 0 读到末尾（len=0）
        auto rAll = My::File::readRange(P("range.txt"), 0, 0);
        ASSERT_TRUE(rAll.has_value());
        EXPECT_EQ(*rAll, "0123456789ABCDEF");

        // 从偏移 10 读到末尾
        auto rEnd = My::File::readRange(P("range.txt"), 10, 0);
        ASSERT_TRUE(rEnd.has_value());
        EXPECT_EQ(*rEnd, "ABCDEF");

        // 偏移超出文件
        auto rBad = My::File::readRange(P("range.txt"), 100, 5);
        EXPECT_FALSE(rBad.has_value());

        // len 超出文件范围应截断
        auto rClip = My::File::readRange(P("range.txt"), 14, 100);
        ASSERT_TRUE(rClip.has_value());
        EXPECT_EQ(*rClip, "EF");
    }

    // filesEqual：文件比较
    TEST_F(FileTest, V120_FilesEqualBasic)
    {
        WriteOk("eq_a.txt", "hello world");
        WriteOk("eq_b.txt", "hello world");
        WriteOk("eq_c.txt", "different");

        EXPECT_TRUE(My::File::filesEqual(P("eq_a.txt"), P("eq_b.txt")));
        EXPECT_FALSE(My::File::filesEqual(P("eq_a.txt"), P("eq_c.txt")));

        // 自身比较
        EXPECT_TRUE(My::File::filesEqual(P("eq_a.txt"), P("eq_a.txt")));

        // 空文件
        WriteOk("eq_empty1.txt", "");
        WriteOk("eq_empty2.txt", "");
        EXPECT_TRUE(My::File::filesEqual(P("eq_empty1.txt"), P("eq_empty2.txt")));
        EXPECT_FALSE(My::File::filesEqual(P("eq_a.txt"), P("eq_empty1.txt")));

        // 不存在的文件
        EXPECT_FALSE(My::File::filesEqual(P("eq_a.txt"), P("no_such.txt")));
    }

    // filesEqual：大文件块比较
    TEST_F(FileTest, V120_FilesEqualLarge)
    {
        const std::string big(256 * 1024, 'X'); // 256KB
        WriteOk("big_a.bin", big);
        WriteOk("big_b.bin", big);
        EXPECT_TRUE(My::File::filesEqual(P("big_a.bin"), P("big_b.bin")));

        // 修改最后一个字节
        std::string modified = big;
        modified.back() = 'Y';
        WriteOk("big_c.bin", modified);
        EXPECT_FALSE(My::File::filesEqual(P("big_a.bin"), P("big_c.bin")));
    }

    // fileCrc32：CRC32 哈希
    TEST_F(FileTest, V120_FileCrc32Basic)
    {
        WriteOk("crc.txt", "hello");
        auto h = My::File::fileCrc32(P("crc.txt"));
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(h->size(), 8u); // CRC32 = 4 bytes = 8 hex chars

        // 同一内容哈希一致
        WriteOk("crc2.txt", "hello");
        auto h2 = My::File::fileCrc32(P("crc2.txt"));
        ASSERT_TRUE(h2.has_value());
        EXPECT_EQ(*h, *h2);

        // 不同内容哈希不同
        WriteOk("crc3.txt", "world");
        auto h3 = My::File::fileCrc32(P("crc3.txt"));
        ASSERT_TRUE(h3.has_value());
        EXPECT_NE(*h, *h3);

        // 空文件
        WriteOk("crc_empty.txt", "");
        auto he = My::File::fileCrc32(P("crc_empty.txt"));
        ASSERT_TRUE(he.has_value());
        EXPECT_EQ(*he, "00000000");
    }

    // fileXxHash64：xxHash64 哈希
    TEST_F(FileTest, V120_FileXxHash64Basic)
    {
        WriteOk("xxh.txt", "hello");
        auto h = My::File::fileXxHash64(P("xxh.txt"));
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(h->size(), 16u); // xxHash64 = 8 bytes = 16 hex chars

        // 同一内容哈希一致
        WriteOk("xxh2.txt", "hello");
        auto h2 = My::File::fileXxHash64(P("xxh2.txt"));
        ASSERT_TRUE(h2.has_value());
        EXPECT_EQ(*h, *h2);

        // 不同内容哈希不同
        WriteOk("xxh3.txt", "world");
        auto h3 = My::File::fileXxHash64(P("xxh3.txt"));
        ASSERT_TRUE(h3.has_value());
        EXPECT_NE(*h, *h3);
    }

    // Hasher：增量哈希 — SHA-256
    TEST_F(FileTest, V120_HasherSha256)
    {
        // 一次性计算
        WriteOk("hsha.txt", "hello world");
        auto oneShot = My::File::fileHash(P("hsha.txt"));
        ASSERT_TRUE(oneShot.has_value());

        // 增量计算（分两次 update）
        My::Hasher hasher(My::Hasher::Algorithm::Sha256);
        hasher.update("hello ");
        hasher.update("world");
        auto incremental = hasher.finalize();

        EXPECT_EQ(incremental, *oneShot);
        EXPECT_EQ(incremental.size(), 64u);
    }

    // Hasher：增量哈希 — CRC32
    TEST_F(FileTest, V120_HasherCrc32)
    {
        WriteOk("hcrc.txt", "test data");
        auto oneShot = My::File::fileCrc32(P("hcrc.txt"));
        ASSERT_TRUE(oneShot.has_value());

        My::Hasher hasher(My::Hasher::Algorithm::Crc32);
        hasher.update("test ");
        hasher.update("data");
        auto incremental = hasher.finalize();

        EXPECT_EQ(incremental, *oneShot);
        EXPECT_EQ(incremental.size(), 8u);

        // 官方向量：CRC32("123456789") = 0xcbf43926
        My::Hasher h2(My::Hasher::Algorithm::Crc32);
        h2.update("123456789");
        EXPECT_EQ(h2.finalize(), "cbf43926");
    }

    // Hasher：增量哈希 — xxHash64
    TEST_F(FileTest, V120_HasherXxHash64)
    {
        WriteOk("hxxh.txt", "test data 12345");
        auto oneShot = My::File::fileXxHash64(P("hxxh.txt"));
        ASSERT_TRUE(oneShot.has_value());

        My::Hasher hasher(My::Hasher::Algorithm::XxHash64);
        hasher.update("test ");
        hasher.update("data ");
        hasher.update("12345");
        auto incremental = hasher.finalize();

        EXPECT_EQ(incremental, *oneShot);
        EXPECT_EQ(incremental.size(), 16u);

        // 官方向量：xxHash64("") = ef46db3751d8e999, xxHash64("abc") = 44bc2cf5ad770999
        My::Hasher h2(My::Hasher::Algorithm::XxHash64);
        h2.update("");
        EXPECT_EQ(h2.finalize(), "ef46db3751d8e999");

        My::Hasher h3(My::Hasher::Algorithm::XxHash64);
        h3.update("abc");
        EXPECT_EQ(h3.finalize(), "44bc2cf5ad770999");
    }

    // Hasher：finalize 后开始新计算（不是拼接追加）
    TEST_F(FileTest, V120_HasherResetAfterFinalize)
    {
        My::Hasher hasher(My::Hasher::Algorithm::Crc32);
        hasher.update("abc");
        auto h1 = hasher.finalize();

        // finalize 后内部重置，继续 update 开始新计算，同样内容应得到相同结果
        hasher.update("abc");
        auto h2 = hasher.finalize();
        EXPECT_EQ(h1, h2);

        // 拼接不同内容应得到不同结果（确认不是追加）
        hasher.update("xyz");
        auto h3 = hasher.finalize();
        EXPECT_NE(h1, h3);
    }

    // Hasher：string_view 重载
    TEST_F(FileTest, V120_HasherStringView)
    {
        My::Hasher h1(My::Hasher::Algorithm::Sha256);
        std::string_view sv = "hello world";
        h1.update(sv);
        auto r1 = h1.finalize();

        My::Hasher h2(My::Hasher::Algorithm::Sha256);
        h2.update("hello ");
        h2.update("world");
        auto r2 = h2.finalize();

        EXPECT_EQ(r1, r2);
    }

    // v1.2.0 综合场景：head + tail + readRange + filesEqual + 哈希
    TEST_F(FileTest, V120_AllNewApisCombined)
    {
        // 生成 50 行文件
        {
            My::File::Writer w = My::File::write(P("combo.txt"));
            for (int i = 1; i <= 50; ++i)
                w.writeLine("row_" + std::to_string(i));
            ASSERT_TRUE(w.commit());
        }

        // head + tail 覆盖首尾
        auto h = My::File::head(P("combo.txt"), 5);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ((*h)[0], "row_1");
        EXPECT_EQ((*h)[4], "row_5");

        auto t = My::File::tail(P("combo.txt"), 5);
        ASSERT_TRUE(t.has_value());
        EXPECT_EQ((*t)[0], "row_46");
        EXPECT_EQ((*t)[4], "row_50");

        // readRange 读取字节片段
        auto rr = My::File::readRange(P("combo.txt"), 0, 5);
        ASSERT_TRUE(rr.has_value());
        EXPECT_EQ(*rr, "row_1");

        // filesEqual：复制后应相等
        ASSERT_TRUE(My::File::copy(P("combo.txt"), P("combo_copy.txt")));
        EXPECT_TRUE(My::File::filesEqual(P("combo.txt"), P("combo_copy.txt")));

        // 三种哈希一致性：fileHash vs Hasher
        auto sha = My::File::fileHash(P("combo.txt"));
        auto crc = My::File::fileCrc32(P("combo.txt"));
        auto xxh = My::File::fileXxHash64(P("combo.txt"));
        ASSERT_TRUE(sha.has_value());
        ASSERT_TRUE(crc.has_value());
        ASSERT_TRUE(xxh.has_value());

        My::Hasher hSha(My::Hasher::Algorithm::Sha256);
        My::Hasher hCrc(My::Hasher::Algorithm::Crc32);
        My::Hasher hXxh(My::Hasher::Algorithm::XxHash64);
        auto content = My::File::readall(P("combo.txt"));
        ASSERT_TRUE(content.has_value());
        hSha.update(*content);
        hCrc.update(*content);
        hXxh.update(*content);
        EXPECT_EQ(hSha.finalize(), *sha);
        EXPECT_EQ(hCrc.finalize(), *crc);
        EXPECT_EQ(hXxh.finalize(), *xxh);

        std::cout << "[v1.2.0] all new APIs combined: OK\n";
    }

    // ==================== 审查修复回归测试 ====================

    // P0-1 回归：稀疏索引 readLine 定位正确性
    TEST_F(FileTest, Fix_SparseIndexReadLine)
    {
        constexpr size_t kLines = 200;
        // 生成 200 行文件，每行 ~100 字节
        {
            My::File::Writer w = My::File::write(P("sparse.txt"));
            for (size_t i = 1; i <= kLines; ++i)
            {
                // 每行格式：line_001_padding...padding
                std::string line = "line_" + std::to_string(i);
                line.resize(100, '.');
                w.writeLine(line);
            }
            ASSERT_TRUE(w.commit());
        }

        // 稠密索引作为基准
        My::LineIndex denseIdx(P("sparse.txt"), 1);
        ASSERT_TRUE(denseIdx.valid());
        ASSERT_EQ(denseIdx.lineCount(), kLines);

        // 稀疏索引 granularity=64
        My::LineIndex sparseIdx(P("sparse.txt"), 64);
        ASSERT_TRUE(sparseIdx.valid());
        ASSERT_EQ(sparseIdx.lineCount(), kLines);
        EXPECT_EQ(sparseIdx.granularity(), 64u);

        // 随机抽取多行对比：稠密 vs 稀疏 必须一致
        std::vector<size_t> testLines = {1, 2, 50, 64, 65, 66, 100, 128, 129, 130, 199, 200};
        for (size_t ln : testLines)
        {
            auto dense = My::File::readLine(P("sparse.txt"), ln, denseIdx);
            auto sparse = My::File::readLine(P("sparse.txt"), ln, sparseIdx);
            ASSERT_TRUE(dense.has_value()) << "line " << ln;
            ASSERT_TRUE(sparse.has_value()) << "line " << ln;
            EXPECT_EQ(*dense, *sparse) << "mismatch at line " << ln;
        }

        // 稀疏索引 granularity=2（最小稀疏）
        My::LineIndex sparse2(P("sparse.txt"), 2);
        for (size_t ln : testLines)
        {
            auto dense = My::File::readLine(P("sparse.txt"), ln, denseIdx);
            auto s2 = My::File::readLine(P("sparse.txt"), ln, sparse2);
            ASSERT_TRUE(s2.has_value()) << "line " << ln;
            EXPECT_EQ(*dense, *s2) << "mismatch at line " << ln << " (granularity=2)";
        }
    }

    // P0-2 回归：tail() 文件以 \n 开头时首行不丢失
    TEST_F(FileTest, Fix_TailLeadingNewline)
    {
        // 文件内容：\nfoo\n （首行为空，第二行 foo）
        WriteOk("tail_ln.txt", "\nfoo\n");

        auto r = My::File::tail(P("tail_ln.txt"), 2);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(r->size(), 2u);
        EXPECT_EQ((*r)[0], ""); // 首行是空行
        EXPECT_EQ((*r)[1], "foo");

        // 只取 1 行
        auto r1 = My::File::tail(P("tail_ln.txt"), 1);
        ASSERT_TRUE(r1.has_value());
        ASSERT_EQ(r1->size(), 1u);
        EXPECT_EQ((*r1)[0], "foo");

        // 文件只有一个 \n
        WriteOk("tail_single_nl.txt", "\n");
        auto rs = My::File::tail(P("tail_single_nl.txt"), 1);
        ASSERT_TRUE(rs.has_value());
        ASSERT_EQ(rs->size(), 1u);
        EXPECT_EQ((*rs)[0], ""); // 空行

        // 文件只有 \n，取 5 行
        auto rs5 = My::File::tail(P("tail_single_nl.txt"), 5);
        ASSERT_TRUE(rs5.has_value());
        ASSERT_EQ(rs5->size(), 1u);
        EXPECT_EQ((*rs5)[0], "");
    }

} // namespace
