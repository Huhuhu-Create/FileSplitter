// ============================================================================
//  merge_script.cpp  ——  生成 merge.bat + merge.ps1
//
//  ▍脚本设计要点
//   1. merge.bat 自身只用 ASCII 正文（避免 cmd 的 GBK/UTF-8 乱码），中文提示交给 PowerShell。
//      PowerShell 脚本写成 UTF-8 with BOM，这样 Windows PowerShell 5.1 也能正确读出中文。
//   2. 路径全部用 单引号包裹 的 PowerShell 字面量，转义规则：把 ' 变 ''；
//      因此带空格、中文、括号、& 的路径都安全，避免 cmd 的引号地狱。
//   3. 合并流程：
//        校验分卷齐全 → 按顺序逐卷追加 → 比对大小 → 比对 SHA-256 → 输出结论
//      任一步失败都给出明确中文原因并以非 0 退出码结束（bat 会 pause 住让人看见）。
//   4. 目标已存在时询问：覆盖 / 另存副本，绝不静默覆盖用户数据。
// ============================================================================
#include "merge_script.h"

#include <windows.h>

#include <string>
#include <vector>

#include "util.h"

namespace fsplit {
namespace {

// PowerShell 单引号字符串字面量
std::wstring PsQuote(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out.push_back(c);
    }
    out += L"'";
    return out;
}

// UTF-8 编码 + 可选 BOM
std::string ToUtf8(const std::wstring& s, bool withBom) {
    std::string out;
    if (withBom) {
        out.push_back(static_cast<char>(0xEF));
        out.push_back(static_cast<char>(0xBB));
        out.push_back(static_cast<char>(0xBF));
    }
    if (s.empty()) return out;
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                   nullptr, nullptr);
    if (need <= 0) return out;
    std::string buf(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), buf.data(), need, nullptr,
                        nullptr);
    out += buf;
    return out;
}

bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

}  // namespace

