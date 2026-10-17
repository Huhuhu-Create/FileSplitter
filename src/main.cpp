// ============================================================================
//  main.cpp  ——  文件分卷工具（GUI）
//
//  ▍GUI 方案选型
//    纯 Win32 API + 标准控件（ComboBox / Edit / Button / Progress / ListBox）
//    + 少量 GDI+ 美化（圆角卡片、渐变进度条）。
//    · 不引入 Qt / wxWidgets / imgui：单 exe、无需运行库、体积 200KB 级别；
//    · 不用 WinUI/XAML：那要带一堆依赖，且老系统跑不起来；
//    · 所有字符串走宽字符（Unicode）API，中文路径零障碍。
//    · 高 DPI 感知：Per-Monitor V2 优先，失败退 System DPI，再退 96DPI 缩放。
//
//  ▍线程模型
//    拆分在后台 std::thread 里跑；worker 只通过 PostMessage 把"进度文本/完成/日志"
//    投递给 UI 线程，UI 线程独占所有控件句柄 → 不会出现跨线程操作窗口的问题。
// ============================================================================
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <objidl.h>
#include <gdiplus.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "merge_script.h"
#include "util.h"
#include "zip_volume.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace fsplit;

// ---------------------------------------------------------------------------
//  控件 ID
// ---------------------------------------------------------------------------
enum : int {
    IDC_SRC_EDIT = 1001,
    IDC_SRC_BROWSE,
    IDC_OUT_EDIT,
    IDC_OUT_BROWSE,
    IDC_SEG_COMBO,
    IDC_SEG_EDIT,
    IDC_SEG_UNIT,
    IDC_NAME_EDIT,
    IDC_BTN_START,
    IDC_BTN_CANCEL,
    IDC_BTN_OPEN,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_LOG,
    IDC_INFO,
    IDC_LINK_DIAG,
};

// ---------------------------------------------------------------------------
//  自定义消息（worker -> UI）
// ---------------------------------------------------------------------------
constexpr UINT WM_APP_PROGRESS = WM_APP + 1;  // wParam: percent, lParam: std::wstring*
constexpr UINT WM_APP_LOG = WM_APP + 2;       // lParam: std::wstring*
constexpr UINT WM_APP_DONE = WM_APP + 3;      // wParam: 1=成功 0=失败, lParam: std::wstring*
constexpr UINT WM_APP_MODE = WM_APP + 4;      // wParam: percent, lParam: std::wstring*（阶段说明）

// ---------------------------------------------------------------------------
//  颜色（深色主题，与 IDE 一致）
// ---------------------------------------------------------------------------
namespace theme {
const COLORREF kBg = RGB(0x1E, 0x1F, 0x22);
const COLORREF kCard = RGB(0x25, 0x26, 0x2B);
const COLORREF kCardAlt = RGB(0x2C, 0x2E, 0x34);
const COLORREF kBorder = RGB(0x3A, 0x3D, 0x44);
const COLORREF kText = RGB(0xE8, 0xE9, 0xEC);
const COLORREF kTextDim = RGB(0x9A, 0x9E, 0xA8);
const COLORREF kAccent = RGB(0x3D, 0x8B, 0xFD);
const COLORREF kAccentHi = RGB(0x5A, 0xA0, 0xFF);
const COLORREF kOk = RGB(0x3F, 0xC1, 0x7A);
const COLORREF kErr = RGB(0xF0, 0x6A, 0x6A);
const COLORREF kWarn = RGB(0xE8, 0xB3, 0x3F);
const COLORREF kLogBg = RGB(0x17, 0x18, 0x1B);
}  // namespace theme

// ---------------------------------------------------------------------------
//  分卷大小选项
// ---------------------------------------------------------------------------
struct SegPreset {
    const wchar_t* label;
    std::uint64_t bytes;
};

static const SegPreset g_presets[] = {
    {L"1 MB", 1ull << 20},
    {L"2 MB", 2ull << 20},
    {L"5 MB", 5ull << 20},
    {L"10 MB", 10ull << 20},
    {L"20 MB", 20ull << 20},
    {L"50 MB", 50ull << 20},
    {L"100 MB", 100ull << 20},
    {L"200 MB", 200ull << 20},
    {L"500 MB", 500ull << 20},
    {L"1 GB", 1ull << 30},
    {L"2 GB", 2ull << 30},
    {L"4 GB", 4ull << 30},
};

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hwnd = nullptr;
static HWND g_editSrc = nullptr;
static HWND g_btnSrc = nullptr;
static HWND g_editOut = nullptr;
static HWND g_btnOut = nullptr;
static HWND g_cmbSeg = nullptr;
static HWND g_editSeg = nullptr;
static HWND g_lblUnit = nullptr;
static HWND g_editName = nullptr;
static HWND g_btnStart = nullptr;
static HWND g_btnCancel = nullptr;
static HWND g_btnOpen = nullptr;
static HWND g_progress = nullptr;
static HWND g_status = nullptr;
static HWND g_log = nullptr;
static HWND g_info = nullptr;

