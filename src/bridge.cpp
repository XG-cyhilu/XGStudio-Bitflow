// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#include "bridge.h"

#include <shlobj.h>
#include <shellapi.h>

#include "json.h"
#include "text.h"

namespace {

std::wstring DownloadsDirectory() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) {
        std::wstring result = path;
        CoTaskMemFree(path);
        return result;
    }
    return std::wstring();
}

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

} // namespace

Bridge::Bridge(Sink sink, ThemeQuery theme)
    : sink_(std::move(sink)), theme_(std::move(theme)), mgr_(std::make_unique<xgdl::Manager>()) {}

Bridge::~Bridge() {
    Shutdown();
}

void Bridge::Start() {
    if (started_) return;
    mgr_->Start();
    started_ = true;
}

void Bridge::Shutdown() {
    if (!mgr_) return;
    mgr_->Shutdown();
    started_ = false;
}

void Bridge::Tick() {
    if (!started_ || !sink_) return;

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
        if (mgr_->QueryTask(id, ti)) WriteTask(w, ti);
    }
    w.EndArray();

    xgdl::Event ev;
    bool any = false;
    while (mgr_->PollEvent(ev)) {
        if (!any) {
            w.Key("events").BeginArray();
            any = true;
        }
        w.BeginObject();
        w.Key("task").U64(ev.id);
        w.Key("kind").Str(ev.kind == xgdl::EventKind::Completed ? "completed" : "failed");
        w.Key("path").Str(WToU8(ev.path));
        w.Key("status").Num(ev.httpStatus);
        w.Key("message").Str(WToU8(ev.message));
        w.EndObject();
    }
    if (any) w.EndArray();

    w.EndObject();
    sink_(w.Str());
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

    if (cmd == "app.info") {
        jsn::Writer d;
        d.BeginObject();
        d.Key("name").Str("BitFlow");
        d.Key("version").Str("1.0.0");
        d.Key("engine").Str("XGDL");
        d.Key("theme").Str(theme_ && theme_() ? "dark" : "light");
        d.Key("downloads").Str(WToU8(DownloadsDirectory()));
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

        uint32_t id = mgr_->Add(U8ToW(url), U8ToW(path), opt);
        if (id == 0) {
            return MakeReply(rid, false, "", "入队失败：路径已在队列中，或参数非法");
        }
        order_.push_back(id);

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
        jsn::Writer d;
        d.BeginObject();
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "engine.pause") {
        mgr_->Pause();
        paused_ = true;
        jsn::Writer d;
        d.BeginObject().Key("paused").Bool(true).EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "engine.resume") {
        mgr_->Resume();
        paused_ = false;
        jsn::Writer d;
        d.BeginObject().Key("paused").Bool(false).EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "shell.reveal") {
        std::string path = root.Get("path") ? root.Get("path")->AsStr() : std::string();
        if (path.empty()) return MakeReply(rid, false, "", "缺少 path 字段");
        std::wstring arg = L"/select,\"" + U8ToW(path) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        jsn::Writer d;
        d.BeginObject();
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    if (cmd == "shell.openUrl") {
        std::string url = root.Get("url") ? root.Get("url")->AsStr() : std::string();
        if (url.empty()) return MakeReply(rid, false, "", "缺少 url 字段");
        ShellExecuteW(nullptr, L"open", U8ToW(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        jsn::Writer d;
        d.BeginObject();
        d.EndObject();
        return MakeReply(rid, true, d.Str(), "");
    }

    return MakeReply(rid, false, "", "未知命令: " + cmd);
}
