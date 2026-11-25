// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#include "terminal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <xgcfg/xgcfg.h>
#include <xgcrypto/xgcrypto.h>
#include <xgfs/xgfs.h>
#include <xglog/xglog.h>
#include <xgnet/xgnet.h>
#include <xgproc/xgproc.h>
#include <xgsecure/xgsecure.h>
#include <xgzip/xgzip.h>

#include "settings.h"
#include "text.h"

namespace term {
namespace {

std::wstring g_cwd;

const wchar_t* kHelp =
    L"命令                    说明\n"
    L"help                    显示本帮助\n"
    L"clear                   清屏（等价 Ctrl+L）\n"
    L"pwd / cd <dir>          当前目录 / 切换目录\n"
    L"ls [path] [-r]          列目录（xgfs::ListDir）\n"
    L"stat <path>             路径信息 + 所在卷空间（xgfs）\n"
    L"df [path]               卷可用 / 总容量（xgfs::DiskSpace）\n"
    L"mkdir <path>            递归建目录（xgfs::EnsureDir）\n"
    L"rm <path> [-r]          删除文件 / 递归删除目录\n"
    L"cp <src> <dst>          复制（xgfs::Copy）\n"
    L"mv <src> <dst>          移动（xgfs::Move）\n"
    L"hash <alg> <path>       md5/sha1/sha256/sha384/sha512（xgcrypto）\n"
    L"zip list <zip>          列出压缩包条目（xgzip::List）\n"
    L"zip extract <zip> [dir] 解压（xgzip::Extract）\n"
    L"net probe <url>         取一次并报告状态 / 耗时 / 吞吐（xgnet）\n"
    L"net get <url> [file]    下载到文件，缺省落在当前目录\n"
    L"run <command line>      启动子进程并收集输出（xgproc）\n"
    L"cfg dump                打印 settings.json\n"
    L"cfg get <dot.path>      读一个设置键（xgcfg）\n"
    L"cfg set <dot.path> <v>  写一个设置键并落盘（数值 / true / false 自动识别）\n"
    L"secure rand <n>         生成 n 字节十六进制随机串（xgsecure）\n"
    L"secure hmac <k> <text>  HMAC-SHA256\n"
    L"secure b64 <text>       Base64 编码 / b64d 解码\n"
    L"log level <lvl>         运行时改日志级别（trace..off）\n"
    L"log dir                 日志目录\n"
    L"echo <text>             原样输出";

std::string FmtSize(uint64_t bytes) {
    double v = double(bytes);
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    char buf[64];
    if (i == 0) {
        sprintf_s(buf, "%.0f %s", v, units[i]);
    } else {
        sprintf_s(buf, v >= 100 ? "%.0f %s" : (v >= 10 ? "%.1f %s" : "%.2f %s"), v, units[i]);
    }
    return buf;
}

std::string FmtTime(int64_t ms) {
    if (ms <= 0) return "-";
    time_t t = time_t(ms / 1000);
    tm local{};
    localtime_s(&local, &t);
    char buf[32];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
              local.tm_hour, local.tm_min);
    return buf;
}

std::vector<std::string> Split(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    bool has = false;
    for (char c : text) {
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else {
                cur.push_back(c);
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            has = true;
        } else if (c == ' ' || c == '\t') {
            if (has) {
                out.push_back(cur);
                cur.clear();
                has = false;
            }
        } else {
            cur.push_back(c);
            has = true;
        }
    }
    if (has) out.push_back(cur);
    return out;
}

bool Absolute(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

std::wstring Cwd() {
    if (g_cwd.empty()) {
        g_cwd = U8ToW(DefaultDownloadDir());
        if (g_cwd.empty()) g_cwd = AppDataRoot();
    }
    return g_cwd;
}

std::wstring Resolve(const std::string& raw) {
    if (raw.empty()) return Cwd();
    std::wstring path = U8ToW(raw);
    if (!Absolute(path)) path = xgfs::Join(Cwd(), path);
    return xgfs::Normalize(path);
}

std::string NameOf(const std::wstring& path) {
    return WToU8(xgfs::FileName(path));
}

void Out(std::vector<Line>& lines, const std::string& text) { lines.push_back({ "out", text }); }
void Err(std::vector<Line>& lines, const std::string& text) { lines.push_back({ "err", text }); }
void Info(std::vector<Line>& lines, const std::string& text) { lines.push_back({ "info", text }); }

bool ParseAlg(const std::string& name, xgcrypto::Alg& alg) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "md5") { alg = xgcrypto::Alg::Md5; return true; }
    if (lower == "sha1") { alg = xgcrypto::Alg::Sha1; return true; }
    if (lower == "sha256") { alg = xgcrypto::Alg::Sha256; return true; }
    if (lower == "sha384") { alg = xgcrypto::Alg::Sha384; return true; }
    if (lower == "sha512") { alg = xgcrypto::Alg::Sha512; return true; }
    return false;
}