static HFONT g_fontUI = nullptr;
static HFONT g_fontBold = nullptr;
static HFONT g_fontSmall = nullptr;
static HFONT g_fontMono = nullptr;

static HBRUSH g_brBg = nullptr;
static HBRUSH g_brCard = nullptr;
static HBRUSH g_brLog = nullptr;

static std::thread g_worker;
static std::atomic<bool> g_cancelFlag{false};
static std::atomic<bool> g_running{false};
static int g_percent = 0;
static std::wstring g_lastOutDir;
static std::wstring g_lastBatPath;
static std::wstring g_lastZipBase;
static std::vector<std::wstring> g_lastVolumes;

// 分卷设置持久化（记住上次的选择）
struct Settings {
    std::wstring lastOutDir;
    int presetIndex = 6;      // 默认 100 MB
    int unitIndex = 0;        // 0=MB 1=GB
    std::wstring customValue; // 手动输入值
};
static Settings g_settings;
static const wchar_t* kRegKey = L"Software\\FileSplitterTool";

// ---------------------------------------------------------------------------
//  简易 layout 辅助（按 DPI 缩放）
// ---------------------------------------------------------------------------
static int g_dpi = 96;
static inline int D(int v) { return MulDiv(v, g_dpi, 96); }

static void SetFont(HWND c, HFONT f) {
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
}

// ---------------------------------------------------------------------------
//  日志
// ---------------------------------------------------------------------------
static void LogAppend(const std::wstring& line, COLORREF color) {
    if (!g_log) return;
    // 用颜色：ListBox 不支持逐行颜色，这里用 [时间] 前缀 + 文本；重要信息同时写状态栏
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    std::wstring full = std::wstring(stamp) + line;

    int count = static_cast<int>(SendMessageW(g_log, LB_GETCOUNT, 0, 0));
    if (count > 2000) {  // 防止无限增长
        SendMessageW(g_log, LB_DELETESTRING, 0, 0);
        count--;
    }
    int idx = static_cast<int>(SendMessageW(g_log, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(full.c_str())));
    if (idx >= 0) SendMessageW(g_log, LB_SETTOPINDEX, static_cast<WPARAM>(idx), 0);
    (void)color;
}

static void SetStatus(const std::wstring& text, COLORREF color = theme::kTextDim) {
    if (!g_status) return;
    SetWindowTextW(g_status, text.c_str());
    InvalidateRect(g_status, nullptr, TRUE);
    (void)color;
}

// ---------------------------------------------------------------------------
//  设置读写
// ---------------------------------------------------------------------------
static void LoadSettings() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024];
        DWORD size = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(k, L"OutDir", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            size /= sizeof(wchar_t);
            buf[size] = L'\0';
            g_settings.lastOutDir = buf;
        }
        DWORD val = 0, sz = sizeof(val);
        if (RegQueryValueExW(k, L"Preset", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &sz) ==
            ERROR_SUCCESS)
            g_settings.presetIndex = static_cast<int>(val);
        sz = sizeof(val);
        if (RegQueryValueExW(k, L"Unit", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &sz) ==
            ERROR_SUCCESS)
            g_settings.unitIndex = static_cast<int>(val);
        size = sizeof(buf) - sizeof(wchar_t);
        if (RegQueryValueExW(k, L"Custom", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            size /= sizeof(wchar_t);
            buf[size] = L'\0';
            g_settings.customValue = buf;
        }
        RegCloseKey(k);
    }
}

