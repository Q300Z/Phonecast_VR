#include "phonecast/platform/network/TcpVideoServer.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/audio/AudioProtocol.h"
#include "phonecast/core/input/IInputProvider.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return close(s); }
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const std::string& name, const std::string& detail = "") {
    if (condition) {
        ++g_pass;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_fail;
        std::cerr << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << '\n';
}

template <typename Predicate>
bool WaitCondition(Predicate pred, int timeoutMs = 2500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

SOCKET ConnectClient(const sockaddr_in& address, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            int noDelay = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
            if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                return s;
            }
            closesocket(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return INVALID_SOCKET;
}

bool SendExact(SOCKET s, const std::vector<uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                           static_cast<int>(bytes.size() - sent),
#ifdef _WIN32
                           0
#else
                           MSG_NOSIGNAL
#endif
        );
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool SendMessage(SOCKET s, const phonecast::core::protocol::Message& m) {
    return SendExact(s, phonecast::core::protocol::Serialize(m));
}

bool RecvExact(SOCKET s, uint8_t* dest, std::size_t size) {
    std::size_t recvd = 0;
    while (recvd < size) {
        const int n = recv(s, reinterpret_cast<char*>(dest + recvd), static_cast<int>(size - recvd), 0);
        if (n <= 0) return false;
        recvd += static_cast<std::size_t>(n);
    }
    return true;
}

bool RecvMessage(SOCKET s, phonecast::core::protocol::Message& m) {
    uint8_t header[phonecast::core::protocol::kHeaderSize]{};
    if (!RecvExact(s, header, sizeof(header))) return false;
    uint32_t payloadSize = 0;
    std::string err;
    if (!phonecast::core::protocol::ParseHeader(header, sizeof(header), m, payloadSize, err)) return false;
    m.payload.resize(payloadSize);
    if (payloadSize > 0 && !RecvExact(s, m.payload.data(), payloadSize)) return false;
    return true;
}

uint16_t GetEphemeralPort(sockaddr_in& outAddress) {
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    outAddress = {};
    outAddress.sin_family = AF_INET;
    outAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<sockaddr*>(&outAddress), sizeof(outAddress));
#ifdef _WIN32
    int len = sizeof(outAddress);
#else
    socklen_t len = sizeof(outAddress);
#endif
    getsockname(probe, reinterpret_cast<sockaddr*>(&outAddress), &len);
    const auto port = ntohs(outAddress.sin_port);
    closesocket(probe);
    return port;
}

// ----------------------------------------------------------------------------
// Test 1: Invariant Verification of Remote Control Reset on Disconnect
// Continuous concurrent observer checks that whenever !server.Connected(),
// RemoteControlStatusKnown and both flags MUST be false (zero race windows).
// ----------------------------------------------------------------------------
void TestRemoteControlResetInvariantUnderChurn() {
    std::cout << "\n=== [Challenger 7 - Test 1] Remote Control Reset Invariant Under Churn ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("112233", port, false);
    std::string error;
    server.Start(error);

    std::atomic<bool> stopObserver{false};
    std::atomic<uint64_t> invariantObservations{0};
    std::atomic<uint64_t> invariantViolations{0};

    // Observer thread checking invariant at maximum frequency
    std::thread observer([&] {
        while (!stopObserver.load(std::memory_order_relaxed)) {
            const bool conn = server.Connected();
            const bool known = server.RemoteControlStatusKnown();
            const auto st = server.RemoteControlStatus();

            // CRITICAL INVARIANT:
            // If the server is NOT connected, remoteControlStatusKnown MUST NOT be true,
            // and appEnabled / accessibilityEnabled MUST NOT be true.
            if (!conn) {
                if (known || st.appEnabled || st.accessibilityEnabled) {
                    invariantViolations.fetch_add(1, std::memory_order_relaxed);
                }
            }
            invariantObservations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    const int kCycles = 50;
    for (int i = 0; i < kCycles; ++i) {
        SOCKET c = ConnectClient(address);
        if (c == INVALID_SOCKET) {
            Check(false, "Connect failed on cycle " + std::to_string(i));
            break;
        }

        // Authenticate
        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'1', '1', '2', '2', '3', '3'};
        SendMessage(c, hello);

        WaitCondition([&] { return server.Connected(); });

        // Set remote control status
        phonecast::core::protocol::RemoteControlStatus status{true, true};
        phonecast::core::protocol::Message rcMsg;
        rcMsg.type = phonecast::core::protocol::MessageType::RemoteControlStatus;
        rcMsg.payload = phonecast::core::protocol::SerializeRemoteControlStatus(status);
        SendMessage(c, rcMsg);

        WaitCondition([&] { return server.RemoteControlStatusKnown() && server.RemoteControlStatus().Ready(); });

        // Disconnect abruptly (SO_LINGER with 0 timeout for immediate RST)
        struct linger sl {};
        sl.l_onoff = 1;
        sl.l_linger = 0;
        setsockopt(c, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
        closesocket(c);

        WaitCondition([&] { return !server.Connected(); });
    }

    stopObserver.store(true);
    observer.join();

    Check(invariantViolations.load() == 0,
          "Zero invariant violations observed during rapid connect/disconnect churn",
          "violations=" + std::to_string(invariantViolations.load()) +
          ", observations=" + std::to_string(invariantObservations.load()));
    Check(invariantObservations.load() > 10000,
          "Observer performed sufficient high-frequency checks",
          "total_checks=" + std::to_string(invariantObservations.load()));

    server.Stop();
}

// ----------------------------------------------------------------------------
// Test 2: Rapid Connect / Disconnect Under High Network Load
// Multi-threaded flood of Video, Audio, and IMU packets abruptly severed.
// ----------------------------------------------------------------------------
void TestHighLoadDisconnectChurn() {
    std::cout << "\n=== [Challenger 7 - Test 2] Rapid Connect/Disconnect Under High Load ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("445566", port, true);
    std::string error;
    server.Start(error);

    const int kIterations = 30;
    std::atomic<uint64_t> totalImuReceived{0};
    std::atomic<uint64_t> totalAudioReceived{0};
    std::atomic<uint64_t> totalVideoPops{0};

    for (int iter = 0; iter < kIterations; ++iter) {
        SOCKET c = ConnectClient(address);
        if (c == INVALID_SOCKET) {
            Check(false, "Connect failed on iteration " + std::to_string(iter));
            break;
        }

        // Authenticate
        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'4', '4', '5', '5', '6', '6'};
        SendMessage(c, hello);
        WaitCondition([&] { return server.Connected(); });

        // Negotiate Ping/Pong
        phonecast::core::protocol::Message ping;
        ping.type = phonecast::core::protocol::MessageType::Ping;
        auto trackCap = phonecast::core::protocol::TrackingCapability();
        auto audioCap = phonecast::core::audio::Capability();
        ping.payload.insert(ping.payload.end(), audioCap.begin(), audioCap.end());
        ping.payload.insert(ping.payload.end(), trackCap.begin(), trackCap.end());
        SendMessage(c, ping);
        phonecast::core::protocol::Message pong;
        RecvMessage(c, pong);

        // Send screen metrics & remote control status
        phonecast::core::protocol::StreamScreenMetrics metrics{};
        metrics.version = 1;
        metrics.pixelWidth = 1080;
        metrics.pixelHeight = 2400;
        metrics.densityDpi = 420;
        metrics.physicalWidthMm = 70.0f;
        metrics.physicalHeightMm = 155.0f;
        metrics.xdpi = 420.0f;
        metrics.ydpi = 420.0f;
        phonecast::core::protocol::Message smMsg;
        smMsg.type = phonecast::core::protocol::MessageType::ScreenMetrics;
        smMsg.payload = phonecast::core::protocol::SerializeScreenMetrics(metrics);
        SendMessage(c, smMsg);

        phonecast::core::protocol::RemoteControlStatus rcSt{true, (iter % 2 == 0)};
        phonecast::core::protocol::Message rcMsg;
        rcMsg.type = phonecast::core::protocol::MessageType::RemoteControlStatus;
        rcMsg.payload = phonecast::core::protocol::SerializeRemoteControlStatus(rcSt);
        SendMessage(c, rcMsg);

        // Spawn client producer thread flooding packets
        std::atomic<bool> floodStop{false};
        std::thread floodThread([&, c] {
            uint64_t seq = 1;
            while (!floodStop.load(std::memory_order_relaxed)) {
                // Send IMU
                phonecast::core::protocol::StreamPhoneImu imu{};
                imu.version = 1;
                imu.sensorType = 1;
                imu.accuracy = 3;
                imu.qw = 1.0f;
                imu.gyroX = 0.01f * static_cast<float>(seq % 10);
                phonecast::core::protocol::Message imuMsg;
                imuMsg.type = phonecast::core::protocol::MessageType::PhoneImu;
                imuMsg.payload = phonecast::core::protocol::SerializePhoneImu(imu);
                if (!SendMessage(c, imuMsg)) break;

                // Send Audio frame (bounded size: 1932 bytes)
                phonecast::core::protocol::Message aMsg;
                aMsg.type = phonecast::core::protocol::MessageType::AudioFrame;
                aMsg.payload.assign(1932, 0x12);
                if (!SendMessage(c, aMsg)) break;

                // Send Video frame
                phonecast::core::protocol::Message vMsg;
                vMsg.type = phonecast::core::protocol::MessageType::VideoFrame;
                vMsg.sequence = seq;
                vMsg.payload.assign(256, 0xAA);
                if (!SendMessage(c, vMsg)) break;

                ++seq;
                std::this_thread::yield();
            }
        });

        // Drain concurrently from server side for a short burst
        const auto burstEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
        while (std::chrono::steady_clock::now() < burstEnd) {
            phonecast::core::protocol::Message v;
            if (server.Pop(v)) totalVideoPops.fetch_add(1, std::memory_order_relaxed);
            phonecast::core::protocol::Message a;
            if (server.PopAudio(a)) totalAudioReceived.fetch_add(1, std::memory_order_relaxed);
            phonecast::core::protocol::StreamPhoneImu imu;
            if (server.PopImu(imu)) totalImuReceived.fetch_add(1, std::memory_order_relaxed);

            // Also test reverse command
            phonecast::core::PointerEvent evt{};
            evt.type = phonecast::core::PointerEvent::Type::Move;
            evt.normalizedX = 0.5f;
            evt.normalizedY = 0.5f;
            std::string err;
            server.Send(evt, err);
        }

        // Abruptly sever the connection while flood is ongoing
        floodStop.store(true);
        shutdown(c, SHUT_RDWR);
        floodThread.join();
        closesocket(c);

        // Server MUST transition cleanly to disconnected state
        const bool disconnected = WaitCondition([&] { return !server.Connected(); }, 1500);
        if (!disconnected) {
            Check(false, "Server did not detect disconnect on iteration " + std::to_string(iter));
            break;
        }

        // Post-disconnect state verification:
        Check(!server.RemoteControlStatusKnown(),
              "RemoteControlStatusKnown reset to false after abrupt drop (iter " + std::to_string(iter) + ")");
        auto stAfter = server.RemoteControlStatus();
        Check(!stAfter.appEnabled && !stAfter.accessibilityEnabled,
              "RemoteControlStatus flags reset to false after drop (iter " + std::to_string(iter) + ")");
        Check(!server.LatestScreenMetrics().has_value(),
              "LatestScreenMetrics cleared after drop (iter " + std::to_string(iter) + ")");
        Check(!server.LatestImu().has_value(),
              "LatestImu cleared after drop (iter " + std::to_string(iter) + ")");
    }

    Check(totalImuReceived.load() > 0, "IMU traffic drained successfully under high load",
          "total_imu=" + std::to_string(totalImuReceived.load()));
    Check(server.SessionGeneration() >= static_cast<uint64_t>(kIterations),
          "Session generation advanced cleanly across all high-load iterations",
          "generation=" + std::to_string(server.SessionGeneration()));

    server.Stop();
}

// ----------------------------------------------------------------------------
// Test 3: Malformed & Truncated Protocol Adversarial Fuzzing
// Server must reject invalid messages without corruption or lockup.
// ----------------------------------------------------------------------------
void TestMalformedAndTruncatedInputs() {
    std::cout << "\n=== [Challenger 7 - Test 3] Malformed & Truncated Inputs ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("998877", port, true);
    std::string error;
    server.Start(error);

    // Subtest 3a: Wrong pairing code
    {
        SOCKET c = ConnectClient(address);
        phonecast::core::protocol::Message badHello;
        badHello.type = phonecast::core::protocol::MessageType::Hello;
        badHello.payload = {'0', '0', '0', '0', '0', '0'};
        SendMessage(c, badHello);
        Check(WaitCondition([&] { return !server.Connected(); }),
              "Client with wrong pairing code rejected cleanly");
        closesocket(c);
    }

    // Subtest 3b: Truncated header then abrupt disconnect
    {
        SOCKET c = ConnectClient(address);
        const uint8_t partialHeader[] = {0x50, 0x43, 0x01}; // 3 bytes instead of 16
        send(c, reinterpret_cast<const char*>(partialHeader), sizeof(partialHeader), 0);
        closesocket(c);
        Check(WaitCondition([&] { return !server.Connected(); }),
              "Truncated header disconnect handled cleanly");
    }

    // Subtest 3c: Valid header claiming 4096 bytes payload, sent only 10 bytes then disconnected
    {
        SOCKET c = ConnectClient(address);
        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'9', '9', '8', '8', '7', '7'};
        SendMessage(c, hello);
        WaitCondition([&] { return server.Connected(); });

        phonecast::core::protocol::Message lieMsg;
        lieMsg.type = phonecast::core::protocol::MessageType::VideoFrame;
        auto serialized = phonecast::core::protocol::Serialize(lieMsg);
        serialized[12] = 0x00;
        serialized[13] = 0x10; // 4096 bytes
        serialized[14] = 0x00;
        serialized[15] = 0x00;
        SendExact(c, serialized);
        std::vector<uint8_t> shortChunk(10, 0xEE);
        SendExact(c, shortChunk);
        closesocket(c);

        Check(WaitCondition([&] { return !server.Connected(); }),
              "Truncated payload before EOF handled cleanly without hanging");
    }

    // Subtest 3d: Normal connect after all malformed inputs
    {
        SOCKET c = ConnectClient(address);
        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'9', '9', '8', '8', '7', '7'};
        SendMessage(c, hello);
        Check(WaitCondition([&] { return server.Connected(); }),
              "Server remains fully functional and accepts normal client after fuzzing");
        closesocket(c);
        WaitCondition([&] { return !server.Connected(); });
    }

    server.Stop();
}

// ----------------------------------------------------------------------------
// Test 4: Stop() SLA Under Active Client Transmission
// Stop() must terminate and join in < 150ms while client is spamming data.
// ----------------------------------------------------------------------------
void TestStopSlaUnderLoad() {
    std::cout << "\n=== [Challenger 7 - Test 4] Stop() SLA Under Active Transmission ===\n";
    for (int run = 0; run < 5; ++run) {
        sockaddr_in address{};
        const auto port = GetEphemeralPort(address);

        phonecast::platform::network::TcpVideoServer server("123456", port, true);
        std::string error;
        server.Start(error);

        SOCKET c = ConnectClient(address);
        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'1', '2', '3', '4', '5', '6'};
        SendMessage(c, hello);
        WaitCondition([&] { return server.Connected(); });

        std::atomic<bool> spamming{true};
        std::thread spammer([&, c] {
            std::vector<uint8_t> junk(1024, 0x55);
            while (spamming.load(std::memory_order_relaxed)) {
                send(c, reinterpret_cast<const char*>(junk.data()), static_cast<int>(junk.size()),
#ifndef _WIN32
                     MSG_NOSIGNAL
#else
                     0
#endif
                );
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto startStop = std::chrono::steady_clock::now();
        server.Stop();
        const auto stopDurationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startStop).count();

        spamming.store(false);
        shutdown(c, SHUT_RDWR);
        spammer.join();
        closesocket(c);

        Check(stopDurationMs < 150.0,
              "Run " + std::to_string(run) + " Stop() SLA satisfied (< 150 ms)",
              std::to_string(stopDurationMs) + " ms");
        Check(!server.Connected(), "Server confirmed not connected after Stop()");
        Check(!server.RemoteControlStatusKnown(), "RemoteControlStatusKnown is false after Stop()");
    }
}

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "M4 Challenger 7: Adversarial Network Concurrency & Disconnect Test Suite\n";
    std::cout << "======================================================================\n";

    TestRemoteControlResetInvariantUnderChurn();
    TestHighLoadDisconnectChurn();
    TestMalformedAndTruncatedInputs();
    TestStopSlaUnderLoad();

    std::cout << "\n======================================================================\n";
    std::cout << "M4 CHALLENGER 7 SUMMARY: " << g_pass << " PASSED, " << g_fail << " FAILED\n";
    std::cout << "======================================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
