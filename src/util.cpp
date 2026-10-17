// ============================================================================
//  util.cpp  ——  通用工具：哈希、文件信息、体积格式化、错误文案、Win32 错误映射
// ============================================================================
#include "util.h"

#include <windows.h>
#include <bcrypt.h>     // BCryptHashData —— 系统自带 SHA-256，无需链接第三方库
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace fsplit {

// ---------------------------------------------------------------------------
//  哈希
// ---------------------------------------------------------------------------
struct Sha256Ctx {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> obj;
    bool ok = false;

    Sha256Ctx() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return;
        DWORD objSize = 0, got = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                              reinterpret_cast<PUCHAR>(&objSize), sizeof(objSize),
                                              &got, 0)))
            return;
        obj.resize(objSize);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj.data(), objSize, nullptr, 0, 0)))
            return;
        ok = true;
    }
    ~Sha256Ctx() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool update(const void* data, size_t len) {
        if (!ok) return false;
        return BCRYPT_SUCCESS(BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                            static_cast<ULONG>(len), 0));
    }
    bool finalize(std::wstring& outHex) {
        if (!ok) return false;
        unsigned char digest[32] = {0};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) return false;
        static const wchar_t* kHex = L"0123456789abcdef";
        outHex.clear();
        outHex.reserve(64);
        for (unsigned char c : digest) {
            outHex.push_back(kHex[c >> 4]);
            outHex.push_back(kHex[c & 0x0F]);
        }
        return true;
    }
};

std::wstring Sha256OfFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring{};

    Sha256Ctx ctx;
    std::wstring out;
    std::vector<unsigned char> buf(4u << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            out.clear();
            break;
        }
        if (got == 0) break;  // 读到 EOF
        if (!ctx.update(buf.data(), got)) { out.clear(); break; }
    }
    CloseHandle(h);
    if (!out.empty() || out.empty()) {
        std::wstring hex;
        if (ctx.finalize(hex)) out = hex;
    }
    return out.empty() ? std::wstring{} : out;
}

// ---------------------------------------------------------------------------
//  文件信息
// ---------------------------------------------------------------------------
std::uint64_t FileSizeOrZero(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& dir, std::wstring& errOut) {
    if (dir.empty()) {
        errOut = L"输出目录为空";
        return false;
    }
    if (IsDirectory(dir)) return true;

    // 逐级创建（GetFullPathName + 依次 CreateDirectory），中文/长路径都能处理
    std::wstring full = NormalizePath(dir);
    if (full.empty()) {
        errOut = L"路径无效";
        return false;
    }
    std::wstring cur;
    size_t i = 0;
    // 保留盘符前缀 "C:\"
    if (full.size() >= 2 && full[1] == L':') {
        cur = full.substr(0, 2);
        i = 2;
        if (full.size() > 2 && full[2] == L'\\') {
            cur += L'\\';
            i = 3;
        }
    }
    for (; i <= full.size(); ++i) {
        if (i == full.size() || full[i] == L'\\' || full[i] == L'/') {
            if (!cur.empty()) {
                if (!CreateDirectoryW(cur.c_str(), nullptr)) {
                    DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS) {
                        switch (err) {
                            case ERROR_ACCESS_DENIED:
                                errOut = L"没有权限在该位置创建目录";
                                break;
                            case ERROR_PATH_NOT_FOUND:
                                errOut = L"上级路径不存在";
                                break;
                            case ERROR_DISK_FULL:
                            case ERROR_HANDLE_DISK_FULL:
                                errOut = L"磁盘空间不足，无法创建目录";
                                break;
                            default:
                                errOut = L"创建目录失败（错误码 " + std::to_wstring(err) + L"）";
                                break;
                        }
                        return false;
                    }
                }
            }
            if (i < full.size()) cur += full[i];
        } else {
            cur += full[i];
        }
    }
    return IsDirectory(full);
}

