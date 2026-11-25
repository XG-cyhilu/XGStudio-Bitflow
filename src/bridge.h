// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <xgdl/xgdl.h>

class SettingsStore;
struct AsyncBus;
struct PendingAdds;

class Bridge {
public:
    using Sink = std::function<void(const std::string&)>;
    using ThemeQuery = std::function<bool()>;

    Bridge(Sink sink, ThemeQuery theme, SettingsStore* settings);
    ~Bridge();

    void Start();
    std::string Handle(const std::string& payload);
    void Tick();
    void Shutdown();

private:
    void StartVerify(uint32_t id, const std::wstring& path);
    void StartExtract(uint32_t id, const std::wstring& path);
    void StartProbe(const std::string& url);
    std::wstring PathOf(uint32_t id) const;
    uint32_t EnqueueAdd(const std::wstring& url, const std::wstring& path, xgdl::TaskOptions& opt);

    Sink sink_;
    ThemeQuery theme_;
    SettingsStore* settings_;
    std::unique_ptr<xgdl::Manager> mgr_;
    std::vector<uint32_t> order_;
    std::unordered_map<uint32_t, std::wstring> paths_;
    std::shared_ptr<AsyncBus> bus_;
    std::shared_ptr<PendingAdds> pendingAdds_;
    bool started_ = false;
    bool paused_ = false;
    bool clientSeen_ = false;
};
