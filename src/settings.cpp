// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#include "settings.h"

#include <shlobj.h>

#include <xgfs/xgfs.h>
#include <xgsecure/xgsecure.h>

#include "text.h"

std::wstring AppDataRoot() {
    static std::wstring root;
    if (!root.empty()) return root;

    std::wstring base = L".";
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        base = local;
        CoTaskMemFree(local);
    }
    root = base + L"\\XGStudio\\BitFlow";
    xgfs::EnsureDir(root);
    return root;
}

std::wstring SettingsPath() {
    return AppDataRoot() + L"\\settings.json";
}

std::wstring LogDir() {
    std::wstring dir = AppDataRoot() + L"\\logs";
    xgfs::EnsureDir(dir);
    return dir;
}

std::string DefaultDownloadDir() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) {
        std::string result = WToU8(path);
        CoTaskMemFree(path);
        return result;
    }
    return std::string();
}

xglog::Level ParseLogLevelName(const std::string& name) {
    if (name == "trace") return xglog::Level::Trace;
    if (name == "debug") return xglog::Level::Debug;
    if (name == "info") return xglog::Level::Info;
    if (name == "warn") return xglog::Level::Warn;
    if (name == "error") return xglog::Level::Error;
    if (name == "off") return xglog::Level::Off;
    return xglog::Level::Debug;
}

SettingsStore::SettingsStore() : path_(SettingsPath()) {}

bool SettingsStore::Load() {
    bool firstRun = !xgfs::Exists(path_);

    if (firstRun) {
        s_ = Settings{};
        s_.downloadDir = DefaultDownloadDir();
        s_.installId = xgsecure::RandomHex(16);
        return Save();
    }

    std::string err;
    if (!cfg_.Load(path_, &err)) {
        s_ = Settings{};
        s_.downloadDir = DefaultDownloadDir();
        s_.installId = xgsecure::RandomHex(16);
        return Save();
    }

    s_.downloadDir = cfg_.GetString("download.dir", DefaultDownloadDir());
    s_.smallThreads = int(cfg_.GetInt("engine.smallThreads", 16));
    s_.largeThreads = int(cfg_.GetInt("engine.largeThreads", 4));
    s_.maxConnections = int(cfg_.GetInt("engine.maxConnections", 48));
    s_.speedLimitKb = int(cfg_.GetInt("engine.speedLimitKb", 0));
    s_.logLevel = cfg_.GetString("log.level", "debug");
    s_.autoExtract = cfg_.GetBool("zip.autoExtract", true);
    s_.probeUrl = cfg_.GetString("net.probeUrl", Settings{}.probeUrl);
    s_.installId = cfg_.GetString("app.installId", "");
    if (s_.installId.empty()) {
        s_.installId = xgsecure::RandomHex(16);
        cfg_.Set("app.installId", xgcfg::Value(s_.installId));
        cfg_.Save();
    }
    if (s_.downloadDir.empty()) s_.downloadDir = DefaultDownloadDir();

    dirty_ = false;
    return true;
}

bool SettingsStore::Save() {
    cfg_.Set("download.dir", xgcfg::Value(s_.downloadDir));
    cfg_.Set("engine.smallThreads", xgcfg::Value(int64_t(s_.smallThreads)));
    cfg_.Set("engine.largeThreads", xgcfg::Value(int64_t(s_.largeThreads)));
    cfg_.Set("engine.maxConnections", xgcfg::Value(int64_t(s_.maxConnections)));
    cfg_.Set("engine.speedLimitKb", xgcfg::Value(int64_t(s_.speedLimitKb)));
    cfg_.Set("log.level", xgcfg::Value(s_.logLevel));
    cfg_.Set("zip.autoExtract", xgcfg::Value(s_.autoExtract));
    cfg_.Set("net.probeUrl", xgcfg::Value(s_.probeUrl));
    cfg_.Set("app.installId", xgcfg::Value(s_.installId));

    std::string err;
    if (!cfg_.SaveAs(path_, true, &err)) return false;
    dirty_ = false;
    return true;
}
