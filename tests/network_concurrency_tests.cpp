#include "phonecast/platform/network/TcpVideoServer.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/audio/AudioProtocol.h"
#include "phonecast/core/logging/ILogger.h"
#include "phonecast/vr/overlay/IOverlayRenderer.h"
#include "phonecast/vr/overlay/OverlaySettingsStore.h"
#include "phonecast/vr/overlay/SettingsMenuController.h"

#include "ITrackedCameraSource.h"
#include "TrackedCameraWorker.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return close(s); }
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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

class TestLogger : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel, std::string_view, std::string_view) override {}
};

class MockCameraSource : public phonecast::platform::openvr::ITrackedCameraSource {
public:
    std::atomic<bool> streamingActive{false};
    std::atomic<int> acquireCount{0};
    std::atomic<int> releaseCount{0};

    bool HasCamera() override { return true; }
    bool GetFrameSize(uint32_t& w, uint32_t& h, uint32_t& b) override {
        w = 640; h = 480; b = 640 * 480 * 4; return true;
    }
    bool GetIntrinsics(phonecast::core::tracking::CameraIntrinsics& i) override {
        i = {750.0F, 750.0F, 320.0F, 240.0F, 640, 480}; return true;
    }
    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& t) override {
        t = {{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}}};
        return true;
    }
    bool AcquireStreaming() override {
        streamingActive.store(true);
        acquireCount.fetch_add(1);
        return true;
    }
    void ReleaseStreaming() override {
        streamingActive.store(false);
        releaseCount.fetch_add(1);
    }
    ::vr::EVRTrackedCameraError GetFrame(void*, uint32_t, ::vr::CameraVideoStreamFrameHeader_t& h) override {
        h.nWidth = 640; h.nHeight = 480; h.nBytesPerPixel = 4;
        h.nFrameSequence = 1;
        return ::vr::VRTrackedCameraError_None;
    }
};

