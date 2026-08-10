#pragma once

#include <string_view>

namespace easy_uds {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 4;
inline constexpr int version_patch = 2;
inline constexpr std::string_view version = "0.4.2";
inline constexpr unsigned int protocol_version = 1;

} // namespace easy_uds
