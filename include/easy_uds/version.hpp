#pragma once

#include <string_view>

namespace easy_uds {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 6;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version = "0.6.0";
inline constexpr unsigned int protocol_version = 2;

} // namespace easy_uds
