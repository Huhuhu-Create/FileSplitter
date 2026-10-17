// ============================================================================
//  zip_volume.h  ——  分卷 ZIP 写入器 / 分卷完整性校验
//
//  设计目标
//  1. 零第三方依赖：只用 Win32 + 标准库，自己按 ZIP(APPNOTE 6.3.7) 规范写字节。
//  2. 分卷（Spanning）符合规范：除最后一片外，每片都以 "PK\007\010"（数据描述符签名）
//     结尾；最后一片带完整的中央目录 + EOCD(ZIP64) + "PK\006\007" 分卷结束标记。
//     => 7-Zip / WinRAR / Info-ZIP / Python zipfile 都能直接把 .zip.001 当成多卷包打开。
//  3. 全程流式：源文件以 4MB 缓冲边算 SHA-256 边写入，内存占用恒定，支持超大文件。
//  4. 支持取消：每个缓冲块检查一次取消标志。
//  5. 进度回调：行内进度（复制阶段按已复制字节，收尾阶段按 10~100%）。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fsplit {

// ---------------------------------------------------------------------------
//  错误码：GUI 层把它翻译成中文提示
// ---------------------------------------------------------------------------
enum class ErrorCode {
    Ok = 0,
    InvalidArgument,    // 参数非法（源文件不存在、分卷大小 <=0 等）
    NotRegularFile,     // 源路径不是普通文件（是目录 / 设备）
    SourceMissing,      // 源文件不存在
    SourceOpenFailed,   // 源文件打不开（不存在 / 是目录）
    SourceLocked,       // 源文件被其它程序独占占用
    SourceChanged,      // 复制过程中源文件被修改（大小/时间变了）
    OutputDirFailed,    // 输出目录创建失败
    OutputOpenFailed,   // 分卷文件创建失败（只读目录 / 权限）
    OutputLocked,       // 分卷目标文件被占用
    DiskFull,           // 磁盘空间不足
    WriteFailed,        // 写盘失败
    ReadFailed,         // 读源文件失败
    Cancelled,          // 用户取消
    EntryNameInvalid,   // 归一到 zip 内的路径名失败
    InternalError       // 其他未预期错误
};

// 结构化结果：code 决定提示语，detail 给出具体到文件/字节的细节
struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::wstring detail;  // 失败对象说明（哪个文件、写入了多少字节等）

    bool ok() const { return code == ErrorCode::Ok; }
    static Error none() { return Error{}; }
    static Error make(ErrorCode c, const std::wstring& d = std::wstring{}) {
        Error e;
        e.code = c;
        e.detail = d;
        return e;
    }
};

// 行内进度回调：内部已做过节流（约 100ms 一次）
using ProgressFn = std::function<void(int percent, const std::wstring& text)>;
using CancelFn = std::function<bool()>;

// ---------------------------------------------------------------------------
//  分卷参数
// ---------------------------------------------------------------------------
struct SplitOptions {
    std::wstring sourcePath;     // 源文件（绝对路径）
    std::wstring outputDir;      // 输出目录（绝对路径），不存在会自动创建
    std::wstring baseName;       // 分卷基础名，最终形如 <baseName>.zip.001
    std::uint64_t volumeSize = 100ull * 1024 * 1024;  // 每卷字节数
    bool storeMode = false;      // true = 不压缩（ZIP Store），仅分卷
};

struct SplitResult {
    bool success = false;
    bool cancelled = false;
    Error error;

    std::wstring zipBasePath;            // 形如 D:\out\xxx.zip（已去掉 .001）
    std::vector<std::wstring> volumes;   // 全部分卷完整路径，按顺序
    std::uint64_t sourceSize = 0;
    std::uint64_t totalBytes = 0;
    std::wstring sha256;                 // 源文件 SHA-256（hex 小写）
};

// ---------------------------------------------------------------------------
//  主要接口
// ---------------------------------------------------------------------------

// 拆分。失败时保证不留下半成品分卷文件。
SplitResult SplitFileToVolumes(const SplitOptions& opt, const ProgressFn& onProgress,
                               const CancelFn& onCancel);

// 按顺序校验一个分卷集合（不写盘）：检查缺失、签名、EOCD 体积一致性、分卷数一致。
// 返回 ok 时 detail 为空；否则 detail 是可以直接展示给用户的中文描述。
Error VerifyVolumeSet(const std::wstring& firstVolume);

// 给 GUI 用的中文错误描述（不含路径时 detail 为空）
std::wstring DescribeError(const Error& err);

// 常用工具
std::uint64_t FileSizeOrZero(const std::wstring& path);
std::wstring Sha256OfFile(const std::wstring& path);          // 失败返回空串
std::wstring HumanSize(std::uint64_t bytes);                  // "1.5 GB"
std::wstring FormatVolumeIndex(int index);                    // 1 -> "001"

}  // namespace fsplit
