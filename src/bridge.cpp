// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#include "bridge.h"

#include <windows.h>
#include <shellapi.h>

#include <mutex>
#include <thread>
#include <utility>

#include <xgcrypto/xgcrypto.h>
#include <xgfs/xgfs.h>
#include <xglog/xglog.h>
#include <xgnet/xgnet.h>
#include <xgproc/xgproc.h>
#include <xgsecure/xgsecure.h>
#include <xgzip/xgzip.h>

#include "json.h"
#include "settings.h"
#include "terminal.h"
#include "text.h"

struct AsyncBus {
    void Push(std::string json) {
        std::lock_guard<std::mutex> lock(mu);
        items.push_back(std::move(json));
    }

    std::vector<std::string> Drain() {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<std::string> out = std::move(items);
        items.clear();
        return out;
    }

    std::mutex mu;
    std::vector<std::string> items;
};

namespace {

const char* StateName(xgdl::TaskState s) {
    switch (s) {
    case xgdl::TaskState::Queued: return "queued";
    case xgdl::TaskState::Downloading: return "downloading";
    case xgdl::TaskState::Verifying: return "verifying";
    case xgdl::TaskState::Completed: return "completed";
    case xgdl::TaskState::Failed: return "failed";
    case xgdl::TaskState::Cancelled: return "cancelled";
    }
    return "queued";
}

const char* LevelTag8(xglog::Level lv) {
    switch (lv) {
    case xglog::Level::Trace: return "trace";
    case xglog::Level::Debug: return "debug";
    case xglog::Level::Info: return "info";
    case xglog::Level::Warn: return "warn";
    case xglog::Level::Error: return "error";
    case xglog::Level::Fatal: return "fatal";
    default: return "info";
    }
}

xglog::Level ParseLevel(const std::string& s) {
    if (s == "trace") return xglog::Level::Trace;
    if (s == "debug") return xglog::Level::Debug;
    if (s == "info") return xglog::Level::Info;
    if (s == "warn") return xglog::Level::Warn;
    if (s == "error") return xglog::Level::Error;
    if (s == "fatal") return xglog::Level::Fatal;
    return xglog::Level::Off;
}

xgdl::Priority ParsePriority(const std::string& s) {
    if (s == "high") return xgdl::Priority::High;
    if (s == "low") return xgdl::Priority::Low;
    return xgdl::Priority::Normal;
}

xgdl::HashAlg ParseAlg(const std::string& s) {
    if (s == "md5") return xgdl::HashAlg::Md5;
    if (s == "sha1") return xgdl::HashAlg::Sha1;
    if (s == "sha256") return xgdl::HashAlg::Sha256;
    if (s == "sha512") return xgdl::HashAlg::Sha512;
    return xgdl::HashAlg::None;
}

std::string MakeReply(const std::string& rid, bool ok, const std::string& data, const std::string& error) {
    jsn::Writer w;
    w.BeginObject();
    w.Key("type").Str("reply");
    w.Key("rid").Str(rid);
    w.Key("ok").Bool(ok);
    if (ok) {
        w.Key("data").Raw(data.empty() ? "null" : data);
    } else {
        w.Key("error").Str(error);
    }
    w.EndObject();
    return w.Str();
}

void WriteTask(jsn::Writer& w, const xgdl::TaskInfo& ti) {
    w.BeginObject();
    w.Key("id").U64(ti.id);
    w.Key("url").Str(WToU8(ti.url));
    w.Key("path").Str(WToU8(ti.path));
    w.Key("size").U64(ti.sizeTotal);
    w.Key("done").U64(ti.sizeDone);
    w.Key("state").Str(StateName(ti.state));
    w.Key("attempt").Num(ti.attempt);
    w.Key("chunked").Bool(ti.large);
    w.EndObject();
}

void WriteSettings(jsn::Writer& d, const Settings& s) {
    d.Key("downloadDir").Str(s.downloadDir);
    d.Key("smallThreads").Num(s.smallThreads);
    d.Key("largeThreads").Num(s.largeThreads);
    d.Key("maxConnections").Num(s.maxConnections);
    d.Key("speedLimitKb").Num(s.speedLimitKb);
    d.Key("logLevel").Str(s.logLevel);
    d.Key("autoExtract").Bool(s.autoExtract);
    d.Key("probeUrl").Str(s.probeUrl);
    d.Key("installId").Str(s.installId);
    d.Key("logDir").Str(WToU8(LogDir()));
}

} // namespace