static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (!g_settings.lastOutDir.empty())
        RegSetValueExW(k, L"OutDir", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(g_settings.lastOutDir.c_str()),
                       static_cast<DWORD>((g_settings.lastOutDir.size() + 1) * sizeof(wchar_t)));
    DWORD v = static_cast<DWORD>(g_settings.presetIndex);
    RegSetValueExW(k, L"Preset", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    v = static_cast<DWORD>(g_settings.unitIndex);
    RegSetValueExW(k, L"Unit", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    if (!g_settings.customValue.empty())
        RegSetValueExW(k, L"Custom", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(g_settings.customValue.c_str()),
                       static_cast<DWORD>((g_settings.customValue.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
//  文件 / 文件夹选择对话框
// ---------------------------------------------------------------------------
static std::wstring BrowseFile(HWND owner) {
    wchar_t path[MAX_PATH * 4] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"选择要拆分的文件";
    if (GetOpenFileNameW(&ofn)) return path;
    return std::wstring{};
}

static std::wstring BrowseFolder(HWND owner, const std::wstring& title) {
    IFileDialog* dlg = nullptr;
    std::wstring result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(title.c_str());
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    result = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

// ---------------------------------------------------------------------------
//  读控件文本
// ---------------------------------------------------------------------------
static std::wstring GetText(HWND c) {
    int len = GetWindowTextLengthW(c);
    std::wstring s(static_cast<size_t>(len), L'\0');
    if (len > 0) GetWindowTextW(c, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring{};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 解析分卷大小：优先看"手动输入"框，否则用下拉选项
static bool ResolveSegmentSize(std::uint64_t& out, std::wstring& err) {
    std::wstring raw = Trim(GetText(g_editSeg));
    if (!raw.empty()) {
        // 支持 "100"、"100MB"、"1.5G"、"800KB"
        std::wstring num;
        std::wstring unit;
        for (wchar_t c : raw) {
            if ((c >= L'0' && c <= L'9') || c == L'.') {
                if (!unit.empty()) {
                    err = L"分卷大小格式不正确，请写成 100 或 100MB 这样的形式";
                    return false;
                }
                num.push_back(c);
            } else if (c != L' ') {
                unit.push_back(static_cast<wchar_t>(towupper(c)));
            }
        }
        if (num.empty()) {
            err = L"分卷大小格式不正确";
            return false;
        }
        double v = _wtof(num.c_str());
        if (!(v > 0)) {
            err = L"分卷大小必须大于 0";
            return false;
        }
        std::uint64_t mul = 1ull << 20;  // 默认 MB
        if (!unit.empty()) {
            if (unit == L"B") mul = 1;
            else if (unit == L"K" || unit == L"KB") mul = 1024;
            else if (unit == L"M" || unit == L"MB") mul = 1ull << 20;
            else if (unit == L"G" || unit == L"GB") mul = 1ull << 30;
            else if (unit == L"T" || unit == L"TB") mul = 1ull << 40;
            else {
                err = L"无法识别的单位「" + unit + L"」，可用：KB / MB / GB";
                return false;
            }
        }
        double bytes = v * static_cast<double>(mul);
        if (bytes < 1024.0) {
            err = L"分卷大小不能小于 1 KB";
            return false;
        }
        if (bytes > 1.0e12) {
            err = L"分卷大小过大（最大 1 TB）";
            return false;
        }
        out = static_cast<std::uint64_t>(bytes);
        return true;
    }

    int sel = static_cast<int>(SendMessageW(g_cmbSeg, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(sizeof(g_presets) / sizeof(g_presets[0]))) {
        err = L"请选择或输入分卷大小";
        return false;
    }
    out = g_presets[sel].bytes;
    return true;
}

// ---------------------------------------------------------------------------
//  控件外观（深色主题）
//  标准控件不会跟随系统深色模式，这里用 WM_CTLCOLORxxx 强制配色。
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
//  自定义绘制：卡片背景、进度条
// ---------------------------------------------------------------------------
static void FillRoundRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void PaintProgress(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // 双缓冲，避免闪烁
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // 轨道
    FillRect(mem, &rc, CreateSolidBrush(theme::kCardAlt));
    // 填充
    int w = rc.right - rc.left;
    int fillW = MulDiv(w, g_percent, 100);
    if (fillW > 0) {
        RECT fr = rc;
        fr.right = fr.left + fillW;
        // 简单渐变：左深右亮（按块填充，避免 GDI 渐变 API 依赖）
        const int bars = 60;
        for (int i = 0; i < bars; ++i) {
            int x1 = fr.left + MulDiv(fillW, i, bars);
            int x2 = fr.left + MulDiv(fillW, i + 1, bars);
            int t = MulDiv(255, i, bars - 1);
            COLORREF c = RGB(GetRValue(theme::kAccent) + (GetRValue(theme::kAccentHi) - GetRValue(theme::kAccent)) * t / 255,
                             GetGValue(theme::kAccent) + (GetGValue(theme::kAccentHi) - GetGValue(theme::kAccent)) * t / 255,
                             GetBValue(theme::kAccent) + (GetBValue(theme::kAccentHi) - GetBValue(theme::kAccent)) * t / 255);
            HBRUSH b = CreateSolidBrush(c);
            RECT br2 = {x1, rc.top, x2 + 1, rc.bottom};
            FillRect(mem, &br2, b);
            DeleteObject(b);
        }
    }
    // 百分比文字
    wchar_t txt[16];
    swprintf(txt, 16, L"%d%%", g_percent);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, g_percent > 55 ? RGB(255, 255, 255) : theme::kTextDim);
    SelectObject(mem, g_fontSmall);
    DrawTextW(mem, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// 自绘按钮（主按钮渐变 + 悬停态）
struct ButtonPaint {
    HBRUSH bg = nullptr;
};
static ButtonPaint g_btnStartPaint;

// ---------------------------------------------------------------------------
//  布局
// ---------------------------------------------------------------------------
struct Layout {
    RECT card1, card2, card3;  // 源文件 / 输出 / 分卷
    RECT logRect;
    RECT infoRect;
};

static Layout ComputeLayout(int w, int h) {
    const int pad = D(18);
    const int rowH = D(30);
    const int labelH = D(18);
    const int gap = D(10);

    Layout L{};
    int y = pad;
    int innerW = w - pad * 2;

    // 卡片1：源文件 + 输出目录（两行）
    int card1H = labelH + rowH + gap + labelH + rowH + D(14);
    L.card1 = {pad, y, pad + innerW, y + card1H};
    y = L.card1.bottom + gap;

    // 卡片2：分卷大小 + 基础名 + 按钮区
    int card2H = labelH + rowH + D(12) + rowH + D(12);
    L.card2 = {pad, y, pad + innerW, y + card2H};
    y = L.card2.bottom + gap;

    // 进度 + 状态
    L.card3 = {pad, y, pad + innerW, y + D(56)};
    y = L.card3.bottom + gap;

    // 日志区（占剩余空间）
    L.logRect = {pad, y, pad + innerW, h - pad - D(24)};
    L.infoRect = {pad, h - pad - D(20), pad + innerW, h - pad};
    return L;
}

static void DoLayout(int w, int h) {
    Layout L = ComputeLayout(w, h);
    const int btnW = D(92);

    // ---- 卡片1 ----
    int x = L.card1.left + D(14);
    int cy = L.card1.top + D(12);
    int cw = L.card1.right - L.card1.left - D(28);

    MoveWindow(g_editSrc, x, cy, cw - btnW - D(8), D(30), TRUE);
    MoveWindow(g_btnSrc, x + cw - btnW, cy, btnW, D(30), TRUE);
    cy += D(30) + D(26);
    MoveWindow(g_editOut, x, cy, cw - btnW - D(8), D(30), TRUE);
    MoveWindow(g_btnOut, x + cw - btnW, cy, btnW, D(30), TRUE);

    // ---- 卡片2 ----
    x = L.card2.left + D(14);
    cy = L.card2.top + D(12);
    cw = L.card2.right - L.card2.left - D(28);
    MoveWindow(g_cmbSeg, x, cy, D(130), D(220), TRUE);
    MoveWindow(g_editSeg, x + D(138), cy, D(110), D(30), TRUE);
    MoveWindow(g_lblUnit, x + D(254), cy + D(6), D(120), D(20), TRUE);
    MoveWindow(g_editName, x + cw - D(220), cy, D(220), D(30), TRUE);

    cy += D(30) + D(12);
    int startW = D(108);
    int cxlW = D(84);
    int openW = D(108);
    MoveWindow(g_btnStart, x, cy, startW, D(32), TRUE);
    MoveWindow(g_btnCancel, x + startW + D(10), cy, cxlW, D(32), TRUE);
    MoveWindow(g_btnOpen, x + startW + cxlW + D(20), cy, openW, D(32), TRUE);

    // ---- 进度 ----
    MoveWindow(g_progress, L.card3.left + D(14), L.card3.top + D(12),
               L.card3.right - L.card3.left - D(28), D(14), TRUE);
    MoveWindow(g_status, L.card3.left + D(14), L.card3.top + D(32),
               L.card3.right - L.card3.left - D(28), D(18), TRUE);

    // ---- 日志 ----
    MoveWindow(g_log, L.logRect.left, L.logRect.top, L.logRect.right - L.logRect.left,
               L.logRect.bottom - L.logRect.top, TRUE);
    MoveWindow(g_info, L.infoRect.left, L.infoRect.top, L.infoRect.right - L.infoRect.left,
               L.infoRect.bottom - L.infoRect.top, TRUE);
}

// 需要绘制的"标签"文字（卡片内手写，避免额外控件）
static void PaintLabels(HDC hdc) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    Layout L = ComputeLayout(rc.right, rc.bottom);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, theme::kTextDim);

    int x = L.card1.left + D(14);
    int cw = L.card1.right - L.card1.left - D(28);
    RECT r;

    r = {x, L.card1.top + D(8) - D(6), x + cw, L.card1.top + D(22)};
    DrawTextW(hdc, L"源文件", -1, &r, DT_LEFT | DT_SINGLELINE);

    r = {x, L.card1.top + D(12) + D(30) + D(4), x + cw, L.card1.top + D(12) + D(30) + D(22)};
    DrawTextW(hdc, L"输出目录", -1, &r, DT_LEFT | DT_SINGLELINE);

    // 卡片2 的标签
    int y2 = L.card2.top + D(12) - D(15);
    r = {x, y2, x + D(240), y2 + D(16)};
    DrawTextW(hdc, L"分卷大小", -1, &r, DT_LEFT | DT_SINGLELINE);
    r = {x + cw - D(220) - D(120), y2, x + cw - D(220), y2 + D(16)};
    DrawTextW(hdc, L"分卷基础名", -1, &r, DT_LEFT | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
//  UI 状态切换
// ---------------------------------------------------------------------------
static void SetRunningUI(bool running) {
    EnableWindow(g_btnSrc, !running);
    EnableWindow(g_btnOut, !running);
    EnableWindow(g_cmbSeg, !running);
    EnableWindow(g_editSeg, !running);
    EnableWindow(g_editName, !running);
    EnableWindow(g_editSrc, !running);
    EnableWindow(g_editOut, !running);
    EnableWindow(g_btnStart, !running);
    EnableWindow(g_btnCancel, running);
}

static void SetProgress(int percent) {
    g_percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    InvalidateRect(g_progress, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  校验输入并启动拆分
// ---------------------------------------------------------------------------
static void StartSplit() {
    if (g_running.load()) return;

    std::wstring src = Trim(GetText(g_editSrc));
    std::wstring outDir = Trim(GetText(g_editOut));
    std::wstring baseName = Trim(GetText(g_editName));

    if (src.empty()) {
        MessageBoxW(g_hwnd, L"请先选择要拆分的源文件。", L"提示", MB_ICONINFORMATION);
        SetFocus(g_editSrc);
        return;
    }
    if (IsDirectory(src)) {
        MessageBoxW(g_hwnd, L"所选路径是一个文件夹，本工具只支持拆分单个文件。", L"选择有误",
                    MB_ICONWARNING);
        return;
    }
    if (!FileExists(src)) {
        MessageBoxW(g_hwnd, (L"源文件不存在，可能已被移动或删除：\n\n" + src).c_str(), L"找不到文件",
                    MB_ICONERROR);
        return;
    }
    if (outDir.empty()) outDir = GetParentDir(src);
    if (outDir.empty()) {
        MessageBoxW(g_hwnd, L"请选择输出目录。", L"提示", MB_ICONINFORMATION);
        return;
    }

    std::uint64_t segSize = 0;
    std::wstring segErr;
    if (!ResolveSegmentSize(segSize, segErr)) {
        MessageBoxW(g_hwnd, segErr.c_str(), L"分卷大小无效", MB_ICONWARNING);
        SetFocus(g_editSeg);
        return;
    }

    if (baseName.empty()) {
        baseName = FileNameOf(src);
        SetWindowTextW(g_editName, baseName.c_str());
    }
    // 基础名合法性
    if (baseName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBoxW(g_hwnd, L"分卷基础名不能包含下列字符：\n\\ / : * ? \" < > |", L"名称无效",
                    MB_ICONWARNING);
        SetFocus(g_editName);
        return;
    }
    // 统一以 .zip 结尾（分卷为 name.zip.001）
    {
        std::wstring lower = baseName;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        if (lower.size() < 4 || lower.substr(lower.size() - 4) != L".zip") {
            if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".001")
                baseName = baseName.substr(0, baseName.size() - 4);
            if (Trim(baseName).empty()) baseName = L"archive";
            baseName += L".zip";
            SetWindowTextW(g_editName, baseName.c_str());
        }
    }

    // 源文件大小 & 预估分卷数
    std::uint64_t srcSize = FileSizeOrZero(src);
    int estVols = static_cast<int>(srcSize / segSize) + 1;

    // 目标已存在时提醒
    {
        std::wstring first = JoinPath(outDir, baseName + L".001");
        if (FileExists(first)) {
            int r = MessageBoxW(g_hwnd,
                                (L"输出目录里已经存在同名分卷：\n" + baseName +
                                 L".001\n\n继续操作会覆盖它们，是否继续？")
                                    .c_str(),
                                L"文件已存在", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (r != IDYES) return;
        }
    }

    // 保存设置
    g_settings.lastOutDir = outDir;
    SaveSettings();

    g_lastOutDir = outDir;
    g_lastBatPath.clear();
    g_lastZipBase.clear();
    g_lastVolumes.clear();

    SetRunningUI(true);
    SetProgress(0);
    SetStatus(L"准备中……");

    SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
    LogAppend(L"====== 开始拆分 ======", theme::kText);
    LogAppend(L"源文件  : " + src, theme::kText);
    LogAppend(L"输出目录: " + outDir, theme::kText);
    LogAppend(L"分卷大小: " + HumanSize(segSize) + L"（预计 " + std::to_wstring(estVols) + L" 个分卷）",
              theme::kText);
    LogAppend(L"文件大小: " + HumanSize(srcSize), theme::kText);

    g_cancelFlag.store(false);
    g_running.store(true);

    // 先取 SHA-256（在后台线程做，避免卡界面）
    HWND hwnd = g_hwnd;
    SplitOptions opt;
    opt.sourcePath = src;
    opt.outputDir = outDir;
    opt.baseName = baseName;
    opt.volumeSize = segSize;
    const std::wstring restoreName = FileNameOf(src);

    g_worker = std::thread([hwnd, opt, restoreName]() {
        auto post = [hwnd](UINT msg, WPARAM wp, const std::wstring& text) {
            auto* p = new std::wstring(text);
            PostMessageW(hwnd, msg, wp, reinterpret_cast<LPARAM>(p));
        };

        post(WM_APP_LOG, 0, L"正在计算源文件 SHA-256（用于合并后校验）……");

        // 计算哈希（带取消）
        std::wstring sha;
        {
            // 直接用 SplitFileToVolumes 内的流式读取无法复用，所以这里单独算一遍；
            // 大文件会多读一次盘，但能保证"合并脚本能验证完整性"这一硬需求。
            // 实现：分块读 + BCrypt 与我们 util 中的一致（调用 Sha256OfFile）。
            sha = Sha256OfFile(opt.sourcePath);
        }

        SplitResult result = SplitFileToVolumes(
            opt,
            [&post](int percent, const std::wstring& text) {
                post(WM_APP_PROGRESS, static_cast<WPARAM>(percent), text);
            },
            []() { return g_cancelFlag.load(); });

        if (result.success) {
            post(WM_APP_LOG, 0, L"SHA-256 : " + (sha.empty() ? std::wstring(L"(计算失败，将跳过哈希校验)") : sha));

            // 生成合并脚本
            MergeScriptInfo ms = WriteMergeScripts(
                opt.outputDir, opt.baseName, static_cast<int>(result.volumes.size()),
                result.sourceSize, sha, restoreName, opt.sourcePath);

            std::wstring summary;
            wchar_t line[512];
            swprintf(line, 512, L"共生成 %d 个分卷，合计 %s", static_cast<int>(result.volumes.size()),
                     HumanSize(result.totalBytes).c_str());
            summary = line;
            post(WM_APP_LOG, 0, summary);
            post(WM_APP_LOG, 0, L"合并脚本: " + ms.batPath);

            // 用全局变量传回主线程（只在 post 后立即写，主线程处理 DONE 时读取）
            g_lastVolumes = result.volumes;
            g_lastBatPath = ms.batPath;
            g_lastZipBase = result.zipBasePath;
            post(WM_APP_DONE, 1, summary);
        } else {
            post(WM_APP_LOG, 0, L"失败原因: " + DescribeError(result.error));
            post(WM_APP_DONE, result.cancelled ? 2 : 0, DescribeError(result.error));
        }
        g_running.store(false);
    });
    g_worker.detach();
}

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            const int editStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;

            g_editSrc = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_READONLY,
                                       0, 0, 0, 0, hwnd, (HMENU)IDC_SRC_EDIT, g_hInst, nullptr);
            g_btnSrc = CreateWindowExW(0, L"BUTTON", L"选择文件…",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                       (HMENU)IDC_SRC_BROWSE, g_hInst, nullptr);

            g_editOut = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_READONLY,
                                        0, 0, 0, 0, hwnd, (HMENU)IDC_OUT_EDIT, g_hInst, nullptr);
            g_btnOut = CreateWindowExW(0, L"BUTTON", L"浏览…",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                       (HMENU)IDC_OUT_BROWSE, g_hInst, nullptr);

            g_cmbSeg = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 0, 0, hwnd, (HMENU)IDC_SEG_COMBO, g_hInst, nullptr);
            for (const auto& p : g_presets)
                SendMessageW(g_cmbSeg, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.label));

            g_editSeg = CreateWindowExW(0, L"EDIT", L"", editStyle | ES_NUMBER,
                                        0, 0, 0, 0, hwnd, (HMENU)IDC_SEG_EDIT, g_hInst, nullptr);
            g_lblUnit = CreateWindowExW(0, L"STATIC", L"（留空则用左侧预设）",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                                        (HMENU)IDC_SEG_UNIT, g_hInst, nullptr);

            g_editName = CreateWindowExW(0, L"EDIT", L"", editStyle,
                                         0, 0, 0, 0, hwnd, (HMENU)IDC_NAME_EDIT, g_hInst, nullptr);

            g_btnStart = CreateWindowExW(0, L"BUTTON", L"开始拆分",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                         (HMENU)IDC_BTN_START, g_hInst, nullptr);
            g_btnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0,
                                          0, 0, hwnd, (HMENU)IDC_BTN_CANCEL, g_hInst, nullptr);
            g_btnOpen = CreateWindowExW(0, L"BUTTON", L"打开输出目录",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                        (HMENU)IDC_BTN_OPEN, g_hInst, nullptr);

            g_progress = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                         0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, g_hInst, nullptr);
            g_status = CreateWindowExW(0, L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
            g_log = CreateWindowExW(0, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                                        LBS_NOSEL | WS_BORDER,
                                    0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, g_hInst, nullptr);
            g_info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hwnd, (HMENU)IDC_INFO, g_hInst, nullptr);

            HWND all[] = {g_editSrc, g_btnSrc, g_editOut, g_btnOut, g_cmbSeg, g_editSeg,
                          g_lblUnit, g_editName, g_btnStart, g_btnCancel, g_btnOpen, g_status,
                          g_info};
            for (HWND c : all) SetFont(c, g_fontUI);
            SetFont(g_log, g_fontMono);

            // 恢复上次设置
            SendMessageW(g_cmbSeg, CB_SETCURSEL, g_settings.presetIndex, 0);
            if (!g_settings.customValue.empty())
                SetWindowTextW(g_editSeg, g_settings.customValue.c_str());
            if (!g_settings.lastOutDir.empty())
                SetWindowTextW(g_editOut, g_settings.lastOutDir.c_str());

            SetWindowTextW(g_info,
                           L"提示：合并时双击输出目录里的 merge.bat（需与 merge.ps1 及所有分卷放在一起）");
            return 0;
        }

        case WM_SIZE: {
            if (wp == SIZE_MINIMIZED) return 0;
            DoLayout(LOWORD(lp), HIWORD(lp));
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_brBg);
            Layout L = ComputeLayout(rc.right, rc.bottom);
            FillRoundRect(hdc, L.card1, D(10), theme::kCard, theme::kBorder);
            FillRoundRect(hdc, L.card2, D(10), theme::kCard, theme::kBorder);
            FillRoundRect(hdc, L.card3, D(10), theme::kCard, theme::kBorder);
            // 日志区背景
            HBRUSH lb = CreateSolidBrush(theme::kLogBg);
            RECT lr = L.logRect;
            RoundRect(hdc, lr.left, lr.top, lr.right, lr.bottom, D(10), D(10));
            DeleteObject(lb);
            PaintLabels(hdc);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // 背景 + 卡片
            FillRect(hdc, &rc, g_brBg);
            Layout L = ComputeLayout(rc.right, rc.bottom);
            FillRoundRect(hdc, L.card1, D(10), theme::kCard, theme::kBorder);
            FillRoundRect(hdc, L.card2, D(10), theme::kCard, theme::kBorder);
            FillRoundRect(hdc, L.card3, D(10), theme::kCard, theme::kBorder);
            PaintLabels(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_PROGRESS) {
                PaintProgress(dis->hwndItem);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, theme::kText);
            SetBkColor(hdc, theme::kLogBg);
            return reinterpret_cast<LRESULT>(g_brLog);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            HWND c = reinterpret_cast<HWND>(lp);
            SetTextColor(hdc, IsWindowEnabled(c) ? theme::kText : theme::kTextDim);
            SetBkColor(hdc, theme::kCardAlt);
            return reinterpret_cast<LRESULT>(g_brCard);
        }
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, theme::kText);
            SetBkColor(hdc, theme::kCardAlt);
            return reinterpret_cast<LRESULT>(g_brCard);
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            switch (id) {
                case IDC_SRC_BROWSE: {
                    std::wstring f = BrowseFile(hwnd);
                    if (!f.empty()) {
                        SetWindowTextW(g_editSrc, f.c_str());
                        // 自动填输出目录 & 基础名
                        std::wstring dir = GetParentDir(f);
                        if (GetText(g_editOut).empty() && !dir.empty())
                            SetWindowTextW(g_editOut, dir.c_str());
                        std::wstring name = FileNameOf(f);
                        // 去掉扩展名，拼成 name.zip
                        size_t dot = name.find_last_of(L'.');
                        std::wstring base = (dot != std::wstring::npos && dot > 0)
                                                ? name.substr(0, dot)
                                                : name;
                        SetWindowTextW(g_editName, (base + L".zip").c_str());
                        SetStatus(L"已选择：" + HumanSize(FileSizeOrZero(f)) + L" · " + name);
                    }
                    return 0;
                }
                case IDC_OUT_BROWSE: {
                    std::wstring d = BrowseFolder(hwnd, L"选择分卷输出目录");
                    if (!d.empty()) {
                        SetWindowTextW(g_editOut, d.c_str());
                        g_settings.lastOutDir = d;
                    }
                    return 0;
                }
                case IDC_SEG_COMBO: {
                    if (code == CBN_SELCHANGE) {
                        g_settings.presetIndex =
                            static_cast<int>(SendMessageW(g_cmbSeg, CB_GETCURSEL, 0, 0));
                        // 选择预设时清空手动输入
                        SetWindowTextW(g_editSeg, L"");
                        g_settings.customValue.clear();
                    }
                    return 0;
                }
                case IDC_BTN_START:
                    StartSplit();
                    return 0;
                case IDC_BTN_CANCEL: {
                    if (g_running.load()) {
                        g_cancelFlag.store(true);
                        SetStatus(L"正在取消，等待当前数据块写完……");
                        EnableWindow(g_btnCancel, FALSE);
                        LogAppend(L"用户请求取消，正在安全停止……", theme::kWarn);
                    }
                    return 0;
                }
                case IDC_BTN_OPEN: {
                    std::wstring d = Trim(GetText(g_editOut));
                    if (d.empty() || !IsDirectory(d)) {
                        if (!g_lastOutDir.empty()) d = g_lastOutDir;
                    }
                    if (!d.empty() && IsDirectory(d)) {
                        // 若已生成 merge.bat，优先选中它
                        if (!g_lastBatPath.empty() && FileExists(g_lastBatPath))
                            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                          (L"/select,\"" + g_lastBatPath + L"\"").c_str(), nullptr,
                                          SW_SHOWNORMAL);
                        else
                            OpenInExplorer(d);
                    } else {
                        MessageBoxW(hwnd, L"还没有可打开的输出目录。", L"提示", MB_ICONINFORMATION);
                    }
                    return 0;
                }
                default:
                    break;
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            auto* p = reinterpret_cast<std::wstring*>(lp);
            int percent = static_cast<int>(wp);
            SetProgress(percent);
            if (p) {
                SetStatus(*p + L"（点「取消」可中断）");
                delete p;
            }
            return 0;
        }

        case WM_APP_LOG: {
            auto* p = reinterpret_cast<std::wstring*>(lp);
            if (p) {
                LogAppend(*p, theme::kText);
                delete p;
            }
            return 0;
        }

        case WM_APP_DONE: {
            auto* p = reinterpret_cast<std::wstring*>(lp);
            std::wstring detail = p ? *p : std::wstring();
            delete p;

            g_running.store(false);
            SetRunningUI(false);
            EnableWindow(g_btnCancel, FALSE);

            if (wp == 1) {
                SetProgress(100);
                SetStatus(L"✔ 拆分完成：" + detail, theme::kOk);
                LogAppend(L"====== 拆分完成 ======", theme::kOk);
                LogAppend(L"双击 " + g_lastBatPath + L" 即可合并还原。", theme::kOk);
                int r = MessageBoxW(hwnd,
                                    (L"拆分完成！\n\n" + detail +
                                     L"\n\n是否打开输出目录，查看分卷和 merge.bat？")
                                        .c_str(),
                                    L"完成", MB_ICONINFORMATION | MB_YESNO);
                if (r == IDYES) SendMessageW(hwnd, WM_COMMAND, IDC_BTN_OPEN, 0);
            } else if (wp == 2) {
                SetProgress(0);
                SetStatus(L"已取消", theme::kWarn);
                LogAppend(L"====== 已取消（未完整生成的分卷已清理）======", theme::kWarn);
            } else {
                SetStatus(L"✘ 失败：" + detail, theme::kErr);
                LogAppend(L"====== 失败 ======", theme::kErr);
                MessageBoxW(hwnd, (L"拆分失败：\n\n" + detail).c_str(), L"错误",
                            MB_ICONERROR | MB_OK);
            }
            return 0;
        }

        case WM_CLOSE: {
            if (g_running.load()) {
                int r = MessageBoxW(hwnd, L"正在拆分，确定要退出吗？\n未完成的分卷会被清理。", L"确认退出",
                                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                if (r != IDYES) return 0;
                g_cancelFlag.store(true);
                // 等一小会儿让 worker 收尾
                for (int i = 0; i < 50 && g_running.load(); ++i) Sleep(50);
            }
            SaveSettings();
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  DPI 感知
// ---------------------------------------------------------------------------
static void EnableDpiAwareness() {
    // Per-Monitor V2（Win10 1703+）
    using SetCtxFn = BOOL(WINAPI*)(void*);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setCtx = reinterpret_cast<SetCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        const void* perMonitorV2 = reinterpret_cast<const void*>(static_cast<INT_PTR>(-4));
        if (setCtx && setCtx(const_cast<void*>(perMonitorV2))) return;
        auto setAware = reinterpret_cast<BOOL(WINAPI*)()>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDPIAware")));
        if (setAware && setAware()) return;
    }
    SetProcessDPIAware();
}

static void UpdateDpiScale(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    if (g_dpi <= 0) g_dpi = 96;

    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontMono) DeleteObject(g_fontMono);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
        g_fontUI = CreateFontIndirectW(&lf);
        lf.lfHeight = -MulDiv(7, g_dpi, 72);
        g_fontSmall = CreateFontIndirectW(&lf);
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        lf.lfWeight = FW_SEMIBOLD;
        g_fontBold = CreateFontIndirectW(&lf);
        LOGFONTW fm = lf;
        wcscpy_s(fm.lfFaceName, L"Consolas");
        fm.lfWeight = FW_NORMAL;
        fm.lfHeight = -MulDiv(9, g_dpi, 72);
        g_fontMono = CreateFontIndirectW(&fm);
    }
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInst;

    // 单实例：已有窗口时激活它
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\FileSplitterTool_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND exist = FindWindowW(L"FileSplitterWndClass", nullptr);
        if (exist) {
            ShowWindow(exist, SW_RESTORE);
            SetForegroundWindow(exist);
            return 0;
        }
    }

    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    LoadSettings();

    g_brBg = CreateSolidBrush(theme::kBg);
    g_brCard = CreateSolidBrush(theme::kCard);
    g_brLog = CreateSolidBrush(theme::kLogBg);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"FileSplitterWndClass";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 窗口尺寸（96DPI 基准 720x640）
    RECT wr = {0, 0, 720, 640};
    HDC screen = GetDC(nullptr);
    int screenDpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    wr.right = MulDiv(wr.right, screenDpi, 96);
    wr.bottom = MulDiv(wr.bottom, screenDpi, 96);
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, L"FileSplitterWndClass", L"文件分卷工具 · File Splitter",
                                (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    UpdateDpiScale(hwnd);
    // 窗口创建时控件已按旧 DPI 布局，这里重新刷新字体与布局
    {
        HWND all[] = {g_editSrc, g_btnSrc, g_editOut, g_btnOut, g_cmbSeg, g_editSeg, g_lblUnit,
                      g_editName, g_btnStart, g_btnCancel, g_btnOpen, g_status, g_info};
        for (HWND c : all) SetFont(c, g_fontUI);
        SetFont(g_log, g_fontMono);
        RECT rc;
        GetClientRect(hwnd, &rc);
        DoLayout(rc.right, rc.bottom);
    }

    // 深色标题栏（Win10 1809+；失败无副作用）
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
