#pragma once
// ============================================================================
//  merge_script.h  ——  生成合并用的批处理
//
//  输出两份脚本：
//   · merge.bat        —— 双击运行，调用 PowerShell 做"安全合并 + 哈希校验"
//   · merge.ps1        —— 真正干活的脚本（在 ASCII 之外做 UTF-8 处理、空格路径、进度条）
//   · 分卷清单.txt      —— 可选，记录所有分卷名与 SHA-256，便于人工核对
//
//  为什么要 PowerShell？
//    cmd 只能 copy /b 拼字节（很脆：中文路径、空格、缺卷检测、哈希全部做不到）。
//    真正的校验只能靠 PowerShell（Get-FileHash），Windows 7 以上都自带，无需安装。
//    merge.bat 只做一件事：用绝对路径启动 powershell，并把退出码翻译成人话。
// ============================================================================
#include <string>
#include <vector>

namespace fsplit {

struct MergeScriptInfo {
    std::wstring batPath;
    std::wstring ps1Path;
};

// 生成 merge.bat / merge.ps1
//   outDir        : 分卷所在目录
//   zipBaseName   : 形如 "xxx.zip"（分卷为 xxx.zip.001 ...）
//   volumeCount   : 分卷数量
//   expectedSize  : 源文件大小
//   expectedSha   : 源文件 SHA-256（64 位 hex，可能为空则跳过哈希校验）
//   restoreName   : 还原出来的文件名
//   sourceName    : 原始文件完整路径（仅用于日志说明）
MergeScriptInfo WriteMergeScripts(const std::wstring& outDir, const std::wstring& zipBaseName,
                                  int volumeCount, std::uint64_t expectedSize,
                                  const std::wstring& expectedSha, const std::wstring& restoreName,
                                  const std::wstring& sourceName);

}  // namespace fsplit
