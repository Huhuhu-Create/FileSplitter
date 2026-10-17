@echo off
rem ==========================================================================
rem  build.bat - one click build for FileSplitter.exe
rem  Default toolchain: MinGW64 bundled with Dev-Cpp (GCC 10.3).
rem  If you use another MinGW-w64 / MSYS2, change MINGW_BIN below to the
rem  folder that contains your g++.exe.
rem
rem  NOTE: this script puts the toolchain folder on PATH before calling g++.
rem  Calling g++ through a quoted absolute Windows path can make cc1plus fail
rem  to locate its temp dir (silent "ld returned 1"), so keep it on PATH.
rem ==========================================================================
setlocal
set "MINGW_BIN=E:\Dev-Cpp\MinGW64\bin"
if not exist "%MINGW_BIN%\g++.exe" set "MINGW_BIN="
if defined MINGW_BIN set "PATH=%MINGW_BIN%;%PATH%"

set "ROOT=%~dp0"
set "OUT=%ROOT%dist"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%build" mkdir "%ROOT%build"

set "CXXFLAGS=-std=c++17 -municode -mwindows -O2 -s -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-unknown-pragmas"
set "LDFLAGS=-static -static-libgcc -static-libstdc++"
set "LIBS=-lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -lgdiplus -lole32 -loleaut32 -luuid -ldwmapi -lbcrypt -luxtheme"

echo.
echo [1/3] compile and link ...
g++ %CXXFLAGS% -I"%ROOT%src" "%ROOT%src\main.cpp" "%ROOT%src\zip_volume.cpp" "%ROOT%src\merge_script.cpp" "%ROOT%src\util.cpp" "%ROOT%src\crc32.cpp" -o "%OUT%\FileSplitter.exe" %LDFLAGS% %LIBS% > "%ROOT%build\build.log" 2>&1
if errorlevel 1 (
    echo [FAILED] see build\build.log
    type "%ROOT%build\build.log"
    exit /b 1
)

echo [2/3] build selftest ...
g++ -std=c++17 -municode -O2 -s -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -I"%ROOT%src" "%ROOT%tests\selftest.cpp" "%ROOT%src\zip_volume.cpp" "%ROOT%src\merge_script.cpp" "%ROOT%src\util.cpp" "%ROOT%src\crc32.cpp" -o "%ROOT%build\selftest.exe" %LDFLAGS% -lbcrypt >> "%ROOT%build\build.log" 2>&1
if errorlevel 1 (
    echo [FAILED] see build\build.log
    type "%ROOT%build\build.log"
    exit /b 1
)

echo [3/3] run selftest ...
"%ROOT%build\selftest.exe"
if errorlevel 1 (
    echo.
    echo [WARN] selftest reported failures ^(exe was still built^)
)

echo.
echo [OK] %OUT%\FileSplitter.exe
for %%F in ("%OUT%\FileSplitter.exe") do echo      size: %%~zF bytes
echo.
exit /b 0
