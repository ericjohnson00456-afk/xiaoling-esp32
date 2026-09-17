#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <vector>
#include <array>
#include <cstdint>

#include <sys/socket.h>
#include <netinet/in.h>

class AudioDebugger {
public:
    AudioDebugger();
    ~AudioDebugger();

    void Feed(const std::vector<int16_t>& data);

private:
    bool EnsureSocket();
    void SendPendingAudio();

    int udp_sockfd_ = -1;
    bool server_addr_valid_ = false;
    struct sockaddr_in udp_server_addr_;
    static constexpr size_t kSamplesPerPacket = 640;
    std::array<int16_t, kSamplesPerPacket> pending_audio_{};
    size_t pending_samples_ = 0;
};

#endif
