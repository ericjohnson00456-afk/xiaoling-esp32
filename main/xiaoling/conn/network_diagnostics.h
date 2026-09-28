#ifndef XIAOLING_NETWORK_DIAGNOSTICS_H
#define XIAOLING_NETWORK_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace NetworkDiagnostics {

void Initialize();
void LogSnapshot(const char* reason);
void StartFailureProbe(const std::string& url, const char* reason,
                       uint32_t sequence, size_t payload_size);

}  // namespace NetworkDiagnostics

#endif  // XIAOLING_NETWORK_DIAGNOSTICS_H
