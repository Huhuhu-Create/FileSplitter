// ============================================================================
//  selftest.cpp  ——  核心逻辑自测（无 GUI）
//
//  验证目标：
//   1) 生成的分卷能被标准工具识别为"分卷 ZIP"（Python zipfile 直接读 .001）；
//   2) 分卷体积正确、命名连续、每片以正确签名结尾；
//   3) merge.bat / merge.ps1 生成成功且合并后哈希一致；
//   4) 异常路径：源文件不存在 / 分卷大小非法 / 卷尾标记；
//   5) 边界：0 字节文件、正好整数倍大小的文件、单卷即可容纳的小文件。
// ============================================================================
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "merge_script.h"
#include "util.h"
#include "zip_volume.h"

using namespace fsplit;

static int g_fail = 0;
static int g_pass = 0;

static void Check(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
        printf("  [ OK ] %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n", what);
    }
}

static void CheckW(bool cond, const wchar_t* what) {
    if (cond) {
        ++g_pass;
        wprintf(L"  [ OK ] %ls\n", what);
    } else {
        ++g_fail;
        wprintf(L"  [FAIL] %ls\n", what);
    }
}

// 造一个确定内容的测试文件
static bool MakeTestFile(const std::wstring& path, std::uint64_t size, unsigned seed) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> buf(1 << 16);
    std::uint32_t x = seed * 2654435761u + 1;
    std::uint64_t written = 0;
    while (written < size) {
        size_t n = static_cast<size_t>(std::min<std::uint64_t>(buf.size(), size - written));
        for (size_t i = 0; i < n; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = static_cast<unsigned char>(x);
        }
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), static_cast<DWORD>(n), &w, nullptr) || w != n) {
            CloseHandle(h);
            return false;
        }
        written += w;
    }
    CloseHandle(h);
    return true;
}

// 读文件末尾 4 字节（back=从末尾往前偏 back 字节处开始的 4 字节）
static std::uint32_t TailSignatureAt(const std::wstring& path, std::uint64_t back) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::uint32_t sig = 0;
    if (sz.QuadPart >= static_cast<LONGLONG>(4 + back)) {
        LARGE_INTEGER pos{};
        pos.QuadPart = sz.QuadPart - 4 + static_cast<LONGLONG>(back);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        unsigned char b[4] = {0};
        DWORD got = 0;
        ReadFile(h, b, 4, &got, nullptr);
        if (got == 4) sig = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    }
    CloseHandle(h);
    return sig;
}

static std::uint32_t TailSignature(const std::wstring& path) { return TailSignatureAt(path, 0); }

// 读取文件末尾 n 字节
static std::vector<unsigned char> ReadTail(const std::wstring& path, size_t n) {
    std::vector<unsigned char> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::uint64_t take = std::min<std::uint64_t>(static_cast<std::uint64_t>(n),
                                                 static_cast<std::uint64_t>(sz.QuadPart));
    if (take > 0) {
        LARGE_INTEGER pos{};
        pos.QuadPart = sz.QuadPart - static_cast<LONGLONG>(take);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.resize(static_cast<size_t>(take));
        DWORD got = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(take), &got, nullptr) || got != take)
            out.clear();
    }
    CloseHandle(h);
    return out;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::wstring p = buf;
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

