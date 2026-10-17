// ============================================================================
//  zip_volume.cpp  ——  分卷 ZIP 的写入 / 校验 / 错误文案
//
//  ▍为什么是"分卷 ZIP"而不是"文件切片"？
//    ZIP 规范（APPNOTE 6.3.7 §8.5 "Split and spanned archives"）定义了多卷归档：
//      · 每卷是一个独立文件，按顺序拼起来才是一个完整 zip；
//      · 除最后一片外，每片末尾要写 4 字节签名 0x08074b50（和数据描述符同值），
//        用作"分卷结束标记"，解压工具据此判断边界，而不是当成数据损坏；
//      · 最后一片里有中央目录 + EOCD，还有 4 字节的多卷结束标记 0x06054b50。
//    所以 xxx.zip.001 本身不是"半个 zip"，而是一个合法的、分卷归档的第一片——
//    7-Zip / WinRAR / Info-ZIP 都能直接打开 xxx.zip.001 解压。
//
//  ▍关键实现点
//    1) 流式写入：Local File Header 里 crc/size 先写 0，并在通用标志位 bit3 声明
//       "后面跟数据描述符"，数据写完后再补 descriptor。这样不需要预读一遍源文件。
//    2) 单条目归档 → 中央目录只有 1 项，收尾结构很小，几乎总能塞进最后一片。
//    3) 所有 64 位字段都同时写 ZIP64 扩展字段（值正确），避免 >4GB 时出错。
//    4) 卷内偏移用 writer 自己维护的"本卷已写字节"精确计算，不靠取模。
// ============================================================================
#include "zip_volume.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "crc32.h"
#include "util.h"

namespace fsplit {
namespace {

// ---- 结构签名 ----
constexpr DWORD kSigLocal = 0x04034b50;
constexpr DWORD kSigCentral = 0x02014b50;
constexpr DWORD kSigEocd = 0x06054b50;
constexpr DWORD kSigZip64Eocd = 0x06064b50;
constexpr DWORD kSigZip64Locator = 0x07064b50;
constexpr DWORD kSigDescriptor = 0x08074b50;
constexpr DWORD kSigSplitMarker = 0x08074b50;  // 分卷结束标记（与描述符同值，语义由其位置决定）
constexpr DWORD kSigLastVolumeMarker = 0x06054b50;  // 最后一片的多卷结束标记

constexpr unsigned short kFlagDataDescriptor = 0x0008;
constexpr unsigned short kFlagUtf8 = 0x0800;
constexpr unsigned short kMethodStore = 0;
constexpr unsigned short kVersion20 = 20;
constexpr unsigned short kVersion45 = 45;
constexpr unsigned short kMadeBy = 0x032D;  // UNIX(3) << 8 | 4.5

constexpr size_t kBufferSize = 4u << 20;   // 4MB 读写缓冲
constexpr DWORD kProgressIntervalMs = 90;  // 进度回调节流

// 单条目 + EOCD 的收尾最大体积（中央目录项 ~ 30+名字+扩展，EOCD 22，ZIP64 56+20，标记 4）
constexpr std::uint64_t kMaxTailReserve = 4096;

// ---------------------------------------------------------------------------
//  小端写入
// ---------------------------------------------------------------------------
inline void Put16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
}
inline void Put32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}
inline void Put64(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}

void FileTimeToDos(const FILETIME& ft, std::uint16_t& dosDate, std::uint16_t& dosTime) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) {
        dosDate = 0x0021;  // 1980-01-01
        dosTime = 0;
        return;
    }
    int year = st.wYear < 1980 ? 1980 : (st.wYear > 2107 ? 2107 : st.wYear);
    dosDate = static_cast<std::uint16_t>(((year - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    dosTime = static_cast<std::uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}

// ---------------------------------------------------------------------------
//  归档内文件名编码
//  Windows 版 zip 工具的通行做法是按系统 ANSI 代码页（简体中文 = CP936/GBK）编码；
//  这样资源管理器、WinRAR、7-Zip 在中文系统上都能正常显示。若出现 ANSI 无法表示的字符
//  （极少见，如 emoji），退化为 UTF-8 并置 bit11，现代解压工具按 UTF-8 解释。
// ---------------------------------------------------------------------------
bool EncodeEntryName(const std::wstring& wideName, std::vector<unsigned char>& out, bool& isUtf8) {
    out.clear();
    isUtf8 = false;
    if (wideName.empty()) return false;
    const int len = static_cast<int>(wideName.size());

    int need = WideCharToMultiByte(CP_ACP, 0, wideName.c_str(), len, nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string ansi(static_cast<size_t>(need), '\0');
        if (WideCharToMultiByte(CP_ACP, 0, wideName.c_str(), len, ansi.data(), need, nullptr, nullptr) == need) {
            // 校验可逆性（有些字符会被替换成 '?'）
            int backLen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), need, nullptr, 0);
            if (backLen > 0) {
                std::wstring back(static_cast<size_t>(backLen), L'\0');
                MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), need, back.data(), backLen);
                if (back == wideName) {
                    out.assign(ansi.begin(), ansi.end());
                    return true;
                }
            }
        }
    }

    int need8 = WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), len, nullptr, 0, nullptr, nullptr);
    if (need8 <= 0) return false;
    std::string utf8(static_cast<size_t>(need8), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), len, utf8.data(), need8, nullptr, nullptr);
    out.assign(utf8.begin(), utf8.end());
    isUtf8 = true;
    return true;
}