MergeScriptInfo WriteMergeScripts(const std::wstring& outDir, const std::wstring& zipBaseName,
                                  int volumeCount, std::uint64_t expectedSize,
                                  const std::wstring& expectedSha, const std::wstring& restoreName,
                                  const std::wstring& sourceName) {
    MergeScriptInfo info;
    info.batPath = JoinPath(outDir, L"merge.bat");
    info.ps1Path = JoinPath(outDir, L"merge.ps1");

    const std::wstring restorePath = JoinPath(outDir, restoreName);
    const std::wstring sha = expectedSha.empty() ? L"" : expectedSha;

    // ---------------- merge.ps1 ----------------
    std::wstring ps;
    ps.reserve(16384);
    ps += L"# ----------------------------------------------------------------------------\n";
    ps += L"#  merge.ps1  ——  由「文件分卷工具」自动生成，请勿手动修改\n";
    ps += L"#  作用：按序号合并分卷 → 校验体积与 SHA-256 → 还原原始文件\n";
    ps += L"# ----------------------------------------------------------------------------\n";
    ps += L"$ErrorActionPreference = 'Stop'\n";
    ps += L"$OutputEncoding = [Console]::OutputEncoding = [Text.Encoding]::UTF8\n";
    ps += L"chcp 936 > $null 2>&1\n";
    ps += L"\n";
    ps += L"$Dir        = " + PsQuote(outDir) + L"\n";
    ps += L"$Base       = " + PsQuote(zipBaseName) + L"\n";
    ps += L"$VolCount   = " + std::to_wstring(volumeCount) + L"\n";
    ps += L"$ExpectSize = [int64]" + std::to_wstring(expectedSize) + L"\n";
    ps += L"$ExpectSha  = " + PsQuote(sha) + L"\n";
    ps += L"$Target     = " + PsQuote(restorePath) + L"\n";
    ps += L"$OrigPath   = " + PsQuote(sourceName) + L"\n";
    ps += L"\n";
    ps += L"function Info($m) { Write-Host $m }\n";
    ps += L"function Ok($m)   { Write-Host $m -ForegroundColor Green }\n";
    ps += L"function Warn($m) { Write-Host $m -ForegroundColor Yellow }\n";
    ps += L"function Fail($m) {\n";
    ps += L"    Write-Host '' \n";
    ps += L"    Write-Host '==================== 合并失败 ====================' -ForegroundColor Red\n";
    ps += L"    Write-Host $m -ForegroundColor Red\n";
    ps += L"    Write-Host '==================================================' -ForegroundColor Red\n";
    ps += L"    Write-Host ''\n";
    ps += L"    Read-Host '按 Enter 键退出'\n";
    ps += L"    exit 1\n";
    ps += L"}\n";
    ps += L"\n";
    ps += L"Write-Host ''\n";
    ps += L"Write-Host '========== 文件分卷合并工具 ==========' -ForegroundColor Cyan\n";
    ps += L"Write-Host (\"所在目录 : \" + $Dir)\n";
    ps += L"Write-Host (\"分卷前缀 : \" + $Base)\n";
    ps += L"Write-Host (\"分卷数量 : \" + $VolCount)\n";
    ps += L"Write-Host ''\n";
    ps += L"\n";
    ps += L"# ---------- 1. 分卷清单与完整性 ----------\n";
    ps += L"$vols = @()\n";
    ps += L"for ($i = 1; $i -le $VolCount; $i++) {\n";
    ps += L"    $name = $Base + '.' + $i.ToString('000')\n";
    ps += L"    $full = Join-Path $Dir $name\n";
    ps += L"    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {\n";
    ps += L"        Fail (\"缺少分卷：\" + $name + \"`n请确认所有分卷放在同一目录，且没有被改名或删除。\")\n";
    ps += L"    }\n";
    ps += L"    $len = (Get-Item -LiteralPath $full).Length\n";
    ps += L"    if ($len -lt 4) { Fail (\"分卷为空或被截断：\" + $name) }\n";
    ps += L"    $vols += $full\n";
    ps += L"}\n";
    ps += L"Info (\"[1/5] 分卷齐全，共 \" + $vols.Count + \" 个\")\n";
    ps += L"\n";
    ps += L"# ---------- 2. 目标文件是否已存在 ----------\n";
    ps += L"if (Test-Path -LiteralPath $Target) {\n";
    ps += L"    $existLen = (Get-Item -LiteralPath $Target).Length\n";
    ps += L"    if ($existLen -eq $ExpectSize) {\n";
    ps += L"        Ok (\"目标文件已存在且大小一致，无需重复合并：\" + $Target)\n";
    ps += L"        Write-Host ''\n";
    ps += L"        Read-Host '按 Enter 键退出'\n";
    ps += L"        exit 0\n";
    ps += L"    }\n";
    ps += L"    Warn (\"目标文件已存在但大小不一致：\" + $Target)\n";
    ps += L"    $ans = Read-Host '是否覆盖？输入 Y 覆盖，直接回车改为在同目录生成副本'\n";
    ps += L"    if ($ans -notmatch '^[Yy]') {\n";
    ps += L"        $baseNameOnly = [IO.Path]::GetFileNameWithoutExtension($Target)\n";
    ps += L"        $extOnly      = [IO.Path]::GetExtension($Target)\n";
    ps += L"        $stamp        = Get-Date -Format 'yyyyMMdd_HHmmss'\n";
    ps += L"        $Target       = Join-Path $Dir ($baseNameOnly + '_' + $stamp + $extOnly)\n";
    ps += L"        Warn (\"将改为写入：\" + $Target)\n";
    ps += L"    }\n";
    ps += L"}\n";
    ps += L"\n";
    ps += L"# ---------- 3. 拼接分卷为完整 ZIP，并从中提取原始文件 ----------\n";
    ps += L"#  分卷是「真·分卷 ZIP」：每片按顺序拼起来是一个合法 zip（中间片的 4 字节\n";
    ps += L"#  分卷结束标记属于分卷框架，拼接前要剥掉，否则会插进数据里）。\n";
    ps += L"#  这个 zip 里只有一个条目，就是我们拆分前的原文件；它是 Store（不压缩）方式\n";
    ps += L"#  存的，所以直接把条目流写出来就是原文件本体。\n";
    ps += L"$ZipTemp = Join-Path $Dir ('__merge_' + [Guid]::NewGuid().ToString('N') + '.zip.tmp')\n";
    ps += L"try {\n";
    ps += L"    $zs = [IO.File]::Open($ZipTemp, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)\n";
    ps += L"    try {\n";
    ps += L"        for ($i = 0; $i -lt $vols.Count; $i++) {\n";
    ps += L"            $src  = $vols[$i]\n";
    ps += L"            $name = [IO.Path]::GetFileName($src)\n";
    ps += L"            $bytes = [IO.File]::ReadAllBytes($src)\n";
    ps += L"            $isLast = ($i -eq $vols.Count - 1)\n";
    ps += L"            if (-not $isLast -and $bytes.Length -ge 4) {\n";
    ps += L"                # 剥掉本片末尾的 4 字节分卷结束标记 PK\\x07\\x08\n";
    ps += L"                $nl = $bytes.Length\n";
    ps += L"                if ($bytes[$nl-4] -eq 0x50 -and $bytes[$nl-3] -eq 0x4B -and\n";
    ps += L"                    $bytes[$nl-2] -eq 0x07 -and $bytes[$nl-1] -eq 0x08) {\n";
    ps += L"                    $zs.Write($bytes, 0, $nl - 4)\n";
    ps += L"                } else {\n";
    ps += L"                    $zs.Write($bytes, 0, $nl)\n";
    ps += L"                }\n";
    ps += L"            } else {\n";
    ps += L"                $zs.Write($bytes, 0, $bytes.Length)\n";
    ps += L"            }\n";
    ps += L"            $pct = [int](($i + 1) * 90 / $vols.Count)\n";
    ps += L"            Write-Progress -Activity '正在拼接分卷' -Status $name -PercentComplete $pct\n";
    ps += L"        }\n";
    ps += L"    } finally {\n";
    ps += L"        $zs.Close()\n";
    ps += L"    }\n";
    ps += L"    Write-Progress -Activity '正在拼接分卷' -Completed\n";
    ps += L"    Info '[2/5] 分卷拼接完毕，正在还原原始文件……'\n";
    ps += L"\n";
    ps += L"    # 用 ZipArchive 只读打开，取出唯一条目的原始字节\n";
    ps += L"    Add-Type -AssemblyName System.IO.Compression.FileSystem\n";
    ps += L"    $zip = [IO.Compression.ZipFile]::OpenRead($ZipTemp)\n";
    ps += L"    try {\n";
    ps += L"        if ($zip.Entries.Count -lt 1) {\n";
    ps += L"            Fail '分卷内容为空：zip 里没有任何文件条目，分卷可能已损坏。'\n";
    ps += L"        }\n";
    ps += L"        $entry = $zip.Entries[0]\n";
    ps += L"        Info ('        归档内文件名 : ' + $entry.FullName)\n";
    ps += L"        Info ('        原始体积     : ' + $entry.Length + ' 字节')\n";
    ps += L"        $os = [IO.File]::Open($Target, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)\n";
    ps += L"        try {\n";
    ps += L"            $es = $entry.Open()\n";
    ps += L"            try { $es.CopyTo($os, 1048576) } finally { $es.Dispose() }\n";
    ps += L"        } finally {\n";
    ps += L"            $os.Close()\n";
    ps += L"        }\n";
    ps += L"    } finally {\n";
    ps += L"        $zip.Dispose()\n";
    ps += L"    }\n";
    ps += L"} catch {\n";
    ps += L"    Remove-Item -LiteralPath $Target -Force -ErrorAction SilentlyContinue\n";
    ps += L"    Fail (\"还原失败：\" + $_.Exception.Message + \"`n`n常见原因：`n\" +\n";
    ps += L"          \"  · 分卷缺失或顺序错乱（请确认 .001/.002/... 连续且都在同一目录）`n\" +\n";
    ps += L"          \"  · 分卷被网盘/杀毒软件改写或截断`n\" +\n";
    ps += L"          \"  · 目标磁盘空间不足或没有写入权限\")\n";
    ps += L"} finally {\n";
    ps += L"    Remove-Item -LiteralPath $ZipTemp -Force -ErrorAction SilentlyContinue\n";
    ps += L"}\n";
    ps += L"Info '[3/5] 原始文件已还原'\n";
    ps += L"\n";
    ps += L"# ---------- 4. 校验 ----------\n";
    ps += L"$outLen = (Get-Item -LiteralPath $Target).Length\n";
    ps += L"Info (\"        期望体积 : \" + $ExpectSize + \" 字节\")\n";
    ps += L"Info (\"        实际体积 : \" + $outLen + \" 字节\")\n";
    ps += L"if ($outLen -ne $ExpectSize) {\n";
    ps += L"    Fail (\"体积校验不通过（差 \" + ($ExpectSize - $outLen) + \" 字节），分卷可能损坏或被截断。`n文件已保留在：\" + $Target)\n";
    ps += L"}\n";
    ps += L"Info '[4/5] 体积校验通过'\n";
    ps += L"\n";
    ps += L"if ($ExpectSha -ne '') {\n";
    ps += L"    Info '        正在计算 SHA-256，大文件可能需要一会儿……'\n";
    ps += L"    $actual = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash.ToLower()\n";
    ps += L"    Info (\"        期望 SHA-256 : \" + $ExpectSha)\n";
    ps += L"    Info (\"        实际 SHA-256 : \" + $actual)\n";
    ps += L"    if ($actual -ne $ExpectSha.ToLower()) {\n";
    ps += L"        Fail (\"SHA-256 校验不通过，文件内容与原文件不一致。`n文件已保留在：\" + $Target)\n";
    ps += L"    }\n";
    ps += L"    Info '[5/5] SHA-256 校验通过'\n";
    ps += L"} else {\n";
    ps += L"    Warn '[5/5] 未记录原始哈希，已跳过 SHA-256 校验（仅校验了体积）'\n";
    ps += L"}\n";
    ps += L"\n";
    ps += L"Write-Host ''\n";
    ps += L"Ok '==================== 合并成功 ===================='\n";
    ps += L"Ok (\"还原文件 : \" + $Target)\n";
    ps += L"Ok (\"文件大小 : \" + $outLen + \" 字节\")\n";
    ps += L"Ok '=================================================='\n";
    ps += L"Write-Host ''\n";
    ps += L"$open = Read-Host '是否打开所在文件夹？输入 Y 打开，直接回车退出'\n";
    ps += L"if ($open -match '^[Yy]') { Start-Process explorer.exe ('/select,' + $Target) }\n";
    ps += L"exit 0\n";

    info.ps1Path = JoinPath(outDir, L"merge.ps1");
    WriteAllBytes(info.ps1Path, ToUtf8(ps, true));

    // ---------------- merge.bat ----------------
    // 纯 ASCII；用 %~dp0 定位自身目录，兼容从任意位置（含网络路径）双击。
    std::string bat;
    bat += "@echo off\r\n";
    bat += "setlocal\r\n";
    bat += "chcp 936 >nul 2>&1\r\n";
    bat += "rem 由「文件分卷工具」生成：按序号合并分卷并校验\r\n";
    bat += "set \"HERE=%~dp0\"\r\n";
    bat += "set \"PS1=%HERE%merge.ps1\"\r\n";
    bat += "title Merge Volumes\r\n";
    bat += "\r\n";
    bat += "if not exist \"%PS1%\" (\r\n";
    bat += "    echo [ERROR] merge.ps1 not found next to merge.bat.\r\n";
    bat += "    echo         merge.ps1 should be in the same folder as the volumes.\r\n";
    bat += "    pause\r\n";
    bat += "    exit /b 1\r\n";
    bat += ")\r\n";
    bat += "\r\n";
    bat += "rem 优先用 PowerShell 5.1+；-NoProfile 避免用户 profile 干扰\r\n";
    bat += "where powershell.exe >nul 2>&1\r\n";
    bat += "if errorlevel 1 (\r\n";
    bat += "    echo [ERROR] Windows PowerShell not found. Windows 7 or later is required.\r\n";
    bat += "    pause\r\n";
    bat += "    exit /b 1\r\n";
    bat += ")\r\n";
    bat += "\r\n";
    bat += "powershell.exe -NoProfile -NoLogo -ExecutionPolicy Bypass -File \"%PS1%\"\r\n";
    bat += "if errorlevel 1 exit /b 1\r\n";
    bat += "exit /b 0\r\n";

    WriteAllBytes(info.batPath, bat);
    return info;
}

}  // namespace fsplit
