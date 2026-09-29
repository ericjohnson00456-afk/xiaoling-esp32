#include "realtime_transport_scope.h"

#include <cerrno>
#include <cstddef>

#include <lwip/sockets.h>

extern "C" int __real_lwip_send(int socket, const void* data, size_t size, int flags);

extern "C" int __wrap_lwip_send(int socket, const void* data, size_t size, int flags) {
    const int timeout_ms = xiaoling_transport_current_send_timeout_ms();
    if (timeout_ms <= 0) {
        return __real_lwip_send(socket, data, size, flags);
    }

    timeval previous_timeout = {};
    socklen_t previous_timeout_size = sizeof(previous_timeout);
    if (lwip_getsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                        &previous_timeout, &previous_timeout_size) != 0) {
        return -1;
    }

    timeval realtime_timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (lwip_setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                        &realtime_timeout, sizeof(realtime_timeout)) != 0) {
        return -1;
    }

    const int result = __real_lwip_send(socket, data, size, flags);
    const int send_errno = errno;
    lwip_setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                    &previous_timeout, previous_timeout_size);
    errno = send_errno;
    return result;
}