std::wstring SanitizeEntryName(const std::wstring& fileName) {
    std::wstring name;
    name.reserve(fileName.size());
    for (wchar_t ch : fileName) {
        switch (ch) {
            case L'/':
            case L'\\':
            case L':':
            case L'*':
            case L'?':
            case L'"':
            case L'<':
            case L'>':
            case L'|':
                name.push_back(L'_');
                break;
            default:
                name.push_back(ch);
        }
    }
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name;
}

// ---------------------------------------------------------------------------
//  分卷输出设备
// ---------------------------------------------------------------------------
class VolumeWriter {
public:
    VolumeWriter(std::wstring dir, std::wstring baseName, std::uint64_t volSize)
        : dir_(std::move(dir)), baseName_(std::move(baseName)), volSize_(volSize) {}

    ~VolumeWriter() { CloseCurrent(); }

    const std::vector<std::wstring>& volumes() const { return volumes_; }
    std::uint64_t total() const { return total_; }
    std::uint64_t inVolume() const { return written_; }          // 当前卷已写字节
    // 从归档起点算起的「逻辑偏移」：只累计真实数据字节，不含分卷结束标记与偶数补位。
    // 这样把各分卷按顺序拼接、并剥掉每片末尾那 4 字节标记之后，得到的就是一个
    // 普通 zip，里面所有偏移（local header / 中央目录）都自洽，任何解压工具都能直接读。
    // 若把标记也计入偏移，拼接后中央目录会指向错误位置（这正是之前的 bug）。
    std::uint64_t archiveOffset() const { return closedData_ + written_; }
    std::uint64_t roomLeft() const { return volSize_ - written_; }  // 当前卷剩余
    std::uint64_t volumeSize() const { return volSize_; }
    bool hasOpenVolume() const { return h_ != INVALID_HANDLE_VALUE; }

    // 已打开的卷数量（未开卷时为 0）
    std::uint16_t currentVolumeIndex() const {
        return static_cast<std::uint16_t>(volumes_.empty() ? 0 : volumes_.size() - 1);
    }

    // 关闭当前卷（下次 Write 会自动开新卷）
    void CloseCurrent() {
        if (h_ != INVALID_HANDLE_VALUE) {
            closedData_ += dataBytes_;  // 只累计数据字节，标记/补位不计入逻辑偏移
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    // 在"当前卷"的末尾追加原始字节（不做换卷），用于分卷结束标记与对齐
    Error AppendToCurrentVolume(const void* data, size_t len) {
        if (h_ == INVALID_HANDLE_VALUE) return Error::none();
        DWORD done = 0;
        if (!WriteFile(h_, data, static_cast<DWORD>(len), &done, nullptr) || done != len)
            return Error::make(ErrorCode::WriteFailed,
                               CurrentName() + L"：" + LastErrorText(GetLastError()));
        return Error::none();
    }

    // 在当前卷尾部写 4 字节分卷结束标记（先对齐成偶数长度）
    // 非末卷必须以此收尾，解压工具才认这是"分卷边界"而不是"文件截断"。
    Error AppendSplitMarker() {
        if (h_ == INVALID_HANDLE_VALUE) return Error::none();
        // APPNOTE 要求卷长度为偶数：若写入标记后是奇数，先补 1 字节。
        // 这里直接用 GetFileSizeEx 读真实长度，避免内部计数与实际文件不一致时判断出错。
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h_, &sz)) return Error::none();
        if (((sz.QuadPart + 4) % 2) != 0) {
            const unsigned char z = 0;
            Error e = AppendToCurrentVolume(&z, 1);
            if (!e.ok()) return e;
            ++written_;
            ++total_;
        }
        unsigned char mk[4] = {0x50, 0x4b, 0x07, 0x08};
        Error e = AppendToCurrentVolume(mk, 4);
        if (e.ok()) {
            written_ += 4;
            total_ += 4;
        }
        return e;
    }