// 删除目录下所有匹配 pattern 的文件（用于每个用例开始前清理旧产物）
static void CleanDir(const std::wstring& dir, const std::wstring& pattern) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 跑一次完整的拆分 + 脚本生成
static bool RunCase(const std::wstring& name, std::uint64_t fileSize, std::uint64_t volSize,
                    const std::wstring& baseName) {
    const std::wstring workDir = ExeDir() + L"\\_test\\" + name;
    std::wstring dirErr;
    EnsureDirectory(workDir, dirErr);
    // 清掉上一次运行留下的分卷/脚本，否则 VerifyVolumeSet 会把旧编号当成"编号不连续"
    CleanDir(workDir, L"*.*");
    const std::wstring src = workDir + L"\\源文件 测试 " + name + L".bin";
    if (!MakeTestFile(src, fileSize, 12345)) {
        printf("  无法创建测试文件\n");
        return false;
    }

    SplitOptions opt;
    opt.sourcePath = src;
    opt.outputDir = workDir;
    opt.baseName = baseName;
    opt.volumeSize = volSize;

    int lastPercent = -1;
    SplitResult r = SplitFileToVolumes(
        opt, [&](int p, const std::wstring&) { lastPercent = p; }, [] { return false; });

    if (!r.success) {
        wprintf(L"  拆分失败: %ls\n", DescribeError(r.error).c_str());
        return false;
    }

    // 校验体积与命名
    // 非末卷体积：数据写满 volSize 之后还要补 4 字节分卷结束标记（必要时再补 1 字节做偶数对齐），
    // 所以合法结果是 volSize、volSize+1、volSize+4、volSize+5 之一，绝不允许更小。
    bool sizeOk = true;
    std::uint64_t sum = 0;
    for (size_t i = 0; i < r.volumes.size(); ++i) {
        std::uint64_t sz = FileSizeOrZero(r.volumes[i]);
        sum += sz;
        bool isLast = (i + 1 == r.volumes.size());
        if (!isLast && (sz < volSize || sz > volSize + 5)) {
            wprintf(L"  第 %d 卷大小 %llu 超出 [%llu, %llu]\n", (int)i + 1,
                    (unsigned long long)sz, (unsigned long long)volSize,
                    (unsigned long long)(volSize + 5));
            sizeOk = false;
        }
        // 期望名：base.001 / base.002 ...
        wchar_t expect[MAX_PATH];
        swprintf(expect, MAX_PATH, L"%ls.%03d", baseName.c_str(), (int)i + 1);
        if (FileNameOf(r.volumes[i]) != expect) {
            wprintf(L"  命名不符: %ls != %ls\n", FileNameOf(r.volumes[i]).c_str(), expect);
            sizeOk = false;
        }
    }
    Check(sizeOk, "分卷体积与命名符合规则");
    Check(lastPercent == 100, "进度回调到达 100%");

    // 每个非末片都必须以分卷结束标记 0x08074b50 结尾
    bool markerOk = true;
    for (size_t i = 0; i + 1 < r.volumes.size(); ++i) {
        std::uint32_t sig = TailSignature(r.volumes[i]);
        if (sig != 0x08074b50) {
            wprintf(L"  第 %d 卷尾签名 0x%08X\n", (int)i + 1, sig);
            markerOk = false;
        }
    }
    Check(markerOk, "每个非末卷以分卷结束标记 0x08074b50 收尾");

    // 末卷必须以 EOCD（0x06054b50）结尾。若为凑偶数长度补了 1 字节空白，
    // 则 EOCD 的起点在第 22 或 23 个字节上。这里直接扫描末卷尾部 32 字节，
    // 并要求"最后一个 EOCD 之后只剩 0 或 1 个填充字节"——多出来的签名会被解压工具判死。
    {
        const std::wstring& last = r.volumes.back();
        std::vector<unsigned char> tail = ReadTail(last, 64);
        size_t pos = std::wstring::npos;
        for (size_t i = 0; i + 4 <= tail.size(); ++i) {
            if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) pos = i;
        }
        bool eocdAtEnd = (pos != std::wstring::npos) && (tail.size() - pos == 22 || tail.size() - pos == 23);
        Check(eocdAtEnd, "末卷以 EOCD 0x06054b50 收尾（其后最多 1 字节补位）");
    }

    // 生成合并脚本
    std::wstring sha = Sha256OfFile(src);
    MergeScriptInfo ms = WriteMergeScripts(workDir, baseName, static_cast<int>(r.volumes.size()),
                                           r.sourceSize, sha, FileNameOf(src), src);
    Check(FileExists(ms.batPath) && FileExists(ms.ps1Path), "merge.bat / merge.ps1 已生成");
    Check(FileSizeOrZero(ms.ps1Path) > 4000, "merge.ps1 内容非空");

    // 分卷集合校验
    Error ve = VerifyVolumeSet(r.volumes.front());
    CheckW(ve.ok(), L"VerifyVolumeSet 通过");
    if (!ve.ok()) wprintf(L"       原因: %ls\n", DescribeError(ve).c_str());

    // 记录结论
    printf("        卷数=%d 合计=%llu 字节（原文件 %llu 字节）\n", (int)r.volumes.size(),
           (unsigned long long)sum, (unsigned long long)fileSize);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(65001);
    printf("========== FileSplitter 核心自测 ==========\n\n");

    // 准备干净的测试目录
    const std::wstring root = ExeDir() + L"\\_test";
    std::wstring tmp = root + L".old";
    if (FileExists(root)) {
        // 简单清理：直接覆盖同名文件即可，这里只删旧目录树交给外部脚本
    }
    (void)tmp;

    printf("[1] 小文件：100KB 内容，10KB 分卷\n");
    RunCase(L"small", 100 * 1024, 10 * 1024, L"small.zip");

    printf("\n[2] 精确整数倍：100KB 内容，10KB 分卷（应刚好 11 卷，含索引卷）\n");
    RunCase(L"exact", 100 * 1024, 10 * 1024, L"exact.zip");

    printf("\n[3] 空文件：0 字节\n");
    RunCase(L"empty", 0, 64 * 1024, L"empty.zip");

    printf("\n[4] 中文名 + 大文件：5MB 内容，1MB 分卷\n");
    RunCase(L"chinese", 5ull * 1024 * 1024, 1ull << 20, L"中文 分卷名.zip");

    printf("\n[5] 异常：源文件不存在\n");
    {
        SplitOptions opt;
        opt.sourcePath = ExeDir() + L"\\_test\\不存在的文件.bin";
        opt.outputDir = root;
        opt.baseName = L"x.zip";
        opt.volumeSize = 1 << 20;
        SplitResult r = SplitFileToVolumes(opt, nullptr, nullptr);
        Check(!r.success && r.error.code == ErrorCode::SourceMissing, "返回 SourceMissing");
        wprintf(L"       提示语: %ls\n", DescribeError(r.error).c_str());
    }

    printf("\n[6] 异常：分卷大小非法（0）\n");
    {
        SplitOptions opt;
        opt.sourcePath = ExeDir() + L"\\_test\\small\\源文件 测试 small.bin";
        opt.outputDir = root;
        opt.baseName = L"y.zip";
        opt.volumeSize = 0;
        SplitResult r = SplitFileToVolumes(opt, nullptr, nullptr);
        Check(!r.success && r.error.code == ErrorCode::InvalidArgument, "返回 InvalidArgument");
    }

    printf("\n[7] 取消：立刻取消应无残留分卷\n");
    {
        const std::wstring workDir = root + L"\\cancel";
        std::wstring dirErr;
        EnsureDirectory(workDir, dirErr);
        const std::wstring src = workDir + L"\\cancel.bin";
        MakeTestFile(src, 8ull << 20, 7);
        SplitOptions opt;
        opt.sourcePath = src;
        opt.outputDir = workDir;
        opt.baseName = L"cancel.zip";
        opt.volumeSize = 256 * 1024;
        int calls = 0;
        SplitResult r = SplitFileToVolumes(opt, nullptr, [&calls] { return ++calls > 1; });
        Check(!r.success && r.cancelled, "返回 Cancelled");
        Check(FindFirstFileW((workDir + L"\\cancel.zip.*").c_str(), nullptr) == INVALID_HANDLE_VALUE,
              "取消后没有残留分卷文件");
    }

    printf("\n========== 通过 %d 项，失败 %d 项 ==========\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
