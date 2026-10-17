#include <windows.h>
#include <cstdio>
#include <string>
#include "zip_volume.h"
#include "util.h"
using namespace fsplit;
int wmain(int argc, wchar_t** argv){
    std::wstring first = L"D://workbuddyfile//2026-10-17-20-06-56//build//_test//small//small.zip.001";
    std::wstring name = FileNameOf(first);
    std::wstring dir = GetParentDir(first);
    wprintf(L"name=[%ls] dir=[%ls]\n", name.c_str(), dir.c_str());
    size_t dot = name.find_last_of(L'.');
    std::wstring base = name.substr(0, dot);
    wprintf(L"base=[%ls]\n", base.c_str());
    for (int i=1;i<=13;i++){
        std::wstring p = JoinPath(dir, base + L"." + FormatVolumeIndex(i));
        wprintf(L"%d [%ls] exists=%d\n", i, p.c_str(), (int)FileExists(p));
    }
    return 0;
}
