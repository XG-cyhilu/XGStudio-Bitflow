// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <xgdl/xgdl.h>

class Bridge {
public:
    using Sink = std::function<void(const std::string&)>;
    using ThemeQuery = std::function<bool()>;

    Bridge(Sink sink, ThemeQuery theme);
    ~Bridge();

    void Start();
    std::string Handle(const std::string& payload);
    void Tick();
    void Shutdown();

private:
    Sink sink_;
    ThemeQuery theme_;
    std::unique_ptr<xgdl::Manager> mgr_;
    std::vector<uint32_t> order_;
    bool started_ = false;
    bool paused_ = false;
};