    // 末卷收尾：只补齐偶数长度（APPNOTE 8.5 要求每卷长度为偶数）。
    // 绝不在这里写 0x06054b50——EOCD 已经写过了，重复会让解压工具判定归档损坏。
    Error PadLastVolumeToEven() {
        if (h_ == INVALID_HANDLE_VALUE) return Error::none();
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h_, &sz)) return Error::none();
        if ((sz.QuadPart % 2) == 0) return Error::none();
        const unsigned char z = 0;
        Error e = AppendToCurrentVolume(&z, 1);
        if (e.ok()) {
            ++written_;
            ++total_;
        }
        return e;
    }

    Error Write(const void* data, size_t len) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        while (len > 0) {
            if (h_ == INVALID_HANDLE_VALUE) {
                Error e = OpenNext();
                if (!e.ok()) return e;
            }
            size_t n = static_cast<size_t>(std::min<std::uint64_t>(roomLeft(), len));
            DWORD done = 0;
            if (!WriteFile(h_, p, static_cast<DWORD>(n), &done, nullptr) || done != n) {
                DWORD err = GetLastError();
                if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL)
                    return Error::make(ErrorCode::DiskFull,
                                       L"写入 " + CurrentName() + L" 时目标磁盘已满");
                return Error::make(ErrorCode::WriteFailed, CurrentName() + L"：" + LastErrorText(err));
            }
            written_ += done;
            total_ += done;
            dataBytes_ += done;  // 真实数据字节（用于逻辑偏移）
            p += done;
            len -= done;
            if (written_ >= volSize_) {
                // 卷写满：补上分卷结束标记再关闭（除末卷外每片都必须以此收尾）。
                Error e = AppendSplitMarker();
                (void)e;
                CloseCurrent();
            }
        }
        return Error::none();
    }

    // 删除已经产生的所有分卷（用于失败回滚）
    void Cleanup() {
        CloseCurrent();
        for (auto& v : volumes_) DeleteFileW(v.c_str());
        volumes_.clear();
    }

private:
    std::wstring CurrentName() const {
        return volumes_.empty() ? baseName_ : FileNameOf(volumes_.back());
    }

    Error OpenNext() {
        std::wstring name =
            baseName_ + L"." + FormatVolumeIndex(static_cast<int>(volumes_.size()) + 1);
        std::wstring full = JoinPath(dir_, name);
        h_ = CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ErrorCode code = (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
                                 ? ErrorCode::OutputLocked
                                 : (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL)
                                       ? ErrorCode::DiskFull
                                       : ErrorCode::OutputOpenFailed;
            return Error::make(code, name + L"（" + LastErrorText(err) + L"）");
        }
        volumes_.push_back(full);
        written_ = 0;
        dataBytes_ = 0;
        return Error::none();
    }

    std::wstring dir_;
    std::wstring baseName_;
    std::uint64_t volSize_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t dataBytes_ = 0;   // 本卷内的真实数据字节（不含标记与补位）
    std::uint64_t closedData_ = 0;  // 已关闭各卷的真实数据字节之和
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::vector<std::wstring> volumes_;
};

// 中央目录需要的条目信息
struct EntryInfo {
    std::uint32_t crc = 0;
    unsigned short flags = 0;
    std::uint16_t dosDate = 0;
    std::uint16_t dosTime = 0;
    std::uint16_t localVolume = 0;   // 本地文件头所在卷号（0 起）
    std::uint32_t localOffset = 0;   // 卷内偏移
};

// 在当前卷尾部补一个分卷结束标记（偶数对齐后写 4 字节 0x08074b50），并强制换卷
Error WriteSplitMarker(VolumeWriter& w) {
    return w.AppendSplitMarker();
}

// 进度节流
struct Throttle {
    const ProgressFn& fn;
    DWORD lastTick = 0;
    int lastPercent = -1;
    explicit Throttle(const ProgressFn& f) : fn(f) {}
    void report(int percent, const std::wstring& text, bool force = false) {
        if (!fn) return;
        DWORD now = GetTickCount();
        if (!force && now - lastTick < kProgressIntervalMs) return;
        lastTick = now;
        lastPercent = percent;
        fn(percent, text);
    }
};

// ---------------------------------------------------------------------------
//  极端情况：卷太小放不下文件头/描述符
//  最小卷大小 = 头部(30+名字+20) + 至少 1 块数据 + 描述符(16|24) + 分卷标记(4)
// ---------------------------------------------------------------------------
std::uint64_t MinimumVolumeSize(size_t nameLen) {
    return 30 + nameLen + 20 + 24 + 4;
}

}  // namespace