template <typename Predicate>
bool WaitCondition(Predicate pred, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

SOCKET ConnectClient(const sockaddr_in& address, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                return s;
            }
            closesocket(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

// ============================================================================
// Test 1: RequestVideoResync State Machine & Network Round-Trip
// ============================================================================
void TestRequestVideoResync() {
    std::cout << "\n=== [Test 1] RequestVideoResync State Machine & Round-Trip ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("123456", port, false);
    std::string error;

    // Disconnected state
    Check(!server.RequestVideoResync(error), "RequestVideoResync fails when disconnected");
    Check(!error.empty(), "Error message populated on disconnected RequestVideoResync");

    Check(server.Start(error), "Server started");
    SOCKET client = ConnectClient(address);
    Check(client != INVALID_SOCKET, "Client connected");

    // Authenticate
    phonecast::core::protocol::Message hello;
    hello.type = phonecast::core::protocol::MessageType::Hello;
    hello.payload = {'1', '2', '3', '4', '5', '6'};
    Check(SendMessage(client, hello), "Client authenticated");
    Check(WaitCondition([&] { return server.Connected(); }), "Server reports connected");

    // Send VideoConfig
    phonecast::core::protocol::Message configMsg;
    configMsg.type = phonecast::core::protocol::MessageType::VideoConfig;
    configMsg.payload = {0x01, 0x02};
    SendMessage(client, configMsg);

    // Initial RequestKeyFrame from VideoConfig
    phonecast::core::protocol::Message req1;
    Check(RecvMessage(client, req1), "Received initial keyframe request from VideoConfig");
    Check(req1.type == phonecast::core::protocol::MessageType::RequestKeyFrame, "Type is RequestKeyFrame");

    // Pop the VideoConfig on server
    phonecast::core::protocol::Message popped;
    Check(WaitCondition([&] { return server.Pop(popped); }), "Server pops VideoConfig");

    // Send KeyFrame seq 0
    phonecast::core::protocol::Message keyFrame;
    keyFrame.type = phonecast::core::protocol::MessageType::VideoFrame;
    keyFrame.flags = phonecast::core::protocol::MessageFlags::KeyFrame;
    keyFrame.sequence = 0;
    keyFrame.payload = {0xAA};
    SendMessage(client, keyFrame);
    Check(WaitCondition([&] { return server.Pop(popped); }), "Server pops keyframe");
    Check(popped.sequence == 0, "Popped sequence is 0");

    // Send 3 delta frames (seq 1, 2, 3)
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        phonecast::core::protocol::Message delta;
        delta.type = phonecast::core::protocol::MessageType::VideoFrame;
        delta.sequence = seq;
        delta.payload = {0xBB};
        SendMessage(client, delta);
    }

    // Wait until server has all 3 in queue
    Check(WaitCondition([&] { return server.Stats().queueDepth == 3; }), "Queue depth reached 3");

    // Now call RequestVideoResync()!
    Check(server.RequestVideoResync(error), "RequestVideoResync succeeds while connected", error);

    // Verify:
    // a) Queued prediction frames were discarded immediately
    const auto stats = server.Stats();
    Check(stats.queueDepth == 0, "Queue depth dropped to 0 (queued prediction frames discarded)");
    Check(stats.droppedFrames >= 3, "Dropped frames incremented by discarded count");
    Check(stats.resyncRequests >= 2, "Resync requests incremented");

    // b) Client received RequestKeyFrame message over TCP
    phonecast::core::protocol::Message req2;
    Check(RecvMessage(client, req2), "Client received resync RequestKeyFrame over TCP");
    Check(req2.type == phonecast::core::protocol::MessageType::RequestKeyFrame, "Message type is RequestKeyFrame");

    // c) If client sends another delta frame (seq 4), server must drop it (waitingForKeyFrame)
    phonecast::core::protocol::Message delta4;
    delta4.type = phonecast::core::protocol::MessageType::VideoFrame;
    delta4.sequence = 4;
    delta4.payload = {0xCC};
    SendMessage(client, delta4);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(!server.Pop(popped), "Server dropped delta frame received while waitingForKeyFrame");

    // d) If client sends a new KeyFrame (seq 5), server must accept it
    phonecast::core::protocol::Message key5;
    key5.type = phonecast::core::protocol::MessageType::VideoFrame;
    key5.flags = phonecast::core::protocol::MessageFlags::KeyFrame;
    key5.sequence = 5;
    key5.payload = {0xDD};
    SendMessage(client, key5);
    Check(WaitCondition([&] { return server.Pop(popped); }), "Server accepted new KeyFrame after resync");
    Check(popped.sequence == 5, "Accepted frame is keyframe sequence 5");

    closesocket(client);
    server.Stop();
}

// ============================================================================
// Test 2: Send (Pointer Input) and SendNotificationOpen over TCP
// ============================================================================
void TestRemoteInputAndNotificationSend() {
    std::cout << "\n=== [Test 2] Send (Pointer Input) & SendNotificationOpen ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("654321", port, false);
    std::string error;

    phonecast::core::PointerEvent event{};
    event.type = phonecast::core::PointerEvent::Type::Down;
    event.normalizedX = 0.5F;
    event.normalizedY = 0.5F;
    event.sequence = 42;

    // Disconnected errors
    Check(!server.Send(event, error), "Send fails when disconnected");
    Check(!server.SendNotificationOpen(1001, error), "SendNotificationOpen fails when disconnected");
    Check(server.SendNotificationOpen(0, error), "SendNotificationOpen with token 0 is a valid no-op");

    Check(server.Start(error), "Server started");
    SOCKET client = ConnectClient(address);
    Check(client != INVALID_SOCKET, "Client connected");

    phonecast::core::protocol::Message hello;
    hello.type = phonecast::core::protocol::MessageType::Hello;
    hello.payload = {'6', '5', '4', '3', '2', '1'};
    SendMessage(client, hello);
    Check(WaitCondition([&] { return server.Connected(); }), "Client authenticated");

    // Test Send(event)
    Check(server.Send(event, error), "Send pointer event succeeds while connected", error);
    phonecast::core::protocol::Message rxPointer;
    Check(RecvMessage(client, rxPointer), "Client received RemoteInput message");
    Check(rxPointer.type == phonecast::core::protocol::MessageType::RemoteInput, "Message type is RemoteInput");
    Check(rxPointer.sequence == 42, "Pointer sequence matches 42");
    phonecast::core::PointerEvent parsedEvent{};
    Check(phonecast::core::protocol::ParsePointerEvent(rxPointer.payload.data(), rxPointer.payload.size(), parsedEvent, error),
          "Pointer event deserialized cleanly", error);
    Check(parsedEvent.type == phonecast::core::PointerEvent::Type::Down, "Type is Down");
    Check(std::abs(parsedEvent.normalizedX - 0.5F) < 1e-4F, "normalizedX is 0.5");

    // Test SendNotificationOpen
    Check(server.SendNotificationOpen(998877, error), "SendNotificationOpen succeeds", error);
    phonecast::core::protocol::Message rxNotif;
    Check(RecvMessage(client, rxNotif), "Client received NotificationOpen message");
    Check(rxNotif.type == phonecast::core::protocol::MessageType::NotificationOpen, "Type is NotificationOpen");
    Check(rxNotif.sequence == 998877, "Notification action token matches 998877");

    // Test SendRemoteText
    Check(server.SendRemoteText("Testing Text Injection", error), "SendRemoteText succeeds", error);
    phonecast::core::protocol::Message rxText;
    Check(RecvMessage(client, rxText), "Client received RemoteText message");
    Check(rxText.type == phonecast::core::protocol::MessageType::RemoteText, "Type is RemoteText");
    std::string parsedRemoteText;
    Check(phonecast::core::protocol::ParseRemoteText(rxText.payload.data(), rxText.payload.size(), parsedRemoteText, error),
          "RemoteText deserialized cleanly", error);
    Check(parsedRemoteText == "Testing Text Injection", "Text matches injected string");

    closesocket(client);
    server.Stop();
}

// ============================================================================
// Test 3: RemoteControlStatus Ingest and Session Isolation
// ============================================================================
void TestRemoteControlStatusLifecycle() {
    std::cout << "\n=== [Test 3] RemoteControlStatus Ingest and Session Isolation ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("111222", port, false);
    std::string error;
    Check(!server.RemoteControlStatusKnown(), "Initial RemoteControlStatusKnown is false");

    server.Start(error);
    SOCKET client = ConnectClient(address);

    phonecast::core::protocol::Message hello;
    hello.type = phonecast::core::protocol::MessageType::Hello;
    hello.payload = {'1', '1', '1', '2', '2', '2'};
    SendMessage(client, hello);
    WaitCondition([&] { return server.Connected(); });

    // Send malformed RemoteControlStatus
    phonecast::core::protocol::Message malformed;
    malformed.type = phonecast::core::protocol::MessageType::RemoteControlStatus;
    malformed.payload = {0xFF};
    SendMessage(client, malformed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(!server.RemoteControlStatusKnown(), "Malformed RemoteControlStatus rejected; status remains unknown");

    // Send valid RemoteControlStatus (appEnabled=true, accessibilityEnabled=false)
    phonecast::core::protocol::RemoteControlStatus st1{true, false};
    phonecast::core::protocol::Message validMsg1;
    validMsg1.type = phonecast::core::protocol::MessageType::RemoteControlStatus;
    validMsg1.payload = phonecast::core::protocol::SerializeRemoteControlStatus(st1);
    SendMessage(client, validMsg1);

    Check(WaitCondition([&] { return server.RemoteControlStatusKnown(); }), "RemoteControlStatusKnown becomes true");
    auto currentSt = server.RemoteControlStatus();
    Check(currentSt.appEnabled && !currentSt.accessibilityEnabled, "Status reflects appEnabled=true, accessibility=false");
    Check(!currentSt.Ready(), "RemoteControlStatus is not Ready when accessibility is false");

    // Update to Ready (both enabled)
    phonecast::core::protocol::RemoteControlStatus st2{true, true};
    phonecast::core::protocol::Message validMsg2;
    validMsg2.type = phonecast::core::protocol::MessageType::RemoteControlStatus;
    validMsg2.payload = phonecast::core::protocol::SerializeRemoteControlStatus(st2);
    SendMessage(client, validMsg2);

    Check(WaitCondition([&] { return server.RemoteControlStatus().Ready(); }), "RemoteControlStatus becomes Ready");

    // Disconnect client
    closesocket(client);
    Check(WaitCondition([&] { return !server.Connected(); }), "Server observed disconnect");
    Check(WaitCondition([&] { return !server.RemoteControlStatusKnown(); }), "RemoteControlStatusKnown reset to false on disconnect");

    server.Stop();
}

// ============================================================================
// Test 4: Extreme Multi-Threaded Concurrency Hammer
// ============================================================================
void TestMultiThreadedConcurrencyHammer() {
    std::cout << "\n=== [Test 4] Extreme Multi-Threaded Concurrency Hammer ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("333444", port, true);
    std::string error;
    server.Start(error);

    SOCKET client = ConnectClient(address);
    phonecast::core::protocol::Message hello;
    hello.type = phonecast::core::protocol::MessageType::Hello;
    hello.payload = {'3', '3', '3', '4', '4', '4'};
    SendMessage(client, hello);
    WaitCondition([&] { return server.Connected(); });

    // Negotiate tracking and audio
    phonecast::core::protocol::Message ping;
    ping.type = phonecast::core::protocol::MessageType::Ping;
    auto trackCap = phonecast::core::protocol::TrackingCapability();
    auto audioCap = phonecast::core::audio::Capability();
    ping.payload.insert(ping.payload.end(), audioCap.begin(), audioCap.end());
    ping.payload.insert(ping.payload.end(), trackCap.begin(), trackCap.end());
    SendMessage(client, ping);
    phonecast::core::protocol::Message pong;
    RecvMessage(client, pong);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> popsVideo{0}, popsImu{0}, popsAudio{0}, sendsInput{0};

    // Thread 1: Drain Video
    std::thread t1([&] {
        phonecast::core::protocol::Message m;
        while (running.load()) {
            if (server.Pop(m)) ++popsVideo;
            else std::this_thread::yield();
        }
    });

    // Thread 2: Drain IMU & poll LatestImu
    std::thread t2([&] {
        phonecast::core::protocol::StreamPhoneImu imu;
        while (running.load()) {
            (void)server.LatestImu();
            if (server.PopImu(imu)) ++popsImu;
            else std::this_thread::yield();
        }
    });

    // Thread 3: Drain Audio
    std::thread t3([&] {
        phonecast::core::protocol::Message aMsg;
        while (running.load()) {
            if (server.PopAudio(aMsg)) ++popsAudio;
            else std::this_thread::yield();
        }
    });

    // Thread 4: Poll ScreenMetrics & Stats
    std::thread t4([&] {
        while (running.load()) {
            (void)server.LatestScreenMetrics();
            (void)server.Stats();
            (void)server.ImuDropped();
            (void)server.ImuReceived();
            (void)server.AudioDropped();
            std::this_thread::yield();
        }
    });

    // Thread 5: Concurrent Sends & Resync Requests
    std::thread t5([&] {
        phonecast::core::PointerEvent evt{};
        evt.type = phonecast::core::PointerEvent::Type::Move;
        std::string err;
        while (running.load()) {
            server.Send(evt, err);
            server.SendNotificationOpen(0, err);
            server.RequestVideoResync(err);
            ++sendsInput;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Thread 6: Sender hammering socket with interleaved video, IMU, and audio
    std::thread t6([&] {
        uint64_t seq = 0;
        phonecast::core::protocol::StreamPhoneImu imu{};
        imu.version = 1; imu.sensorType = 1; imu.qw = 1.0F;
        const auto imuPayload = phonecast::core::protocol::SerializePhoneImu(imu);

        while (running.load()) {
            // Send IMU
            phonecast::core::protocol::Message imuMsg;
            imuMsg.type = phonecast::core::protocol::MessageType::PhoneImu;
            imuMsg.payload = imuPayload;
            SendMessage(client, imuMsg);

            // Send Video
            phonecast::core::protocol::Message vMsg;
            vMsg.type = phonecast::core::protocol::MessageType::VideoFrame;
            vMsg.flags = (seq % 10 == 0) ? phonecast::core::protocol::MessageFlags::KeyFrame : 0;
            vMsg.sequence = seq++;
            vMsg.payload = {0x00, 0x00, 0x00, 0x01};
            SendMessage(client, vMsg);

            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    running.store(false);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();

    Check(popsImu.load() > 0, "Concurrent hammer successfully processed IMU packets",
          "pops=" + std::to_string(popsImu.load()));
    Check(sendsInput.load() > 0, "Concurrent hammer successfully sent reverse commands",
          "sends=" + std::to_string(sendsInput.load()));
    Check(true, "No deadlock, crash, or data race occurred during concurrent hammer");

    closesocket(client);
    server.Stop();
}

// ============================================================================
// Test 5: Rapid Client Connect / Disconnect / Reconnect Churn
// ============================================================================
void TestRapidReconnectChurn() {
    std::cout << "\n=== [Test 5] Rapid Client Connect / Disconnect / Reconnect Churn ===\n";
    sockaddr_in address{};
    const auto port = GetEphemeralPort(address);

    phonecast::platform::network::TcpVideoServer server("777888", port, false);
    std::string error;
    server.Start(error);

    const int kIterations = 15;
    for (int i = 0; i < kIterations; ++i) {
        SOCKET c = ConnectClient(address);
        Check(c != INVALID_SOCKET, "Churn iteration " + std::to_string(i) + " connected");

        phonecast::core::protocol::Message hello;
        hello.type = phonecast::core::protocol::MessageType::Hello;
        hello.payload = {'7', '7', '7', '8', '8', '8'};
        SendMessage(c, hello);

        WaitCondition([&] { return server.Connected(); });
        Check(server.Connected(), "Server confirmed connection in iteration " + std::to_string(i));

        // Abruptly close
        closesocket(c);
        WaitCondition([&] { return !server.Connected(); });
    }

    Check(server.SessionGeneration() >= kIterations,
          "Session generation incremented cleanly across rapid reconnect churn",
          "generation=" + std::to_string(server.SessionGeneration()));

    const auto startStop = std::chrono::steady_clock::now();
    server.Stop();
    const auto stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startStop).count();
    Check(stopMs < 150.0, "Clean destruction SLA satisfied (< 150 ms)",
          std::to_string(stopMs) + " ms");
}

// ============================================================================
// Test 6: TrackedCameraWorker Low-Power Condition-Variable Sleep/Wake
// ============================================================================
void TestWorkerConditionVariableSleepWake() {
    std::cout << "\n=== [Test 6] TrackedCameraWorker Sleep/Wake Condition Variable ===\n";
    TestLogger logger;
    auto mock = std::make_unique<MockCameraSource>();
    auto* mockPtr = mock.get();

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mock));
    worker.Start();

    // 100 rapid SetActive toggles from 2 concurrent threads
    std::atomic<bool> stopToggling{false};
    std::thread toggler1([&] {
        for (int i = 0; i < 50 && !stopToggling.load(); ++i) {
            worker.SetActive(true);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            worker.SetActive(false);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    std::thread toggler2([&] {
        for (int i = 0; i < 50 && !stopToggling.load(); ++i) {
            worker.SetActive(false);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            worker.SetActive(true);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    toggler1.join();
    toggler2.join();

    // Leave active and check acquire
    worker.SetActive(true);
    Check(WaitCondition([&] { return mockPtr->streamingActive.load(); }),
          "Worker woke up on condition variable and acquired streaming");

    // Leave inactive and check release
    worker.SetActive(false);
    Check(WaitCondition([&] { return !mockPtr->streamingActive.load(); }),
          "Worker released streaming and returned to low-power condition variable wait");

    const auto t0 = std::chrono::steady_clock::now();
    worker.Stop();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    Check(elapsed < 50.0, "Worker stopped cleanly under condition variable sleep (< 50 ms)",
          std::to_string(elapsed) + " ms");
}

// ============================================================================
// Test 7: SettingsMenuController & OverlaySettingsStore PhoneTracked Integration
// ============================================================================
void TestSettingsAndStorePhoneTracked() {
    std::cout << "\n=== [Test 7] SettingsMenuController & OverlaySettingsStore ===\n";

    phonecast::vr::SettingsMenuController menu;
    phonecast::vr::OverlaySettings settings{};
    settings.placementMode = phonecast::vr::PlacementMode::RightControllerLocked;

    menu.Open(settings);
    Check(menu.IsOpen(), "Menu opened");

    // Navigate to Placement page
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem); // 1 = Placement
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(menu.View().title == "PLACEMENT", "On PLACEMENT page");

    // Cycle placement forward: RightControllerLocked (3) -> PhoneTracked (4)
    menu.Handle(phonecast::vr::SettingsMenuCommand::Increase);
    Check(menu.Draft().placementMode == phonecast::vr::PlacementMode::PhoneTracked,
          "Placement mode cycled to PlacementMode::PhoneTracked");
    Check(menu.View().values[0] == "PHONE TRACKED", "Menu view displays 'PHONE TRACKED'");

    // Cycle placement forward: PhoneTracked (4) -> HeadLocked (0) (wrap-around)
    menu.Handle(phonecast::vr::SettingsMenuCommand::Increase);
    Check(menu.Draft().placementMode == phonecast::vr::PlacementMode::HeadLocked,
          "Placement mode wrapped around to HeadLocked");

    // Save and load round-trip via OverlaySettingsStore
    const auto tempDir = std::filesystem::temp_directory_path() / "phonecast_test_store";
    std::filesystem::create_directories(tempDir);
    const auto iniFile = tempDir / "test-settings.ini";
    phonecast::vr::OverlaySettingsStore store(iniFile);
    phonecast::vr::OverlaySettings toSave{};
    toSave.placementMode = phonecast::vr::PlacementMode::PhoneTracked;
    toSave.distanceMeters = 0.50F;

    // Test 7a: Virtual screen width (0.20m)
    toSave.widthMeters = 0.20F;
    std::string err;
    Check(store.Save(toSave, err), "Saved OverlaySettings with width 0.20m", err);
    phonecast::vr::OverlaySettings loadedA{};
    bool foundA = false;
    Check(store.Load(loadedA, foundA, err), "Loaded OverlaySettings with width 0.20m succeeds", err);
    Check(loadedA.placementMode == phonecast::vr::PlacementMode::PhoneTracked, "Loaded placementMode is PhoneTracked");
    Check(std::abs(loadedA.widthMeters - 0.20F) < 1e-4F, "Loaded widthMeters matches 0.20m");

    // Test 7b: Physical phone width (0.075m / 75mm, typical smartphone under PhoneTracked)
    // BUG REPORT: OverlaySettingsStore::Load enforces minimum 0.10m, rejecting real phones!
    toSave.widthMeters = 0.075F;
    Check(store.Save(toSave, err), "Saved OverlaySettings with physical phone width 0.075m", err);
    phonecast::vr::OverlaySettings loadedB{};
    bool foundB = false;
    const bool loadSuccess = store.Load(loadedB, foundB, err);
    Check(loadSuccess && foundB, "OverlaySettingsStore::Load succeeds for physical phone width 0.075m", err);
    Check(loadedB.placementMode == phonecast::vr::PlacementMode::PhoneTracked, "Loaded placementMode is PhoneTracked");
    Check(std::abs(loadedB.widthMeters - 0.075F) < 1e-4F, "Loaded widthMeters matches 0.075m");

    std::filesystem::remove_all(tempDir);
}

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "M4 Challenger 4: Adversarial Network, Concurrency & OpenVR Test Suite\n";
    std::cout << "======================================================================\n";

    TestRequestVideoResync();
    TestRemoteInputAndNotificationSend();
    TestRemoteControlStatusLifecycle();
    TestMultiThreadedConcurrencyHammer();
    TestRapidReconnectChurn();
    TestWorkerConditionVariableSleepWake();
    TestSettingsAndStorePhoneTracked();

    std::cout << "\n======================================================================\n";
    std::cout << "M4 CHALLENGER 4 TEST SUMMARY: " << g_pass << " PASSED, " << g_fail << " FAILED\n";
    std::cout << "======================================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