void CmdLs(std::vector<Line>& lines, const std::vector<std::string>& args) {
    std::wstring dir = Resolve(args.size() > 1 && args[1] != "-r" ? args[1] : "");
    bool recursive = false;
    for (const auto& a : args) {
        if (a == "-r" || a == "/s") recursive = true;
    }
    if (!xgfs::IsDir(dir)) {
        Err(lines, "不是目录：" + WToU8(dir));
        return;
    }
    xgfs::ListOptions opt;
    opt.recursive = recursive;
    std::vector<xgfs::DirEntry> entries;
    std::string err;
    if (!xgfs::ListDir(dir, entries, opt, &err)) {
        Err(lines, err);
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const xgfs::DirEntry& a, const xgfs::DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return a.name < b.name;
    });
    Info(lines, WToU8(dir) + "  " + std::to_string(entries.size()) + " 项");
    uint64_t total = 0;
    for (const auto& e : entries) {
        if (!e.isDir) total += e.size;
        std::string row = e.isDir ? "d  " : "   ";
        row += WToU8(e.name);
        if (!e.isDir) {
            row += std::string("  ") + FmtSize(e.size);
        }
        row += "  " + FmtTime(e.mtimeMs);
        Out(lines, row);
    }
    uint64_t freeBytes = 0, totalBytes = 0;
    if (xgfs::DiskSpace(dir, freeBytes, totalBytes)) {
        Info(lines, "合计 " + FmtSize(total) + " · 卷可用 " + FmtSize(freeBytes) + " / " + FmtSize(totalBytes));
    }
}

} // namespace

