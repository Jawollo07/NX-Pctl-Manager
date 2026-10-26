// TOTP parent-code helper (RFC 6238, SHA-1, 6 digits, 60-second period).
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace totp
{
constexpr std::size_t SECRET_BYTES = 20;

bool secret_file_exists();
bool load_or_create_secret(std::array<uint8_t, SECRET_BYTES>& secret);
bool load_secret(std::array<uint8_t, SECRET_BYTES>& secret);
std::string base32(const uint8_t* data, std::size_t size);
bool verify_code(const std::string& code);
} // namespace totp
