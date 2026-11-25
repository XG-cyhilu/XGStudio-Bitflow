# BitFlow

XG STUDIO 出品的高速多功能下载器。前端是 WebView2 承载的原生 VanillaJS 页面，后端是 C++17 宿主进程，下载引擎直接集成 XGDL，并复用 XGStudio-cpp-Core 的 cfg / crypto / fs / log / net / proc / secure / zip 模块提供设置、校验、日志、解压与测速能力。

A high-speed, multi-functional downloader developed by XG STUDIO: a VanillaJS front end rendered by WebView2, a C++17 host process, the XGDL download engine wired in directly, plus XGStudio-cpp-Core modules (cfg / crypto / fs / log / net / proc / secure / zip) for settings, hashing, logging, extraction and probing.

- 语言 / Language：C++17（后端）、VanillaJS（前端，无构建链）
- 渲染 / Rendering：Microsoft Edge WebView2（WebView2LoaderStatic，无额外 DLL）
- 设计 / Design：Microsoft Fluent 2 Design System 语义化令牌（浅色 / 深色跟随系统）
- 引擎 / Engine：XGDL（libcurl CURLM 多路复用 + 分块 Range + 镜像回退 + 摘要校验）
- 许可 / License：GPL-3.0-or-later

## 功能 / Features

- 多线程下载、分块 Range 续传、镜像回退、入队摘要预校验
- 任务完成后自动计算 SHA-256（`xgcrypto`），行内显示 hash chip，点开即完整值
- `.zip` 完成后可自动解压到同名目录，也可手动触发（`xgzip`）
- 设置页写入 `settings.json`（`xgcfg` 点路径 + 原子写）：下载目录、线程 / 连接数 / 限速、日志级别、自动解压、测速地址
- 运行日志页实时滚动（`xglog` 环形缓冲 + `UiPoll`），同时落盘到 `logs/bitflow.log`
- 网络测速（`xgnet`）：状态码、耗时、吞吐；超时未读完时按已收字节给出估算
- 打开所在文件夹 / 定位文件走 `xgproc`，写入与目录创建走 `xgfs`，安装 ID 由 `xgsecure` 生成

## 目录结构 / Layout

```
CMakeLists.txt               宿主工程
cmake/embed_web_assets.cmake 把 web/ 打成 RCDATA 资源，生成资源表
src/main.cpp                 Win32 窗口 + WebView2 宿主 + 主题/DPI + 资源服务
src/bridge.h/.cpp            JS ↔ C++ 命令桥（JSON 协议，XGDL 轮询转推送）
src/settings.h/.cpp          设置结构、加载/保存、应用数据目录
src/json.h                   极简 JSON 解析 / 序列化
src/text.h                   UTF-8 ↔ UTF-16 转换
web/                         前端源码（index.html / css / js，Fluent 2 令牌）
vendor/xgdl/                 XGDL 源码 + 静态 libcurl（Apache-2.0）
vendor/core/                 XGStudio-cpp-Core 子集（cfg crypto fs log net proc secure zip，Apache-2.0）
vendor/webview2/             WebView2 SDK 头文件与 x64 静态 loader（MIT）
```

## 构建 / Build

依赖：Visual Studio 2022（MSVC x64）、CMake ≥ 3.20、WebView2 Runtime（Win10/11 一般已预装）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

产物：`build/Release/Bitflow.exe`（约 1.0 MB，前端资源与全部静态库都已在里面，单文件即可运行）。

Requirements: Visual Studio 2022 (MSVC x64), CMake ≥ 3.20, and the WebView2 Runtime (preinstalled on most Windows 10/11). The output is a single self-contained `Bitflow.exe`; front-end assets are embedded as RCDATA.

## 架构 / Architecture

