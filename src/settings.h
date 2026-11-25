// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#pragma once

#include <string>

#include <xgcfg/xgcfg.h>
#include <xglog/xglog.h>

struct Settings {
    std::string downloadDir;
    int smallThreads = 16;
    int largeThreads = 4;
    int maxConnections = 48;
    int speedLimitKb = 0;
    std::string logLevel = "debug";
    bool autoExtract = true;
    std::string probeUrl = "https://mirrors.aliyun.com/ubuntu/ls-lR.gz";
    std::string installId;
};

std::wstring AppDataRoot();
std::wstring SettingsPath();
std::wstring LogDir();
std::string DefaultDownloadDir();
xglog::Level ParseLogLevelName(const std::string& name);

class SettingsStore {
public:
    SettingsStore();

    bool Load();
    bool Save();

    const Settings& Get() const { return s_; }
    void Set(const Settings& s) { s_ = s; dirty_ = true; }
    const std::wstring& Path() const { return path_; }

private:
    xgcfg::Config cfg_;
    Settings s_;
    std::wstring path_;
    bool dirty_ = false;
};