// ---------------------------------------------------------------------------
//  主流程
// ---------------------------------------------------------------------------
SplitResult SplitFileToVolumes(const SplitOptions& opt, const ProgressFn& onProgress,
                              const CancelFn& onCancel) {
    SplitResult result;
    Throttle progress(onProgress);
    result.sourceSize = 0;

    // ---------------- 1. 参数校验 ----------------
    const std::wstring src = NormalizePath(opt.sourcePath);
    const std::wstring outDir = NormalizePath(opt.outputDir);
    const std::wstring baseName = opt.baseName;

    if (src.empty()) {
        result.error = Error::make(ErrorCode::InvalidArgument, L"未选择源文件");
        return result;
    }
    if (baseName.empty()) {
        result.error = Error::make(ErrorCode::InvalidArgument, L"分卷基础名为空");
        return result;
    }
    if (opt.volumeSize == 0) {
        result.error = Error::make(ErrorCode::InvalidArgument, L"分卷大小必须大于 0");
        return result;
    }

    const DWORD attrs = GetFileAttributesW(src.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.error = Error::make(ErrorCode::SourceMissing, FileNameOf(src));
        return result;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        result.error = Error::make(ErrorCode::NotRegularFile, FileNameOf(src) + L" 是一个文件夹");
        return result;
    }
    if (attrs & FILE_ATTRIBUTE_OFFLINE) {
        result.error = Error::make(ErrorCode::SourceOpenFailed, L"这是一个仅在线可用的占位文件");
        return result;
    }

    std::wstring dirErr;
    if (!EnsureDirectory(outDir, dirErr)) {
        result.error = Error::make(ErrorCode::OutputDirFailed, dirErr + L"（" + outDir + L"）");
        return result;
    }

    progress.report(0, L"正在读取源文件信息……", true);

    // ---------------- 2. 打开源文件（只允许别人读，禁止写 → 能立刻发现"被占用"） ----------------
    HANDLE hSrc = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        ErrorCode code = (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
                             ? ErrorCode::SourceLocked
                             : ErrorCode::SourceOpenFailed;
        std::wstring detail = LastErrorText(err) +
                              ((err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
                                   ? L"（请先关闭正在使用该文件的程序，例如播放器、编辑软件或资源管理器预览）"
                                   : L"");
        result.error = Error::make(code, detail);
        return result;
    }

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hSrc, &liSize)) {
        CloseHandle(hSrc);
        result.error = Error::make(ErrorCode::SourceOpenFailed, L"读取文件大小失败");
        return result;
    }
    const std::uint64_t srcSize = static_cast<std::uint64_t>(liSize.QuadPart);
    result.sourceSize = srcSize;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad);

    // ---------------- 3. 归档内文件名 ----------------
    const std::wstring cleanName = SanitizeEntryName(FileNameOf(src));
    std::vector<unsigned char> entryName;
    bool utf8Name = false;
    if (!EncodeEntryName(cleanName, entryName, utf8Name)) {
        CloseHandle(hSrc);
        result.error = Error::make(ErrorCode::EntryNameInvalid, cleanName);
        return result;
    }

    // ---------------- 4. 卷大小合理性 ----------------
    const std::uint64_t minVol = MinimumVolumeSize(entryName.size());
    if (opt.volumeSize < minVol) {
        CloseHandle(hSrc);
        result.error = Error::make(ErrorCode::InvalidArgument,
                                   L"分卷大小太小（至少需要 " + HumanSize(minVol) + L"）");
        return result;
    }

    // ---------------- 5. 磁盘空间预检 ----------------
    {
        const std::uint64_t volCount = srcSize / opt.volumeSize + 1 + 1;
        const std::uint64_t need = srcSize + volCount * 64 + (32ull << 20);
        const std::uint64_t freeBytes = FreeSpaceAt(outDir);
        if (freeBytes != UINT64_MAX && need > freeBytes) {
            CloseHandle(hSrc);
            result.error =
                Error::make(ErrorCode::DiskFull, L"目标磁盘仅剩 " + HumanSize(freeBytes) +
                                                     L"，而本次约需 " + HumanSize(need) +
                                                     L"。请更换输出目录，或缩小分卷大小后重试。");
            return result;
        }
    }

    // ---------------- 6. 开始写入 ----------------
    VolumeWriter writer(outDir, baseName, opt.volumeSize);

    const bool needZip64 = srcSize > 0xFFFFFFFFull;
    EntryInfo ce;
    ce.flags = static_cast<unsigned short>(kFlagDataDescriptor | (utf8Name ? kFlagUtf8 : 0));
    FileTimeToDos(fad.ftLastWriteTime, ce.dosDate, ce.dosTime);

    // ---- 6.1 本地文件头（必须整块落在同一卷；卷足够大时必然成立） ----
    // 注意：localOffset 记录的是「从归档起点算起」的绝对偏移。第一片时它就是 0（卷内偏移也 0），
    // 但写出时必须用 archiveOffset()，这样把各分卷物理拼接成完整 zip 后中央目录仍能定位到条目。
    // 条目所在的"起始分卷号"。与 EOCD 保持一致写 0（视作单卷），
    // 否则 .NET / Java 等严格的 zip 实现会认定这是分卷归档而拒绝解压。
    ce.localVolume = 0;
    ce.localOffset = static_cast<std::uint32_t>(writer.archiveOffset());

    std::vector<unsigned char> lfh;
    lfh.reserve(64 + entryName.size());
    Put32(lfh, kSigLocal);
    Put16(lfh, static_cast<unsigned short>(needZip64 ? kVersion45 : kVersion20));
    Put16(lfh, ce.flags);
    Put16(lfh, kMethodStore);
    Put16(lfh, ce.dosTime);
    Put16(lfh, ce.dosDate);
    Put32(lfh, 0);  // CRC 由描述符给出
    if (needZip64) {
        Put32(lfh, 0xFFFFFFFFu);
        Put32(lfh, 0xFFFFFFFFu);
        Put16(lfh, static_cast<unsigned short>(entryName.size()));
        Put16(lfh, 20);
        Put16(lfh, 0x0001);  // ZIP64 扩展头
        Put16(lfh, 16);
        Put64(lfh, srcSize);
        Put64(lfh, srcSize);
    } else {
        Put32(lfh, 0);
        Put32(lfh, static_cast<std::uint32_t>(srcSize));
        Put16(lfh, static_cast<unsigned short>(entryName.size()));
        Put16(lfh, 0);
    }
    lfh.insert(lfh.end(), entryName.begin(), entryName.end());

    Error err = writer.Write(lfh.data(), lfh.size());
    if (!err.ok()) {
        CloseHandle(hSrc);
        writer.Cleanup();
        result.error = err;
        return result;
    }

    // ---- 6.2 流式复制数据 ----
    progress.report(0, L"开始写入 " + cleanName + L" ……", true);
    {
        std::vector<unsigned char> buf(kBufferSize);
        std::uint64_t done = 0;
        unsigned int crc = 0;
        for (;;) {
            if (onCancel && onCancel()) {
                err = Error::make(ErrorCode::Cancelled);
                break;
            }
            DWORD got = 0;
            if (!ReadFile(hSrc, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                err = Error::make(ErrorCode::ReadFailed, LastErrorText(GetLastError()));
                break;
            }
            if (got == 0) break;
            crc = Crc32Update(crc, buf.data(), got);
            err = writer.Write(buf.data(), got);
            if (!err.ok()) break;
            done += got;
            if (done > srcSize) {
                err = Error::make(ErrorCode::SourceChanged, L"源文件在读取过程中变大了");
                break;
            }
            if (srcSize > 0) {
                int percent = static_cast<int>((done * 100) / srcSize);
                if (percent > 99) percent = 99;
                wchar_t line[320];
                swprintf(line, 320, L"正在写入 %d%%（%s / %s，已生成 %d 个分卷）", percent,
                         HumanSize(done).c_str(), HumanSize(srcSize).c_str(),
                         static_cast<int>(writer.volumes().size()));
                progress.report(percent, line);
            }
        }
        CloseHandle(hSrc);

        if (err.ok() && done != srcSize) {
            err = Error::make(ErrorCode::SourceChanged,
                              L"实际只读到 " + HumanSize(done) + L"，与拆分前的大小 " +
                                  HumanSize(srcSize) + L" 不一致（文件可能被其它程序修改）");
        }
        if (!err.ok()) {
            writer.Cleanup();
            result.error = err;
            result.cancelled = (err.code == ErrorCode::Cancelled);
            return result;
        }
        ce.crc = static_cast<std::uint32_t>(crc);
    }

    progress.report(96, L"正在写入索引……", true);

    // ---- 6.3 数据描述符（带上 CRC 与大小） ----
    // 若当前卷剩余空间不够放描述符：说明数据刚好把本卷填满，只需在卷尾补分卷标记，
    // 描述符会自动落到下一卷开头（Write 见句柄已关闭会开新卷）。
    {
        const size_t descLen = needZip64 ? 24 : 16;
        if (writer.hasOpenVolume() && writer.roomLeft() < descLen) {
            Error e = WriteSplitMarker(writer);
            if (!e.ok()) {
                writer.Cleanup();
                result.error = e;
                return result;
            }
            writer.CloseCurrent();
        }

        std::vector<unsigned char> desc;
        Put32(desc, kSigDescriptor);
        Put32(desc, ce.crc);
        if (needZip64) {
            Put64(desc, srcSize);
            Put64(desc, srcSize);
        } else {
            Put32(desc, static_cast<std::uint32_t>(srcSize));
            Put32(desc, static_cast<std::uint32_t>(srcSize));
        }
        err = writer.Write(desc.data(), desc.size());
        if (!err.ok()) {
            writer.Cleanup();
            result.error = err;
            return result;
        }
    }

    // ---- 6.4 中央目录 ----
    std::vector<unsigned char> cd;
    {
        Put32(cd, kSigCentral);
        Put16(cd, kMadeBy);
        Put16(cd, static_cast<unsigned short>(needZip64 ? kVersion45 : kVersion20));
        Put16(cd, ce.flags);
        Put16(cd, kMethodStore);
        Put16(cd, ce.dosTime);
        Put16(cd, ce.dosDate);
        Put32(cd, ce.crc);

        std::vector<unsigned char> extra;
        if (needZip64) {
            Put32(cd, 0xFFFFFFFFu);
            Put32(cd, 0xFFFFFFFFu);
            Put16(cd, static_cast<unsigned short>(entryName.size()));
            // extra 内容：未压缩(8) + 压缩(8) + 本地头偏移(4) = 20
            Put16(cd, 20);
            Put16(extra, 0x0001);
            Put16(extra, 16);
            Put64(extra, srcSize);
            Put64(extra, srcSize);
        } else {
            Put32(cd, static_cast<std::uint32_t>(srcSize));
            Put32(cd, static_cast<std::uint32_t>(srcSize));
            Put16(cd, static_cast<unsigned short>(entryName.size()));
            Put16(cd, 0);
        }
        Put16(cd, 0);  // comment len
        Put16(cd, ce.localVolume);
        Put16(cd, 0);  // internal attrs
        Put32(cd, 0);  // external attrs
        Put32(cd, ce.localOffset);
        cd.insert(cd.end(), extra.begin(), extra.end());
        cd.insert(cd.end(), entryName.begin(), entryName.end());
    }

    // 收尾结构预留：中央目录 + EOCD(22) + ZIP64 结构(56+20) + 结束标记(4) + 对齐
    const std::uint64_t tailSize = cd.size() + 22 + 4 + (needZip64 ? 76 : 0) + 4;
    if (writer.hasOpenVolume() && writer.roomLeft() < tailSize) {
        // 当前卷放不下索引：先补分卷标记，再换新卷
        Error e = WriteSplitMarker(writer);
        if (!e.ok()) {
            writer.Cleanup();
            result.error = e;
            return result;
        }
        writer.CloseCurrent();  // 中央目录落到新卷
    }

    const std::uint16_t cdVolume = writer.currentVolumeIndex();
    // 中央目录偏移同样必须是归档绝对偏移（不是卷内偏移），否则拼接后无法定位
    const std::uint64_t cdOffset64 = writer.archiveOffset();
    const std::uint32_t cdOffset = static_cast<std::uint32_t>(cdOffset64);

    err = writer.Write(cd.data(), cd.size());
    if (!err.ok()) {
        writer.Cleanup();
        result.error = err;
        return result;
    }

    const std::uint16_t lastVolume = writer.currentVolumeIndex();
    const std::uint16_t volumeCount = static_cast<std::uint16_t>(writer.volumes().size());
    if (needZip64 || volumeCount > 0xFFFE) {
        // ZIP64 目录结束记录
        std::uint64_t z64Offset = writer.archiveOffset();  // 绝对偏移
        std::vector<unsigned char> z64;
        Put32(z64, kSigZip64Eocd);
        Put64(z64, 44);
        Put16(z64, kMadeBy);
        Put16(z64, kVersion45);
        Put32(z64, static_cast<std::uint32_t>(lastVolume));
        Put32(z64, static_cast<std::uint32_t>(cdVolume));
        Put64(z64, 1);
        Put64(z64, 1);
        Put64(z64, static_cast<std::uint64_t>(cd.size()));
        Put64(z64, static_cast<std::uint64_t>(cdOffset));

        std::vector<unsigned char> loc;
        Put32(loc, kSigZip64Locator);
        Put32(loc, static_cast<std::uint32_t>(lastVolume));
        Put64(loc, z64Offset);
        Put32(loc, 1);

        err = writer.Write(z64.data(), z64.size());
        if (err.ok()) err = writer.Write(loc.data(), loc.size());
        if (!err.ok()) {
            writer.Cleanup();
            result.error = err;
            return result;
        }
    }

    // EOCD（目录结束记录）
    // ▍为什么卷号字段写 0 而不是真实卷号？
    //   APPNOTE 规定这两个字段是"本片编号 / 中央目录所在片编号"。若如实写 10 / 5，
    //   那么 xxx.zip.001 会被识别为"分卷归档"，.NET 的 ZipArchive 会直接拒绝解压
    //   （异常：不支持拆分或跨区的存档）。
    //   写 0/0 之后，这一片在解压工具看来就是一个**普通的单卷 zip**：
    //     · 7-Zip / WinRAR 双击 .001 直接打开、解压都没问题；
    //     · 合并脚本把它当普通 zip 读，能稳定还原出原文件；
    //     · 中间片仍然带 4 字节分卷结束标记，分卷语义不丢失。
    //   代价：少数工具不再把它标注为"多卷"，但不影响任何实际使用。
    std::vector<unsigned char> eocd;
    Put32(eocd, kSigEocd);
    Put16(eocd, 0);  // 本片编号（0 = 视作单卷，见上方说明）
    Put16(eocd, 0);  // 中央目录所在片（0）
    Put16(eocd, 1);  // 本片条目数
    Put16(eocd, 1);  // 总条目数
    Put32(eocd, static_cast<std::uint32_t>(cd.size()));
    Put32(eocd, cdOffset);
    Put16(eocd, 0);  // 注释长度

    err = writer.Write(eocd.data(), eocd.size());
    if (!err.ok()) {
        writer.Cleanup();
        result.error = err;
        return result;
    }

    // ---- 6.5 末卷收尾 ----
    // 注意：EOCD(0x06054b50) 本身就是"归档结束"的标志，后面**不能再**追加任何签名。
    // 早期版本在 EOCD 之后又补了一个 0x06054b50，Python zipfile 会因此直接判定
    // "File is not a zip file"（它在末尾找到的 EOCD 起点落在多余标记之后，注释长度对不上）。
    // 这里只做一件事：若末卷总长为奇数，补 1 字节对齐（APPNOTE 8.5 要求每卷长度为偶数）。
    {
        if (writer.hasOpenVolume()) err = writer.PadLastVolumeToEven();
        writer.CloseCurrent();
        if (!err.ok()) {
            writer.Cleanup();
            result.error = err;
            return result;
        }
    }

    progress.report(100, L"拆分完成", true);

    result.success = true;
    result.volumes = writer.volumes();
    result.totalBytes = writer.total();
    result.zipBasePath = JoinPath(outDir, baseName + L".zip");
    return result;
}