Bridge::Bridge(Sink sink, ThemeQuery theme, SettingsStore* settings)
    : sink_(std::move(sink)),
      theme_(std::move(theme)),
      settings_(settings),
      bus_(std::make_shared<AsyncBus>()) {
    xgdl::Config cfg;
    if (settings_) {
        const Settings& s = settings_->Get();
        cfg.smallThreads = s.smallThreads > 0 ? s.smallThreads : 16;
        cfg.largeThreads = s.largeThreads > 0 ? s.largeThreads : 4;
        cfg.maxConnections = s.maxConnections > 0 ? s.maxConnections : 48;
        cfg.maxSpeedBytesPerSec = s.speedLimitKb > 0 ? uint64_t(s.speedLimitKb) * 1024ull : 0ull;
    }
    mgr_ = std::make_unique<xgdl::Manager>(cfg);
}

Bridge::~Bridge() {
    Shutdown();
}

void Bridge::Start() {
    if (started_) return;
    mgr_->Start();
    started_ = true;
    XGLOG_INFO("engine started");
}

void Bridge::Shutdown() {
    if (!mgr_) return;
    if (started_) XGLOG_INFO("engine shutting down");
    mgr_->Shutdown();
    started_ = false;
}

std::wstring Bridge::PathOf(uint32_t id) const {
    auto it = paths_.find(id);
    if (it != paths_.end()) return it->second;
    xgdl::TaskInfo ti;
    if (mgr_->QueryTask(id, ti)) return ti.path;
    return std::wstring();
}

void Bridge::Tick() {
    if (!started_ || !sink_) return;

    static int flushTick = 0;
    if (++flushTick >= 25) {
        flushTick = 0;
        xglog::Flush();
    }

    jsn::Writer w;
    w.BeginObject();
    w.Key("type").Str("tick");

    xgdl::Progress p = mgr_->Snapshot();
    w.Key("progress").BeginObject();
    w.Key("total").U64(p.bytesTotal);
    w.Key("done").U64(p.bytesDone);
    w.Key("speed").Num(p.bytesPerSec);
    w.Key("active").Num(p.active);
    w.Key("queued").Num(p.queued);
    w.Key("completed").Num(p.completed);
    w.Key("failed").Num(p.failed);
    w.Key("paused").Bool(paused_);
    w.EndObject();

    w.Key("items").BeginArray();
    for (uint32_t id : order_) {
        xgdl::TaskInfo ti;
        if (mgr_->QueryTask(id, ti)) {
            paths_[id] = ti.path;
            WriteTask(w, ti);
        }
    }
    w.EndArray();

    xgdl::Event ev;
    bool anyEvent = false;
    while (mgr_->PollEvent(ev)) {
        if (!anyEvent) {
            w.Key("events").BeginArray();
            anyEvent = true;
        }
        w.BeginObject();
        w.Key("task").U64(ev.id);
        w.Key("kind").Str(ev.kind == xgdl::EventKind::Completed ? "completed" : "failed");
        w.Key("path").Str(WToU8(ev.path));
        w.Key("status").Num(ev.httpStatus);
        w.Key("message").Str(WToU8(ev.message));
        w.EndObject();

        if (ev.kind == xgdl::EventKind::Completed) {
            XGLOG_INFO("task %u completed", ev.id);
            std::wstring path = ev.path.empty() ? PathOf(ev.id) : ev.path;
            StartVerify(ev.id, path);
            if (settings_ && settings_->Get().autoExtract) {
                std::wstring ext = xgfs::Extension(path);
                for (auto& ch : ext) ch = towlower(ch);
                if (ext == L".zip") StartExtract(ev.id, path);
            }
        } else {
            XGLOG_ERROR("task %u failed: %s", ev.id, WToU8(ev.message).c_str());
        }
    }
    if (anyEvent) w.EndArray();

    xglog::Record rec;
    int logCount = 0;
    w.Key("logs").BeginArray();
    while (clientSeen_ && logCount < 60 && xglog::UiPoll(rec)) {
        w.BeginObject();
        w.Key("level").Str(LevelTag8(rec.level));
        w.Key("time").I64(rec.timeMs);
        w.Key("text").Str(rec.text);
        w.EndObject();
        ++logCount;
    }
    w.EndArray();

    std::vector<std::string> async = bus_->Drain();
    w.Key("async").BeginArray();
    for (const auto& item : async) w.Raw(item);
    w.EndArray();

    w.EndObject();
    sink_(w.Str());
}

