// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 XG Studio

#pragma once

#include <string>
#include <vector>

namespace term {

struct Line {
    std::string kind;   // out / err / info / echo / clear
    std::string text;
};

// 执行一行命令，返回要显示在终端里的行。线程不安全的部分（cwd）由调用方串行化。
std::vector<Line> Run(const std::string& line);

} // namespace term
