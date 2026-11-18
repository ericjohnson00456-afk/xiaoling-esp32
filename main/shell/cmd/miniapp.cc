#include "miniapp.h"
#include "miniapp/miniapp.h"
#include <esp_console.h>

#ifdef CONFIG_MINIAPP
#include "miniapp/miniapp_install.h"
#include "miniapp/miniapp_platform.h"
#include "miniapp/miniapp_store.h"
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {
constexpr size_t kChunkBytes = 120;
constexpr int64_t kTimeoutUs = 30 * 1000 * 1000;
struct Session {
    char token[17];
    miniapp_package_t package;
    char *source;
    size_t received;
    int64_t touched;
    bool uploading;
};
Session *session = nullptr;
std::mutex session_mutex;
esp_timer_handle_t timeout_timer = nullptr;

void ClearSession() {
    if (!session) return;
    if (session->uploading) miniapp_install_end();
    free(session->source);
    free(session);
    session = nullptr;
    esp_timer_stop(timeout_timer);
}

bool StartSession(const char *token, bool uploading) {
    if (!timeout_timer) {
        esp_timer_create_args_t args{};
        args.name = "miniapp_usb";
        args.callback = [](void *) {
            std::unique_lock<std::mutex> lock(session_mutex, std::try_to_lock);
            if (lock.owns_lock() && session &&
                esp_timer_get_time() - session->touched >= kTimeoutUs) ClearSession();
        };
        if (esp_timer_create(&args, &timeout_timer) != ESP_OK) return false;
    }
    session = static_cast<Session *>(miniapp_calloc(1, sizeof(Session)));
    if (!session) return false;
    memcpy(session->token, token, sizeof(session->token));
    session->uploading = uploading;
    session->touched = esp_timer_get_time();
    if (esp_timer_start_periodic(timeout_timer, 1000000) != ESP_OK) {
        free(session);
        session = nullptr;
        return false;
    }
    return true;
}

bool Number(const char *text, size_t &value) {
    const size_t length = strlen(text);
    if (!length || length > 6 || strspn(text, "0123456789") != length) return false;
    value = strtoul(text, nullptr, 10);
    return value <= MINIAPP_SOURCE_MAX;
}

std::string Error(const std::string &reason) {
    std::string result = "error " + reason.substr(0, 160);
    std::replace(result.begin(), result.end(), '\n', ' ');
    std::replace(result.begin(), result.end(), '\r', ' ');
    return result;
}

std::string Process(int argc, char **argv) {
    std::lock_guard<std::mutex> lock(session_mutex);
    const char *token = argv[1], *verb = argv[2];
    if (strcmp(verb, "hello") == 0 && argc == 3)
        return "hello 1 " + std::to_string(MINIAPP_SOURCE_MAX) + " " + std::to_string(kChunkBytes);
    if (session && strcmp(token, session->token)) return Error("transfer in progress");
    if (session) session->touched = esp_timer_get_time();
    if (strcmp(verb, "abort") == 0 && argc == 3) {
        ClearSession();
        return "abort";
    }
    if (strcmp(verb, "push") == 0 && argc == 6) {
        if (session) return Error("transfer in progress");
        const size_t id_size = strlen(argv[3]);
        size_t size;
        if (!id_size || id_size > MINIAPP_ID_MAX - 6 ||
            strspn(argv[3], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != id_size ||
            !Number(argv[4], size) || size == 0 || strlen(argv[5]) != 32 ||
            strspn(argv[5], "0123456789abcdef") != 32) return Error("invalid package");
        if (miniapp_platform_alarm()) return Error("alarm is ringing");
        if (miniapp_init() != 0 || !miniapp_install_begin()) return Error("miniapp unavailable or busy");
        if (!StartSession(token, true)) {
            miniapp_install_end();
            return Error("not enough memory for transfer");
        }
        session->source = static_cast<char *>(miniapp_alloc(size + 1));
        if (!session->source) {
            ClearSession();
            return Error("not enough memory for Lua source");
        }
        session->package.size = size;
        snprintf(session->package.id, sizeof(session->package.id), "local:%s", argv[3]);
        snprintf(session->package.name, sizeof(session->package.name), "%s", argv[3]);
        snprintf(session->package.version, sizeof(session->package.version), "%s", argv[5]);
        memcpy(session->package.hash, argv[5], 33);
        return "push 0";
    }
    if (strcmp(verb, "pull") == 0 && argc == 3) {
        if (session) return Error("transfer in progress");
        if (!StartSession(token, false)) return Error("not enough memory for transfer");
        const char *error = miniapp_store_load(&session->package, &session->source);
        if (error) {
            ClearSession();
            return Error(error);
        }
        return "pull " + std::to_string(session->package.size) + " " + session->package.hash;
    }
    if (!session) return Error("no transfer");
    if (strcmp(verb, "put") == 0 && argc == 5 && session->uploading) {
        size_t offset, size = 0;
        uint8_t chunk[kChunkBytes];
        if (!Number(argv[3], offset) || offset != session->received ||
            mbedtls_base64_decode(chunk, sizeof(chunk), &size,
                                  reinterpret_cast<const uint8_t *>(argv[4]), strlen(argv[4])) != 0 ||
            !size || size > session->package.size - session->received)
            return Error("invalid chunk or offset");
        memcpy(session->source + session->received, chunk, size);
        session->received += size;
        return "put " + std::to_string(session->received);
    }
    if (strcmp(verb, "commit") == 0 && argc == 3 && session->uploading) {
        if (session->received != session->package.size) return Error("incomplete source");
        session->source[session->received] = 0;
        std::string error;
        char runtime_error[128]{};
        const bool ok = Miniapp::VerifySource(session->package, session->source, error) &&
                        miniapp_install(&session->package, session->source,
                                         runtime_error, sizeof(runtime_error)) == 0;
        const std::string result = ok ? std::string("commit ") + session->package.hash
                                     : Error(error.empty() ? runtime_error : error);
        ClearSession();
        return result;
    }
    if (strcmp(verb, "get") == 0 && argc == 4 && !session->uploading) {
        size_t offset;
        if (!Number(argv[3], offset) || offset >= session->package.size) return Error("invalid offset");
        uint8_t encoded[kChunkBytes * 4 / 3 + 1];
        size_t encoded_size;
        const size_t size = std::min(kChunkBytes, size_t(session->package.size) - offset);
        if (mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_size,
                                  reinterpret_cast<const uint8_t *>(session->source + offset), size) != 0)
            return Error("failed to encode source");
        return "get " + std::to_string(offset) + " " +
               std::string(reinterpret_cast<char *>(encoded), encoded_size);
    }
    return Error("invalid command");
}

int HandleCommand(int argc, char **argv) {
    if (argc < 3 || strlen(argv[1]) != 16 || strspn(argv[1], "0123456789abcdef") != 16) return 1;
    // Release the session lock before console I/O; a disconnected host must
    // not prevent the timer from releasing its source buffer and install lease.
    const std::string response = Process(argc, argv);
    printf("\n@miniapp %s %s\n", argv[1], response.c_str());
    fflush(stdout);
    return 0;
}
} // namespace
#endif

esp_err_t RegisterMiniappCommand() {
#ifdef CONFIG_MINIAPP
    esp_console_cmd_t command{};
    command.command = "miniapp";
    command.help = "Lua source transfer (use scripts/miniapp.py)";
    command.func = HandleCommand;
    return esp_console_cmd_register(&command);
#else
    return ESP_OK;
#endif
}
