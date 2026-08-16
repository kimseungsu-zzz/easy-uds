#pragma once

#include <string_view>

namespace easy_uds {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version = "1.0.0";
inline constexpr unsigned int protocol_version = 2;

} // namespace easy_uds
