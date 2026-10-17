# 文件分卷工具 FileSplitter

Windows 桌面工具：把任意大文件拆成多个 zip 分卷，并生成双击即可还原原文件的合并脚本。

- 纯 Win32 API + 标准控件，**零第三方依赖**，单个 exe 约 390 KB
- 深色主题、Per-Monitor V2 DPI、自绘进度条
- 分卷是**真·分卷 ZIP**（Store 不压缩），中间片带 `PK\x07\x08` 分卷结束标记
- 合并后**直接得到原始文件**，自动做体积 + SHA-256 双重校验

---

## 一、运行

直接双击：

```
dist\FileSplitter.exe
```

无需安装任何运行库（MinGW 静态链接）。

## 二、从源码构建

```
双击 build.bat
```

会依次：编译 GUI → 编译自测 → 跑自测。产物在 `dist\FileSplitter.exe`。

如果 g++ 不在默认位置，改 `build.bat` 顶部：

```bat
set "MINGW_BIN=E:\Dev-Cpp\MinGW64\bin"
```

> ⚠️ 不要在 Git Bash 里用引号包裹的绝对路径直接调 `g++.exe`——cc1plus 会找不到临时目录而静默失败（`ld returned 1`，日志空）。`build.bat` 已把工具链目录加入 `PATH`，这样才稳。

## 三、源码结构

| 文件 | 职责 |
|---|---|
| `src/zip_volume.h/.cpp` | 核心：分卷 ZIP 写入、分卷集合校验、错误文案 |
| `src/main.cpp` | GUI（约 1200 行）：布局、自绘控件、工作线程、设置持久化 |
| `src/merge_script.h/.cpp` | 生成 `merge.bat` + `merge.ps1` |
| `src/util.h/.cpp` | SHA-256、CRC32 辅助、路径、磁盘空间、错误映射 |
| `src/crc32.h/.cpp` | IEEE CRC-32 查表实现 |
| `tests/selftest.cpp` | 命令行自测（32 项断言，无 GUI） |
| `build.bat` | 一键构建 + 自测 |

## 四、分卷格式关键点

### 写入

1. 流式写 Local File Header，CRC/尺寸先置 0，置通用标志位 bit3（0x0008）声明后跟数据描述符。
2. 4 MB 循环读源文件 → 累加 CRC32 → 写卷；卷写满时补 4 字节 `PK\x07\x08` 再换下一片。
3. 数据写完补 Data Descriptor（签名 + crc32 + 压缩/原始大小）。
4. 写中央目录 → EOCD；末卷若为奇数长度补 1 字节对齐。
5. 文件名编码：优先 CP_ACP（GBK）、不置 UTF-8 标志，与中文 Windows 上的 WinRAR/7-Zip 一致；ANSI 不可逆时退化 UTF-8 并置 bit11。

### 两个容易踩的坑（已修）

| 坑 | 症状 |
|---|---|
| EOCD 之后又追加 `PK\x05\x06` | Python / 部分工具报 "File is not a zip file" |
| EOCD 的卷号字段写成真实卷号（如 10/5） | .NET `ZipArchive` 抛「不支持拆分或跨区的存档」 |

**最终方案**：EOCD 的 `diskNumber` / `cdDiskNumber` 与中央目录条目的 `diskNumberStart` 一律写 **0**，让末卷在解压工具看来就是一个**普通单卷 zip**；中间片仍保留分卷结束标记，分卷语义不丢失。

另一个关键点：**所有偏移（Local Header 偏移、中央目录偏移、ZIP64 记录偏移）都按「逻辑字节流」计算——只累计真实数据字节，不含分卷标记与补位。** 这样把各分卷顺序拼接、剥掉每片末尾 4 字节标记后，就是一个完全自洽的普通 zip。

### 合并

`merge.ps1`（由 `merge.bat` 调用）：

1. 检查 1..N 分卷是否齐全
2. 逐卷读入，剥掉中间片末尾 4 字节标记，拼成临时 `.zip.tmp`
3. 用 `System.IO.Compression.ZipArchive` 打开，取出唯一条目的流写入目标文件
4. 校验体积
5. 校验 SHA-256
6. 清理临时文件

路径全部用 PowerShell 单引号字面量（`'` → `''`），因此带空格、中文、括号、`&` 都安全。`merge.bat` 正文纯 ASCII，避免 cmd 的 GBK 乱码；中文提示由 UTF-8 with BOM 的 `merge.ps1` 输出。

## 五、自测

```bash
build\selftest.exe
```

覆盖：小文件多卷、整数倍大小、0 字节文件、中文名 + 5MB、源文件不存在、分卷大小为 0、取消后无残留。

当前状态：**32 项全部通过**。

## 六、端到端验证清单

```
1. 拆分 5 MB 测试文件 → 1 MB 分卷 → 得到 6 个分卷
2. 删除原始文件
3. 双击 merge.bat
4. 输出 [1/5]…[5/5] 全部通过，SHA-256 一致
5. 还原文件 md5 == 原文件 md5
```

## 七、异常处理

| 场景 | 表现 |
|---|---|
| 磁盘空间不足 | 预检时拦截，提示剩余/所需空间；写入中触发则回滚删除所有分卷 |
| 文件被占用 | 打开时用 `FILE_SHARE_READ`，立即发现并提示"请先关闭正在使用该文件的程序" |
| 分卷缺失 | 合并前逐个检查，报出具体缺哪一卷 |
| 分卷损坏/截断 | 检查中间片尾标记 + 末片 EOCD，不通过则明确报错 |
| 源文件中途变化 | 读完比对字节数，不一致则报错并回滚 |
| 输出文件已存在 | 询问覆盖 / 另存带时间戳的副本，绝不静默覆盖 |
