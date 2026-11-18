#pragma once
#include <string>
class EspSimplePlayerBackend;
bool MiniappRegisterHls(EspSimplePlayerBackend &backend);
std::string MiniappAudioUri(const std::string &url);
