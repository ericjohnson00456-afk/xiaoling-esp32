#pragma once
#include "miniapp.h"
#include <functional>
#include <string>

namespace Miniapp {
using Completion = std::function<void(bool, std::string)>;
void InstallUrl(miniapp_package_t package, std::string url, Completion completion);
bool ValidArguments(const std::string &name, const cJSON *args, std::string &error);
bool AllowedTool(const std::string &name);
std::string JsonText(cJSON *value);
bool VerifySource(const miniapp_package_t &package, const char *source, std::string &error);
} // namespace Miniapp
