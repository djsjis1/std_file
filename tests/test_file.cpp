#include <gtest/gtest.h>
#include "file.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
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
} // namespace