void Bridge::StartVerify(uint32_t id, const std::wstring& path) {
    if (path.empty()) return;
    std::shared_ptr<AsyncBus> bus = bus_;
    std::wstring target = path;
    std::thread([bus, id, target] {
        std::string hex = xgcrypto::HashFileHex(xgcrypto::Alg::Sha256, target);
        jsn::Writer w;
        w.BeginObject();
        w.Key("type").Str("verify");
        w.Key("task").U64(id);
        w.Key("algo").Str("sha256");
        w.Key("hex").Str(hex);
        w.Key("ok").Bool(!hex.empty());
        w.EndObject();
        bus->Push(w.Str());
    }).detach();
}

void Bridge::StartExtract(uint32_t id, const std::wstring& path) {
    if (path.empty()) return;
    std::wstring dest = xgfs::Join(xgfs::Parent(path), xgfs::Stem(path));
    std::string mkErr;
    if (!xgfs::IsDir(dest) && !xgfs::EnsureDir(dest, &mkErr)) {
        jsn::Writer w;
        w.BeginObject();
        w.Key("type").Str("extract");
        w.Key("task").U64(id);
        w.Key("ok").Bool(false);
        w.Key("dest").Str(WToU8(dest));
        w.Key("error").Str(mkErr);
        w.EndObject();
        bus_->Push(w.Str());
        return;
    }

    std::shared_ptr<AsyncBus> bus = bus_;
    std::wstring target = path;
    std::thread([bus, id, target, dest] {
        xgzip::ExtractOptions opt;
        opt.overwrite = true;
        xgzip::Result res;
        bool ok = xgzip::Extract(target, dest, opt, res, nullptr);
        jsn::Writer w;
        w.BeginObject();
        w.Key("type").Str("extract");
        w.Key("task").U64(id);
        w.Key("ok").Bool(ok);
        w.Key("dest").Str(WToU8(dest));
        w.Key("files").Num(res.files);
        w.Key("bytes").U64(res.bytes);
        w.Key("ms").I64(res.elapsedMs);
        w.Key("error").Str(res.error);
        w.EndObject();
        bus->Push(w.Str());
        if (ok) {
            XGLOG_INFO("extracted %d files from task %u", res.files, id);
        } else {
            XGLOG_ERROR("extract task %u failed: %s", id, res.error.c_str());
        }
    }).detach();
}

void Bridge::StartProbe(const std::string& url) {
    if (url.empty()) return;
    std::shared_ptr<AsyncBus> bus = bus_;
    std::thread([bus, url] {
        xgnet::Client client;
        xgnet::Request req;
        req.url = url;
        req.timeoutMs = 15000;
        req.userAgent = "BitFlow/1.0";
        xgnet::Response res;
        bool transported = client.Perform(req, res);
        bool ok = transported && res.IsSuccess();
        bool partial = !ok && res.received > 0;
        double speed = res.totalSec > 0 ? double(res.received) / res.totalSec : 0.0;
        jsn::Writer w;
        w.BeginObject();
        w.Key("type").Str("probe");
        w.Key("ok").Bool(ok || partial);
        w.Key("partial").Bool(partial);
        w.Key("status").Num(double(res.status));
        w.Key("ms").Num(res.totalSec * 1000.0);
        w.Key("speed").Num(speed);
        w.Key("received").U64(res.received);
        w.Key("error").Str(res.error);
        w.EndObject();
        bus->Push(w.Str());
        XGLOG_INFO("probe %s -> status %ld in %.0f ms", url.c_str(), res.status, res.totalSec * 1000.0);
    }).detach();
}