1. 宿主窗口用 `WS_OVERLAPPEDWINDOW`，标题栏跟随系统深浅色（DWM 属性 20/34/35/36），窗口背景与页面 `--color-app-surface` 同色，避免加载闪白。
2. WebView2 的页面源是自造的 `https://bitflow.local/`；`WebResourceRequested` 处理器从 EXE 的 RCDATA 资源里取文件并内联 MIME、`Cache-Control: no-store`，因此没有外部资源目录，也没有本地 HTTP 服务。
3. 前端 `chrome.webview.postMessage` 发 `{rid, cmd, ...}`，宿主 `Bridge::Handle` 返回 `{type:"reply", rid, ok, data}`；`SetTimer` 每 200 ms 调一次 `Bridge::Tick()`，一次性推送 `Snapshot()` 聚合进度、每个任务的 `QueryTask()` 明细、`PollEvent()` 抽干的事件、`xglog::UiPoll` 的日志行和后台任务（校验 / 解压 / 测速）的异步结果。XGDL 本身没有回调，这个轮询循环就是它和 UI 之间唯一的桥。
4. 外部链接一律由 `NavigationStarting` 取消并交给系统浏览器，页面自身导航只允许 `bitflow.local`。
5. 主题切换由 `WM_SETTINGCHANGE`（`ImmersiveColorSet`）触发，宿主重设 DWM 属性并把 `{type:"theme"}` 推给前端。
6. 应用数据在 `%LOCALAPPDATA%\XGStudio\BitFlow\`：`settings.json`（原子写）、`logs\bitflow.log`（8 MB 轮转、保留 8 份）、`WebView2\`（浏览器用户数据）。

## 桥协议 / Bridge protocol

下行（页面 → 宿主）：

| cmd | 参数 | 说明 |
|---|---|---|
| `app.info` | — | 返回名称、版本、引擎名、当前主题、下载目录 |
| `settings.get` | — | 返回全部设置项 + `installId` / `logDir` / `settingsPath` |
| `settings.set` | `downloadDir` `smallThreads` `largeThreads` `maxConnections` `speedLimitKb` `logLevel` `autoExtract` `probeUrl` | 保存并立即应用日志级别，返回 `saved` / `restartRequired` |
| `dl.add` | `url` `path` `size` `algo` `digest` `mirrors[]` `headers[[k,v]]` `priority` `mtime` | 入队（会预检目录与磁盘空间），返回 `task` id |
| `dl.cancel` | `task` | 取消单任务 |
| `dl.query` | `task` | 单任务明细 |
| `dl.cancelAll` | — | 清空队列 |
| `engine.pause` / `engine.resume` | — | 暂停派发 / 恢复 |
| `task.verify` | `task` | 重新计算 SHA-256 |
| `task.extract` | `task` | 解压该任务落盘的 zip |
| `net.probe` | `url`（可省略，用设置里的地址） | 测速，结果异步回推 |
| `shell.reveal` | `path` | 资源管理器定位文件 |
| `shell.openUrl` | `url` | 系统浏览器打开 |
| `log.revealDir` | — | 打开日志目录 |

上行（宿主 → 页面）：`reply`（命令应答）、`tick`（`progress` + `items` + `events` + `logs` + `async`）、`theme`。
`async` 元素类型：`verify`（`task`/`algo`/`hex`/`ok`）、`extract`（`task`/`ok`/`dest`/`files`/`bytes`/`ms`/`error`）、`probe`（`ok`/`partial`/`status`/`ms`/`speed`/`received`/`error`）。

## 已知边界 / Known limits

- 单任务暂停没有实现：XGDL 只有全局 `Pause()`，单任务只能 `Cancel()`。
- 任务列表只存在于内存，退出即清空；持久化清单与续传恢复尚未接入。
- 引擎参数（线程 / 连接数 / 限速）改完需要重启进程才生效，设置页会明确提示。
- 启动时需要 WebView2 Runtime，缺少时弹提示而不是自动安装。

## 许可 / License

GPL-3.0-or-later，见 `LICENSE`。`vendor/xgdl` 与 `vendor/core` 为 Apache-2.0（各自目录下有 `LICENSE.txt`），`vendor/webview2` 为 Microsoft 的 MIT 许可。

Application code is GPL-3.0-or-later. Vendored libraries keep their own licenses: Apache-2.0 for `vendor/xgdl` and `vendor/core`, MIT for `vendor/webview2`.