std::vector<Line> Run(const std::string& rawLine) {
    static std::mutex runMutex;
    std::lock_guard<std::mutex> guard(runMutex);
    std::vector<Line> lines;
    std::string text = rawLine;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return lines;

    std::vector<std::string> args = Split(text);
    if (args.empty()) return lines;
    std::string cmd = args[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (cmd == "help" || cmd == "?") {
        // kHelp 是宽字符，逐行拆开输出
        std::wstring wide = kHelp;
        size_t start = 0;
        while (start <= wide.size()) {
            size_t next = wide.find(L'\n', start);
            std::wstring seg = wide.substr(start, next == std::wstring::npos ? std::wstring::npos : next - start);
            Out(lines, WToU8(seg));
            if (next == std::wstring::npos) break;
            start = next + 1;
        }
        return lines;
    }

    if (cmd == "clear") {
        lines.push_back({ "clear", "" });
        return lines;
    }

    if (cmd == "echo") {
        Out(lines, text.size() > 5 ? text.substr(5) : "");
        return lines;
    }

    if (cmd == "pwd") {
        Out(lines, WToU8(Cwd()));
        return lines;
    }

    if (cmd == "cd") {
        std::wstring dir = Resolve(args.size() > 1 ? args[1] : "");
        if (!xgfs::IsDir(dir)) {
            Err(lines, "目录不存在：" + WToU8(dir));
            return lines;
        }
        g_cwd = dir;
        Out(lines, WToU8(dir));
        return lines;
    }

    if (cmd == "ls" || cmd == "dir") {
        CmdLs(lines, args);
        return lines;
    }

    if (cmd == "stat") {
        if (args.size() < 2) {
            Err(lines, "用法：stat <path>");
            return lines;
        }
        std::wstring path = Resolve(args[1]);
        if (!xgfs::Exists(path)) {
            Err(lines, "不存在：" + WToU8(path));
            return lines;
        }
        uint64_t size = 0;
        xgfs::Size(path, size);
        int64_t mtime = 0;
        xgfs::ModifiedTime(path, mtime);
        Info(lines, WToU8(path));
        Out(lines, std::string(xgfs::IsDir(path) ? "目录" : "文件") +
                      (xgfs::IsFile(path) ? "  " + FmtSize(size) : std::string()));
        Out(lines, "修改时间 " + FmtTime(mtime));
        uint64_t freeBytes = 0, totalBytes = 0;
        std::string err;
        if (xgfs::DiskSpace(xgfs::IsDir(path) ? path : xgfs::Parent(path), freeBytes, totalBytes, &err)) {
            Out(lines, "卷可用 " + FmtSize(freeBytes) + " / " + FmtSize(totalBytes));
        }
        return lines;
    }

    if (cmd == "df") {
        std::wstring path = Resolve(args.size() > 1 ? args[1] : "");
        uint64_t freeBytes = 0, totalBytes = 0;
        std::string err;
        if (!xgfs::DiskSpace(path, freeBytes, totalBytes, &err)) {
            Err(lines, err.empty() ? "取磁盘信息失败" : err);
            return lines;
        }
        Out(lines, "可用 " + FmtSize(freeBytes) + "  总计 " + FmtSize(totalBytes) +
                      "  占用 " + FmtSize(totalBytes - freeBytes));
        return lines;
    }

    if (cmd == "mkdir") {
        if (args.size() < 2) {
            Err(lines, "用法：mkdir <path>");
            return lines;
        }
        std::wstring path = Resolve(args[1]);
        std::string err;
        if (!xgfs::EnsureDir(path, &err)) {
            Err(lines, err.empty() ? "创建失败" : err);
            return lines;
        }
        Out(lines, "已创建 " + WToU8(path));
        return lines;
    }

    if (cmd == "rm" || cmd == "del") {
        if (args.size() < 2) {
            Err(lines, "用法：rm <path> [-r]");
            return lines;
        }
        bool recursive = false;
        for (const auto& a : args) {
            if (a == "-r" || a == "/s") recursive = true;
        }
        std::wstring path = Resolve(args[1]);
        std::string err;
        if (!xgfs::RemoveWithRetry(path, recursive, 5, 120, &err)) {
            Err(lines, err.empty() ? "删除失败" : err);
            return lines;
        }
        Out(lines, "已删除 " + WToU8(path));
        return lines;
    }

    if (cmd == "cp" || cmd == "copy" || cmd == "mv" || cmd == "move") {
        if (args.size() < 3) {
            Err(lines, "用法：" + cmd + " <src> <dst>");
            return lines;
        }
        std::wstring src = Resolve(args[1]);
        std::wstring dst = Resolve(args[2]);
        if (!xgfs::Exists(src)) {
            Err(lines, "源不存在：" + WToU8(src));
            return lines;
        }
        std::string err;
        bool ok = (cmd[0] == 'm')
                      ? xgfs::Move(src, dst, true, &err)
                      : xgfs::Copy(src, dst, true, &err);
        if (!ok) {
            Err(lines, err.empty() ? "操作失败" : err);
            return lines;
        }
        Out(lines, (cmd[0] == 'm' ? "已移动 " : "已复制 ") + WToU8(src) + " -> " + WToU8(dst));
        return lines;
    }

    if (cmd == "hash") {
        if (args.size() < 3) {
            Err(lines, "用法：hash <md5|sha1|sha256|sha384|sha512> <path>");
            return lines;
        }
        xgcrypto::Alg alg;
        if (!ParseAlg(args[1], alg)) {
            Err(lines, "未知算法：" + args[1]);
            return lines;
        }
        std::wstring path = Resolve(args[2]);
        if (!xgfs::IsFile(path)) {
            Err(lines, "不是文件：" + WToU8(path));
            return lines;
        }
        uint64_t size = 0;
        xgfs::Size(path, size);
        Info(lines, "计算 " + std::string(xgcrypto::AlgName(alg)) + " · " + WToU8(xgfs::FileName(path)) +
                      " · " + FmtSize(size));
        std::string hex = xgcrypto::HashFileHex(alg, path);
        if (hex.empty()) {
            Err(lines, "计算失败");
            return lines;
        }
        Out(lines, hex);
        return lines;
    }

    if (cmd == "zip") {
        if (args.size() < 3) {
            Err(lines, "用法：zip list <zip> | zip extract <zip> [dir]");
            return lines;
        }
        std::string sub = args[1];
        std::wstring zip = Resolve(args[2]);
        if (!xgfs::IsFile(zip)) {
            Err(lines, "压缩包不存在：" + WToU8(zip));
            return lines;
        }
        if (sub == "list") {
            std::vector<xgzip::Entry> entries;
            std::string err;
            if (!xgzip::List(zip, entries, err)) {
                Err(lines, err.empty() ? "无法读取压缩包" : err);
                return lines;
            }
            uint64_t total = 0, compressed = 0;
            for (const auto& e : entries) {
                total += e.size;
                compressed += e.compressedSize;
                std::string row = e.isDir ? "d  " : "   ";
                row += WToU8(e.name);
                if (!e.isDir) row += "  " + FmtSize(e.size);
                if (e.encrypted) row += "  [加密]";
                Out(lines, row);
            }
            Info(lines, std::to_string(entries.size()) + " 项 · 原始 " + FmtSize(total) +
                          " · 压缩后 " + FmtSize(compressed));
            return lines;
        }
        if (sub == "extract") {
            std::wstring dest = args.size() > 3 ? Resolve(args[3])
                                                : xgfs::Join(xgfs::Parent(zip), xgfs::Stem(zip));
            xgzip::ExtractOptions opt;
            opt.overwrite = true;
            xgzip::Result res;
            if (!xgzip::Extract(zip, dest, opt, res)) {
                Err(lines, res.error.empty() ? "解压失败" : res.error);
                return lines;
            }
            Info(lines, "解压到 " + WToU8(dest));
            Out(lines, std::to_string(res.files) + " 个文件 / " + std::to_string(res.dirs) + " 个目录 · " +
                          FmtSize(res.bytes) + " · " + std::to_string(res.elapsedMs) + " ms" +
                          (res.skipped ? " · 跳过 " + std::to_string(res.skipped) : std::string()));
            return lines;
        }
        Err(lines, "未知子命令：" + sub);
        return lines;
    }

    if (cmd == "net") {
        if (args.size() < 3) {
            Err(lines, "用法：net probe <url> | net get <url> [file]");
            return lines;
        }
        std::string sub = args[1];
        std::string url = args[2];
        xgnet::Client client;
        xgnet::Request req;
        req.url = url;
        req.timeoutMs = 15000;
        req.userAgent = "BitFlow/1.0";
        xgnet::Response res;
        bool transport = client.Perform(req, res);
        double speed = res.totalSec > 0 ? double(res.received) / res.totalSec : 0.0;
        std::string summary = "HTTP " + std::to_string(res.status) + " · " +
                              std::to_string(int64_t(res.totalSec * 1000)) + " ms · 收到 " +
                              FmtSize(res.received) + " · " + FmtSize(uint64_t(speed)) + "/s";
        if (!transport || (!res.IsSuccess() && res.received == 0)) {
            Err(lines, (res.error.empty() ? "请求失败" : res.error) + "  " + summary);
            return lines;
        }
        Info(lines, summary);
        if (sub == "get" || sub == "save") {
            if (!res.IsSuccess()) {
                Err(lines, "下载未成功，未写文件");
                return lines;
            }
            std::wstring dst;
            if (args.size() > 3) {
                dst = Resolve(args[3]);
            } else {
                std::string name = url;
                size_t cut = name.find_first_of("?#");
                if (cut != std::string::npos) name = name.substr(0, cut);
                size_t slash = name.find_last_of('/');
                name = slash == std::string::npos ? "download.bin" : name.substr(slash + 1);
                if (name.empty()) name = "download.bin";
                dst = xgfs::Join(Cwd(), U8ToW(name));
            }
            std::string err;
            if (!xgfs::WriteFileAtomic(dst, res.body.data(), res.body.size(), &err)) {
                Err(lines, err.empty() ? "写文件失败" : err);
                return lines;
            }
            Out(lines, "已保存 " + WToU8(dst) + "  " + FmtSize(res.body.size()));
        }
        return lines;
    }

    if (cmd == "run" || cmd == "exec") {
        if (text.size() < 4) {
            Err(lines, "用法：run <command line>");
            return lines;
        }
        std::wstring command = U8ToW(text.substr(cmd.size()));
        while (!command.empty() && command.front() == L' ') command.erase(command.begin());
        xgproc::Result res = xgproc::Run(command, 30000, Cwd());
        if (!res.started) {
            Err(lines, "启动失败：" + WToU8(command));
            return lines;
        }
        Info(lines, "退出码 " + std::to_string(res.exitCode) + (res.timedOut ? "（超时）" : "") +
                      " · " + std::to_string(res.out.size()) + " 行输出");
        int shown = 0;
        for (const auto& l : res.out) {
            if (shown++ >= 80) {
                Out(lines, "… 输出已截断");
                break;
            }
            std::string body = l.text;
            if (body.size() > 400) body = body.substr(0, 400) + "…";
            Out(lines, (l.stderrLine ? "! " : "  ") + body);
        }
        return lines;
    }

    if (cmd == "cfg") {
        if (args.size() < 2) {
            Err(lines, "用法：cfg dump | cfg get <path> | cfg set <path> <value>");
            return lines;
        }
        xgcfg::Config cfg;
        std::string err;
        bool loaded = cfg.Load(SettingsPath(), &err);
        if (!loaded && args[1] != "set") {
            Err(lines, err.empty() ? "读取 settings.json 失败" : err);
            return lines;
        }
        if (args[1] == "dump") {
            std::string pretty = xgcfg::Dump(cfg.Root(), true);
            size_t start = 0;
            while (start < pretty.size()) {
                size_t next = pretty.find('\n', start);
                Out(lines, pretty.substr(start, next == std::string::npos ? std::string::npos : next - start));
                if (next == std::string::npos) break;
                start = next + 1;
            }
            return lines;
        }
        if (args[1] == "get" && args.size() >= 3) {
            const xgcfg::Value* node = cfg.Root().FindPath(args[2]);
            if (!node) {
                Err(lines, "没有键 " + args[2]);
                return lines;
            }
            Out(lines, xgcfg::Dump(*node, false));
            return lines;
        }
        if (args[1] == "set" && args.size() >= 4) {
            std::string value = args[3];
            xgcfg::Value v;
            if (value == "true" || value == "false") {
                v = xgcfg::Value(value == "true");
            } else {
                bool numeric = !value.empty() && value.find_first_not_of("0123456789+-.") == std::string::npos;
                if (numeric && value.find('.') == std::string::npos) {
                    v = xgcfg::Value(int64_t(std::atoll(value.c_str())));
                } else if (numeric) {
                    v = xgcfg::Value(std::atof(value.c_str()));
                } else {
                    v = xgcfg::Value(value);
                }
            }
            cfg.Set(args[2], v);
            if (!cfg.SaveAs(SettingsPath(), true, &err)) {
                Err(lines, err.empty() ? "写入失败" : err);
                return lines;
            }
            const xgcfg::Value* after = cfg.Root().FindPath(args[2]);
            Out(lines, "已写入 " + args[2] + " = " + (after ? xgcfg::Dump(*after, false) : std::string("?")) +
                          "（重启生效）");
            return lines;
        }
        Err(lines, "参数不足");
        return lines;
    }

    if (cmd == "secure") {
        if (args.size() < 2) {
            Err(lines, "用法：secure rand <n> | secure hmac <key> <text> | secure b64 <text> | secure b64d <text>");
            return lines;
        }
        if (args[1] == "rand") {
            size_t n = args.size() > 2 ? size_t(std::atoi(args[2].c_str())) : 16;
            if (n == 0 || n > 512) {
                Err(lines, "n 需在 1..512");
                return lines;
            }
            Out(lines, xgsecure::RandomHex(n));
            return lines;
        }
        if (args[1] == "hmac" && args.size() >= 4) {
            Out(lines, xgsecure::HmacSha256Hex(args[2], args[3]));
            return lines;
        }
        if (args[1] == "b64" && args.size() >= 3) {
            Out(lines, xgsecure::Base64Encode(args[2].data(), args[2].size()));
            return lines;
        }
        if (args[1] == "b64d" && args.size() >= 3) {
            std::vector<uint8_t> bytes;
            if (!xgsecure::Base64Decode(args[2], bytes)) {
                Err(lines, "不是合法 Base64");
                return lines;
            }
            Out(lines, std::string(bytes.begin(), bytes.end()));
            return lines;
        }
        Err(lines, "未知子命令：" + args[1]);
        return lines;
    }

    if (cmd == "log") {
        if (args.size() >= 2 && args[1] == "dir") {
            Out(lines, WToU8(LogDir()));
            return lines;
        }
        if (args.size() >= 3 && args[1] == "level") {
            xglog::Level level = ParseLogLevelName(args[2]);
            xglog::SetLevel(level);
            Info(lines, "日志级别 -> " + WToU8(xglog::LevelName(level)));
            return lines;
        }
        Err(lines, "用法：log level <trace|debug|info|warn|error|off> | log dir");
        return lines;
    }

    Err(lines, "未知命令：" + cmd + "（输入 help 查看用法）");
    return lines;
}

} // namespace term