std::string Bridge::Handle(const std::string& payload) {
    jsn::Node root;
    jsn::Parser parser(payload);
    if (!parser.Parse(root) || !root.IsObj()) {
        return MakeReply("", false, "", "参数不是合法的 JSON 对象");
    }

    std::string rid = root.Get("rid") ? root.Get("rid")->AsStr() : std::string();
    const jsn::Node* cmdNode = root.Get("cmd");
    if (!cmdNode || !cmdNode->IsStr()) {
        return MakeReply(rid, false, "", "缺少 cmd 字段");
    }
    std::string cmd = cmdNode->str;
    clientSeen_ = true;
    Settings fallback;

    if (cmd == "app.info") {
        jsn::Writer d;
        d.BeginObject();
        d.Key("name").Str("BitFlow");
        d.Key("version").Str("1.0.0");
        d.Key("engine").Str("XGDL");
        d.Key("theme").Str(theme_ && theme_() ? "dark" : "light");
        d.Key("downloads").Str(settings_ ? settings_->Get().downloadDir : std::string());
        d.Key("logDir").Str(WToU8(LogDir()));
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "settings.get") {
        jsn::Writer d;
        d.BeginObject();
        WriteSettings(d, settings_ ? settings_->Get() : fallback);
        d.Key("settingsPath").Str(WToU8(SettingsPath()));
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "settings.set") {
        if (!settings_) return MakeReply(rid, false, "", "设置不可用");
        Settings s = settings_->Get();
        const jsn::Node* n = nullptr;
        bool engineChanged = false;

        if ((n = root.Get("downloadDir")) && n->IsStr()) s.downloadDir = n->str;
        if ((n = root.Get("smallThreads")) && n->IsNum()) {
            int v = int(n->AsU64());
            engineChanged |= (v != s.smallThreads);
            s.smallThreads = v;
        }
        if ((n = root.Get("largeThreads")) && n->IsNum()) {
            int v = int(n->AsU64());
            engineChanged |= (v != s.largeThreads);
            s.largeThreads = v;
        }
        if ((n = root.Get("maxConnections")) && n->IsNum()) {
            int v = int(n->AsU64());
            engineChanged |= (v != s.maxConnections);
            s.maxConnections = v;
        }
        if ((n = root.Get("speedLimitKb")) && n->IsNum()) {
            int v = int(n->AsU64());
            engineChanged |= (v != s.speedLimitKb);
            s.speedLimitKb = v;
        }
        if ((n = root.Get("logLevel")) && n->IsStr()) s.logLevel = n->str;
        if ((n = root.Get("autoExtract")) && n->type == jsn::Node::Bool) s.autoExtract = n->b;
        if ((n = root.Get("probeUrl")) && n->IsStr()) s.probeUrl = n->str;

        settings_->Set(s);
        bool saved = settings_->Save();
        xglog::SetLevel(ParseLevel(s.logLevel));

        jsn::Writer d;
        d.BeginObject();
        d.Key("saved").Bool(saved);
        d.Key("restartRequired").Bool(engineChanged);
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "dl.add") {
        std::string url = root.Get("url") ? root.Get("url")->AsStr() : std::string();
        std::string path = root.Get("path") ? root.Get("path")->AsStr() : std::string();
        if (url.empty() || path.empty()) {
            return MakeReply(rid, false, "", "URL 和保存路径不能为空");
        }

        xgdl::TaskOptions opt;
        const jsn::Node* n = nullptr;
        if ((n = root.Get("size")) != nullptr) opt.expectBytes = n->AsU64();
        if ((n = root.Get("priority")) != nullptr) opt.priority = ParsePriority(n->AsStr());
        if ((n = root.Get("mtime")) != nullptr) opt.mtimeMs = n->AsI64();
        if ((n = root.Get("algo")) != nullptr) opt.hash.alg = ParseAlg(n->AsStr());
        if ((n = root.Get("digest")) != nullptr) opt.hash.hex = U8ToW(n->AsStr());
        if ((n = root.Get("mirrors")) != nullptr && n->IsArr()) {
            for (const auto& m : n->arr) {
                if (m.IsStr() && !m.str.empty()) opt.mirrors.push_back(U8ToW(m.str));
            }
        }
        if ((n = root.Get("headers")) != nullptr && n->IsArr()) {
            for (const auto& h : n->arr) {
                if (h.IsArr() && h.arr.size() == 2 && h.arr[0].IsStr() && h.arr[1].IsStr()) {
                    opt.headers.emplace_back(U8ToW(h.arr[0].str), U8ToW(h.arr[1].str));
                }
            }
        }

        std::wstring wpath = U8ToW(path);
        std::wstring parent = xgfs::Parent(wpath);
        if (!parent.empty()) {
            std::string dirErr;
            if (!xgfs::IsDir(parent) && !xgfs::EnsureDir(parent, &dirErr)) {
                return MakeReply(rid, false, "", "无法创建保存目录：" + dirErr);
            }
        }
        if (opt.expectBytes > 0) {
            uint64_t freeBytes = 0;
            uint64_t totalBytes = 0;
            std::string spaceErr;
            if (xgfs::DiskSpace(parent.empty() ? wpath : parent, freeBytes, totalBytes, &spaceErr) &&
                freeBytes < opt.expectBytes) {
                return MakeReply(rid, false, "", "磁盘空间不足：" + std::to_string(freeBytes) + " 字节可用");
            }
        }

        uint32_t id = mgr_->Add(U8ToW(url), wpath, opt);
        if (id == 0) {
            return MakeReply(rid, false, "", "入队失败：路径已在队列中，或参数非法");
        }
        order_.push_back(id);
        paths_[id] = wpath;
        XGLOG_INFO("task %u queued: %s", id, path.c_str());

        jsn::Writer d;
        d.BeginObject();
        d.Key("task").U64(id);
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "dl.cancel" || cmd == "dl.query") {
        const jsn::Node* t = root.Get("task");
        if (!t || !t->IsNum()) return MakeReply(rid, false, "", "缺少 task 字段");
        uint32_t id = uint32_t(t->AsU64());
        if (cmd == "dl.cancel") {
            mgr_->Cancel(id);
            XGLOG_WARN("task %u cancelled", id);
            jsn::Writer d;
            d.BeginObject();
            d.EndObject();
            return MakeReply(rid, true, d.Str(), "");
        }
        xgdl::TaskInfo ti;
        if (!mgr_->QueryTask(id, ti)) return MakeReply(rid, false, "", "任务不存在");
        jsn::Writer d;
        WriteTask(d, ti);
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "dl.cancelAll") {
        mgr_->CancelAll();
        XGLOG_WARN("all tasks cancelled");
        jsn::Writer d;
        d.BeginObject();
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "engine.pause" || cmd == "engine.resume") {
        bool pause = cmd == "engine.pause";
        if (pause) {
            mgr_->Pause();
        } else {
            mgr_->Resume();
        }
        paused_ = pause;
        XGLOG_INFO(pause ? "engine paused" : "engine resumed");
        jsn::Writer d;
        d.BeginObject().Key("paused").Bool(paused_).EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "task.verify") {
        const jsn::Node* t = root.Get("task");
        if (!t || !t->IsNum()) return MakeReply(rid, false, "", "缺少 task 字段");
        uint32_t id = uint32_t(t->AsU64());
        std::wstring path = PathOf(id);
        if (path.empty()) return MakeReply(rid, false, "", "任务不存在");
        if (!xgfs::IsFile(path)) return MakeReply(rid, false, "", "文件尚未落盘");
        StartVerify(id, path);
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "task.extract") {
        const jsn::Node* t = root.Get("task");
        if (!t || !t->IsNum()) return MakeReply(rid, false, "", "缺少 task 字段");
        uint32_t id = uint32_t(t->AsU64());
        std::wstring path = PathOf(id);
        if (path.empty() || !xgfs::IsFile(path)) return MakeReply(rid, false, "", "文件尚未落盘");
        StartExtract(id, path);
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "term.exec") {
        std::string line = root.Get("line") ? root.Get("line")->AsStr() : std::string();
        if (line.empty()) return MakeReply(rid, false, "", "缺少命令");
        std::shared_ptr<AsyncBus> bus = bus_;
        std::thread([bus, line] {
            std::vector<term::Line> lines = term::Run(line);
            jsn::Writer w;
            w.BeginObject();
            w.Key("type").Str("term");
            w.Key("lines").BeginArray();
            for (const term::Line& l : lines) {
                w.BeginObject();
                w.Key("kind").Str(l.kind);
                w.Key("text").Str(l.text);
                w.EndObject();
            }
            w.EndArray();
            w.EndObject();
            bus->Push(w.Str());
        }).detach();
        jsn::Writer d;
        d.BeginObject();
        d.Key("queued").Bool(true);
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "net.probe") {
        std::string url = root.Get("url") ? root.Get("url")->AsStr() : std::string();
        if (url.empty() && settings_) url = settings_->Get().probeUrl;
        if (url.empty()) return MakeReply(rid, false, "", "缺少测速地址");
        StartProbe(url);
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "shell.reveal") {
        std::string path = root.Get("path") ? root.Get("path")->AsStr() : std::string();
        if (path.empty()) return MakeReply(rid, false, "", "缺少 path 字段");
        std::wstring target = U8ToW(path);
        xgproc::Result res = xgproc::Run(L"explorer.exe /select,\"" + target + L"\"", 5000);
        if (!res.started) return MakeReply(rid, false, "", "无法启动资源管理器");
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "shell.openUrl") {
        std::string url = root.Get("url") ? root.Get("url")->AsStr() : std::string();
        if (url.empty()) return MakeReply(rid, false, "", "缺少 url 字段");
        ShellExecuteW(nullptr, L"open", U8ToW(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "log.revealDir") {
        std::wstring dir = LogDir();
        if (!xgfs::IsDir(dir)) xgfs::EnsureDir(dir);
        xgproc::Run(L"explorer.exe \"" + dir + L"\"", 5000);
        jsn::Writer d;
        d.BeginObject().EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    return MakeReply(rid, false, "", "未知命令: " + cmd);
}