// ---------------------------------------------------------------------------
//  分卷集合校验（合并脚本与 GUI 都会用到）
// ---------------------------------------------------------------------------
Error VerifyVolumeSet(const std::wstring& firstVolume) {
    if (firstVolume.empty()) return Error::make(ErrorCode::InvalidArgument, L"未指定分卷");
    if (!FileExists(firstVolume)) return Error::make(ErrorCode::SourceMissing, FileNameOf(firstVolume));

    // 解析出 前缀.bat 与序号：xxx.zip.001 → base = xxx.zip
    std::wstring name = FileNameOf(firstVolume);
    std::wstring dir = NormalizePath(GetParentDir(firstVolume));  // 统一分隔符，去掉尾部斜杠
    if (dir.empty()) dir = L".";
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return Error::make(ErrorCode::InvalidArgument, L"分卷名不符合 xxx.zip.NNN 规则");
    const std::wstring ext = name.substr(dot + 1);
    if (ext.size() != 3 || ext.find_first_not_of(L"0123456789") != std::wstring::npos)
        return Error::make(ErrorCode::InvalidArgument, L"分卷扩展名不是三位数字");
    const std::wstring base = name.substr(0, dot);

    // 先把目录里实际存在的 xxx.zip.NNN 全部列出来，再判断连续性。
    // 直接扫目录（而不是 1..999 逐个探测）更可靠，也能顺便发现"断档后还有文件"的情况。
    std::vector<int> present;
    {
        WIN32_FIND_DATAW fd{};
        std::wstring pattern = JoinPath(dir, base + L".???");
        HANDLE hf = FindFirstFileW(pattern.c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring fn = fd.cFileName;
                if (fn.size() != base.size() + 4) continue;
                if (fn.compare(0, base.size(), base) != 0) continue;
                if (fn[base.size()] != L'.') continue;
                const std::wstring digits = fn.substr(base.size() + 1);
                if (digits.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
                present.push_back(_wtoi(digits.c_str()));
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        std::sort(present.begin(), present.end());
        present.erase(std::unique(present.begin(), present.end()), present.end());
    }

    if (present.empty()) return Error::make(ErrorCode::SourceMissing, base + L".001");

    int count = 0;
    std::vector<int> holes;
    for (int idx : present) {
        if (idx == count + 1) {
            count = idx;  // 紧跟上一个号，连续
        } else if (holes.size() < 8) {
            holes.push_back(idx);  // 跳号后又出现的卷
        }
    }
    if (!holes.empty()) {
        std::wstring list;
        for (size_t i = 0; i < holes.size() && i < 6; ++i) {
            if (i) list += L"、";
            list += base + L"." + FormatVolumeIndex(holes[i]);
        }
        if (holes.size() > 6) list += L" 等";
        return Error::make(ErrorCode::WriteFailed,
                           L"分卷编号不连续（连续到最后的是 " + base + L"." +
                               FormatVolumeIndex(count) + L"，却又发现了 " + list +
                               L"），请重新拆分或删除多余文件");
    }
    if (count > 990) return Error::make(ErrorCode::InvalidArgument, L"分卷数量超过上限（999）");

    // ---- 签名检查 ----
    // 第一片必须以 Local File Header（PK\x03\x04）开头，这是"这确实是个 zip"的凭证。
    // 后续各片是数据的纯续写，开头可以是任意字节，不能拿 LFH 签名去要求它们；
    // 它们的有效性由「中间片以分卷结束标记 0x08074b50 收尾」来保证。
    auto ReadTailFour = [](const std::wstring& p, std::uint32_t& sig, std::uint64_t& size) -> bool {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        bool ok = GetFileSizeEx(h, &sz) != FALSE;
        size = ok ? static_cast<std::uint64_t>(sz.QuadPart) : 0;
        sig = 0;
        if (ok && size >= 4) {
            LARGE_INTEGER pos{};
            pos.QuadPart = sz.QuadPart - 4;
            SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
            unsigned char b[4] = {0};
            DWORD got = 0;
            if (ReadFile(h, b, 4, &got, nullptr) && got == 4)
                sig = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        }
        CloseHandle(h);
        return ok;
    };

    // 1) 第一片头部
    {
        const std::wstring p = JoinPath(dir, base + L"." + FormatVolumeIndex(1));
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return Error::make(ErrorCode::SourceOpenFailed, FileNameOf(p) + L" 无法打开");
        unsigned char head[4] = {0, 0, 0, 0};
        DWORD got = 0;
        ReadFile(h, head, 4, &got, nullptr);
        CloseHandle(h);
        if (got != 4) return Error::make(ErrorCode::WriteFailed, FileNameOf(p) + L" 文件过短，不是有效分卷");
        const DWORD sig = head[0] | (head[1] << 8) | (head[2] << 16) | (static_cast<DWORD>(head[3]) << 24);
        if (sig != kSigLocal)
            return Error::make(ErrorCode::WriteFailed,
                               FileNameOf(p) + L" 不是有效的分卷（开头没有 ZIP 文件头签名）");
    }

    // 2) 中间各片必须以分卷结束标记收尾
    for (int i = 1; i < count; ++i) {
        const std::wstring p = JoinPath(dir, base + L"." + FormatVolumeIndex(i));
        std::uint32_t sig = 0;
        std::uint64_t size = 0;
        if (!ReadTailFour(p, sig, size))
            return Error::make(ErrorCode::SourceOpenFailed, FileNameOf(p) + L" 无法打开");
        if (size < 4)
            return Error::make(ErrorCode::WriteFailed, FileNameOf(p) + L" 文件过短，可能不完整");
        if (sig != kSigSplitMarker)
            return Error::make(ErrorCode::WriteFailed,
                               FileNameOf(p) + L" 结尾缺少分卷标记（可能被其它工具改过或未下载完整）");
    }

    // 3) 末片应以 EOCD 收尾（允许尾部有 1 字节偶数对齐补位）
    {
        const std::wstring p = JoinPath(dir, base + L"." + FormatVolumeIndex(count));
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return Error::make(ErrorCode::SourceOpenFailed, FileNameOf(p) + L" 无法打开");
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        std::uint64_t size = static_cast<std::uint64_t>(sz.QuadPart);
        std::vector<unsigned char> tail;
        if (size > 0) {
            const std::uint64_t take = size < 128 ? size : 128;
            LARGE_INTEGER pos{};
            pos.QuadPart = sz.QuadPart - static_cast<LONGLONG>(take);
            SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
            tail.resize(static_cast<size_t>(take));
            DWORD got = 0;
            if (!ReadFile(h, tail.data(), static_cast<DWORD>(take), &got, nullptr)) tail.clear();
            else tail.resize(got);
        }
        CloseHandle(h);
        // 从尾部 128 字节里找最后一个 EOCD，并要求它之后只剩 22 或 23 字节
        // （23 = EOCD 之后又补了 1 字节做偶数对齐）
        bool foundEocdAtEnd = false;
        for (size_t i = 0; i + 4 <= tail.size(); ++i) {
            if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
                const size_t rest = tail.size() - i;
                if (rest == 22 || rest == 23) foundEocdAtEnd = true;
            }
        }
        if (!foundEocdAtEnd)
            return Error::make(ErrorCode::WriteFailed,
                               FileNameOf(p) + L" 结尾没有归档结束标记（末卷可能不完整或已损坏）");
    }
    return Error::none();
}

}  // namespace fsplit
