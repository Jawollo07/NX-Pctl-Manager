#include "util/totp.hpp"
#include <cstdio>

namespace totp
{
bool secret_file_exists()
{
    std::FILE* f = std::fopen("/switch/nx_pctl_totp.key", "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}
} // namespace totp
