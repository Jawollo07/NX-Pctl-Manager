// trf — fmt::format over a runtime (i18n) format string. fmt 12 only accepts
// compile-time format strings by default; translations need fmt::runtime.
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once

#include <fmt/format.h>
#include <string>
#include <utility>

template <typename... Args>
std::string trf(const std::string& format, Args&&... args)
{
    return fmt::format(fmt::runtime(format), std::forward<Args>(args)...);
}
