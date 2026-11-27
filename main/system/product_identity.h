#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

// PID and SID resolve independently. An absent field falls through without
// changing the other field or rewriting a device's saved identity.
inline std::string ResolveIdentityField(std::string_view stored,
                                       std::string_view configured,
                                       std::string_view manufacturer_default,
                                       std::string_view global_default) {
    for (const auto value : {stored, configured, manufacturer_default, global_default}) {
        if (!value.empty()) return std::string(value);
    }
    return {};
}
