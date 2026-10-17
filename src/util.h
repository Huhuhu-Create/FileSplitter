#pragma once
// ============================================================================
//  util.h  ——  通用工具
// ============================================================================
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "zip_volume.h"  // Error / ErrorCode

namespace fsplit {

// ---- 哈希 ----
std::wstring Sha256OfFile(const std::wstring& path);  // 失败返回空串

// ---- 文件 / 路径 ----
std::uint64_t FileSizeOrZero(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool EnsureDirectory(const std::wstring& dir, std::wstring& errOut);
std::wstring GetParentDir(const std::wstring& path);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring FileNameOf(const std::wstring& path);
std::wstring NormalizePath(const std::wstring& path);
bool IsSamePath(const std::wstring& a, const std::wstring& b);
std::uint64_t FreeSpaceAt(const std::wstring& dir);  // 失败返回 UINT64_MAX

// ---- 文案 ----
std::wstring HumanSize(std::uint64_t bytes);
std::wstring FormatVolumeIndex(int index);
std::wstring LastErrorText(DWORD err);
std::wstring DescribeError(const Error& err);

// ---- Shell ----
void OpenInExplorer(const std::wstring& path);
void OpenNotepad(const std::wstring& path);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);

}  // namespace fsplit
