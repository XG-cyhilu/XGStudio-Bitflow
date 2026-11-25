# BitFlow

XG STUDIO 出品的高速多功能下载器。前端是 WebView2 承载的原生 VanillaJS 页面，后端是 C++17 宿主进程，下载引擎直接集成 XGDL。

A high-speed, multi-functional downloader developed by XG STUDIO: a VanillaJS front end rendered by WebView2, a C++17 host process, and the XGDL download engine wired in directly.

- 语言 / Language：C++17（后端）、VanillaJS（前端，无构建链）
- 渲染 / Rendering：Microsoft Edge WebView2（WebView2LoaderStatic，无额外 DLL）
- 设计 / Design：Microsoft Fluent 2 Design System 语义化令牌（浅色 / 深色跟随系统）
- 引擎 / Engine：XGDL（libcurl CURLM 多路复用 + 分块 Range + 镜像回退 + 摘要校验）
- 许可 / License：GPL-3.0-or-later

## 目录结构 / Layout

```
CMakeLists.txt              宿主工程
cmake/embed_web_assets.cmake 把 web/ 打成 RCDATA 资源，生成资源表
src/main.cpp                 Win32 窗口 + WebView2 宿主 + 主题/DPI + 资源服务
src/bridge.h/.cpp            JS ↔ C++ 命令桥（JSON 协议，XGDL 轮询转推送）
src/json.h                   极简 JSON 解析 / 序列化
src/text.h                   UTF-8 ↔ UTF-16 转换
web/                         前端源码（HTML / CSS / JS，Fluent 2 令牌）
vendor/xgdl/                 XGDL 源码 + 静态 libcurl（Apache-2.0）
vendor/webview2/             WebView2 SDK 头文件与 x64 静态 loader（MIT）
```

## 构建 / Build

依赖：Visual Studio 2022（MSVC x64）、CMake ≥ 3.20、WebView2 Runtime（Win10/11 一般已预装）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

产物：`build/Release/Bitflow.exe`（约 650 KB，前端资源已内嵌，单文件即可运行）。

Requirements: Visual Studio 2022 (MSVC x64), CMake ≥ 3.20, and the WebView2 Runtime (preinstalled on most Windows 10/11). The output is a single self-contained `Bitflow.exe`; all front-end assets are embedded as RCDATA.

## 架构 / Architecture

1. 宿主窗口用 `WS_OVERLAPPEDWINDOW`，标题栏跟随系统深浅色（DWM 属性 20/34/35/36），窗口背景与页面 `--color-app-surface` 同色，避免加载闪白。
2. WebView2 的页面源是自造的 `https://bitflow.local/`；`WebResourceRequested` 处理器从 EXE 的 RCDATA 资源里取文件并内联 MIME、`Cache-Control: no-store`，因此没有外部资源目录，也没有本地 HTTP 服务。
3. 前端 `chrome.webview.postMessage` 发 `{rid, cmd, ...}`，宿主 `Bridge::Handle` 返回 `{type:"reply", rid, ok, data}`；`SetTimer` 每 200 ms 调一次 `Bridge::Tick()`，一次性推送 `Snapshot()` 聚合进度、每个任务的 `QueryTask()` 明细以及 `PollEvent()` 抽干的事件。XGDL 本身没有回调，这个轮询循环就是它和 UI 之间唯一的桥。
4. 外部链接一律由 `NavigationStarting` 取消并交给系统浏览器，页面自身导航只允许 `bitflow.local`。
5. 主题切换由 `WM_SETTINGCHANGE`（`ImmersiveColorSet`）触发，宿主重设 DWM 属性并把 `{type:"theme"}` 推给前端。

## 桥协议 / Bridge protocol

下行（页面 → 宿主）：

| cmd | 参数 | 说明 |
|---|---|---|
| `app.info` | — | 返回名称、版本、引擎名、当前主题、下载目录 |
| `dl.add` | `url` `path` `size` `algo` `digest` `mirrors[]` `headers[[k,v]]` `priority` `mtime` | 入队，返回 `task` id |
| `dl.cancel` | `task` | 取消单任务 |
| `dl.query` | `task` | 单任务明细 |
| `dl.cancelAll` | — | 清空队列 |
| `engine.pause` / `engine.resume` | — | 暂停派发 / 恢复 |
| `shell.reveal` | `path` | 资源管理器定位文件 |
| `shell.openUrl` | `url` | 系统浏览器打开 |

上行（宿主 → 页面）：`reply`（命令应答）、`tick`（进度 + 任务列表 + 事件）、`theme`。

## 已知边界 / Known limits

- 单任务暂停没有实现：XGDL 只有全局 `Pause()`，单任务只能 `Cancel()`。
- 任务列表只存在于内存，退出即清空；持久化清单与续传恢复尚未接入。
- 启动时需要 WebView2 Runtime，缺少时弹提示而不是自动安装。

## 许可 / License

GPL-3.0-or-later，见 `LICENSE`。`vendor/xgdl` 为 Apache-2.0（`vendor/xgdl/LICENSE.txt`），`vendor/webview2` 为 Microsoft 的 MIT 许可。