std::wstring GetParentDir(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring{};
    if (pos == 2 && path[1] == L':') return path.substr(0, 3);  // "C:\"
    return path.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return path;
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(path.c_str(), 32768, buf, nullptr);
    if (n == 0 || n >= 32768) return path;
    std::wstring result(buf, n);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

// 磁盘可用空间（字节）；失败返回 UINT64_MAX（视为未知，不拦截）
std::uint64_t FreeSpaceAt(const std::wstring& dir) {
    if (dir.empty() || dir.size() < 2 || dir[1] != L':') return UINT64_MAX;
    wchar_t root[5] = {dir[0], L':', L'\\', L'\0', L'\0'};
    ULARGE_INTEGER freeToCaller{};
    if (!GetDiskFreeSpaceExW(root, &freeToCaller, nullptr, nullptr)) return UINT64_MAX;
    return freeToCaller.QuadPart;
}

bool IsSamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(NormalizePath(a).c_str(), NormalizePath(b).c_str()) == 0;
}

// ---------------------------------------------------------------------------
//  提示文案
// ---------------------------------------------------------------------------
std::wstring HumanSize(std::uint64_t bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (v >= 100.0)
        swprintf(buf, 64, L"%.0f %s", v, kUnits[u]);
    else if (v >= 10.0)
        swprintf(buf, 64, L"%.1f %s", v, kUnits[u]);
    else
        swprintf(buf, 64, L"%.2f %s", v, kUnits[u]);
    return std::wstring(buf);
}

std::wstring FormatVolumeIndex(int index) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%03d", index);
    return std::wstring(buf);
}

std::wstring LastErrorText(DWORD err) {
    switch (err) {
        case ERROR_SUCCESS:
            return L"操作成功";
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return L"路径或文件不存在";
        case ERROR_ACCESS_DENIED:
            return L"拒绝访问（权限不足，或文件被占用）";
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return L"文件正被其它程序占用";
        case ERROR_HANDLE_DISK_FULL:
        case ERROR_DISK_FULL:
            return L"磁盘空间不足";
        case 0x00000521:  // ERROR_DISK_QUOTA_EXCEEDED
            return L"超出磁盘配额";
        case ERROR_WRITE_PROTECT:
            return L"磁盘写保护";
        case ERROR_INVALID_NAME:
            return L"文件名或路径含非法字符";
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_BUFFER_OVERFLOW:
            return L"路径过长";
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return L"内存不足";
        case ERROR_DIRECTORY:
            return L"路径不是有效的目录";
        default:
            break;
    }
    LPWSTR text = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring result;
    if (n && text) {
        result.assign(text, n);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                                   result.back() == L'.' || result.back() == L' '))
            result.pop_back();
    }
    if (text) LocalFree(text);
    if (result.empty()) result = L"系统错误码 " + std::to_wstring(err);
    return result;
}

std::wstring DescribeError(const Error& err) {
    std::wstring head;
    switch (err.code) {
        case ErrorCode::Ok:
            return L"成功";
        case ErrorCode::InvalidArgument:
            head = L"参数不合法";
            break;
        case ErrorCode::NotRegularFile:
            head = L"选择的不是普通文件";
            break;
        case ErrorCode::SourceMissing:
            head = L"源文件不存在";
            break;
        case ErrorCode::SourceOpenFailed:
            head = L"无法打开源文件";
            break;
        case ErrorCode::SourceLocked:
            head = L"源文件被其它程序独占占用";
            break;
        case ErrorCode::SourceChanged:
            head = L"源文件在拆分过程中被修改";
            break;
        case ErrorCode::OutputDirFailed:
            head = L"无法创建输出目录";
            break;
        case ErrorCode::OutputOpenFailed:
            head = L"无法创建分卷文件";
            break;
        case ErrorCode::OutputLocked:
            head = L"目标分卷文件被占用";
            break;
        case ErrorCode::DiskFull:
            head = L"磁盘空间不足";
            break;
        case ErrorCode::WriteFailed:
            head = L"写入分卷失败";
            break;
        case ErrorCode::ReadFailed:
            head = L"读取源文件失败";
            break;
        case ErrorCode::Cancelled:
            head = L"操作已取消";
            break;
        case ErrorCode::EntryNameInvalid:
            head = L"无法把源文件名转换为合法的压缩包内路径";
            break;
        case ErrorCode::InternalError:
        default:
            head = L"内部错误";
            break;
    }
    if (!err.detail.empty()) return head + L"：" + err.detail;
    return head;
}

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------
void OpenInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", L"explorer.exe",
                  (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenNotepad(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr,
                  SW_SHOWNORMAL);
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(mem);
    if (dst) {
        memcpy(dst, text.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    } else {
        GlobalFree(mem);
    }
    CloseClipboard();
    return true;
}

}  // namespace fsplit
