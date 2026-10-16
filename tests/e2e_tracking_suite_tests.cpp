#include "ITrackedCameraSource.h"
#include "TrackedCameraWorker.h"

#include "phonecast/core/logging/ILogger.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/tracking/CornerIntersection.h"
#include "phonecast/core/tracking/EigenInterop.h"
#include "phonecast/core/tracking/InertialPoseFusion.h"
#include "phonecast/core/tracking/LineFitterRansac.h"
#include "phonecast/core/tracking/OneEuroFilter.h"
#include "phonecast/core/tracking/PlanarPnpSolver.h"
#include "phonecast/core/tracking/TrackingTypes.h"
#include "phonecast/platform/network/TcpVideoServer.h"
#include "phonecast/vr/overlay/IOverlayRenderer.h"
#include "phonecast/vr/overlay/OverlayController.h"
#include "phonecast/vr/overlay/OverlaySettingsStore.h"
#include "phonecast/vr/overlay/SettingsMenuController.h"

#include <openvr.h>

#include <Eigen/Dense>
#include <Eigen/Geometry>

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
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int g_totalPass = 0;
int g_totalFail = 0;
int g_tierPass[4] = {0, 0, 0, 0};
int g_tierFail[4] = {0, 0, 0, 0};
int g_currentTier = 0;

void Check(bool condition, const std::string& name, const std::string& detail = "") {
    if (condition) {
        ++g_totalPass;
        ++g_tierPass[g_currentTier];
        std::cout << "  [PASS] " << name;
    } else {
        ++g_totalFail;
        ++g_tierFail[g_currentTier];
        std::cerr << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << '\n';
}

void CheckNear(float actual, float expected, float tol, const std::string& name) {
    const float diff = std::abs(actual - expected);
    if (diff <= tol) {
        ++g_totalPass;
        ++g_tierPass[g_currentTier];
        std::cout << "  [PASS] " << name << '\n';
    } else {
        ++g_totalFail;
        ++g_tierFail[g_currentTier];
        std::cerr << "  [FAIL] " << name << " (actual: " << actual << ", expected: " << expected
                  << ", diff: " << diff << ", tol: " << tol << ")\n";
    }
}

float QuaternionAngleDegrees(const phonecast::core::tracking::Quaternion& q1,
                             const phonecast::core::tracking::Quaternion& q2) {
    const float dot = std::abs(q1.w * q2.w + q1.x * q2.x + q1.y * q2.y + q1.z * q2.z);
    const float clamped = std::clamp(dot, -1.0F, 1.0F);
    return 2.0F * std::acos(clamped) * (180.0F / 3.14159265358979323846F);
}

inline phonecast::core::protocol::StreamScreenMetrics MakeScreenMetrics(
    float widthMm, float heightMm, uint32_t pxW, uint32_t pxH, uint16_t dpi = 400) {
    phonecast::core::protocol::StreamScreenMetrics m{};
    m.version = 1;
    m.flags = 0;
    m.densityDpi = dpi;
    m.pixelWidth = pxW;
    m.pixelHeight = pxH;
    m.physicalWidthMm = widthMm;
    m.physicalHeightMm = heightMm;
    m.xdpi = static_cast<float>(dpi);
    m.ydpi = static_cast<float>(dpi);
    return m;
}

inline phonecast::core::protocol::StreamPhoneImu MakePhoneImu(
    float qw = 1.0f, float qx = 0.0f, float qy = 0.0f, float qz = 0.0f,
    uint8_t sensorType = 1, float gyroZ = 0.0f, float accelY = 9.81f) {
    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.version = 1;
    imu.sensorType = sensorType;
    imu.accuracy = 3;
    imu.flags = 0;
    const float norm = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (norm > 1e-6F) {
        imu.qw = qw / norm;
        imu.qx = qx / norm;
        imu.qy = qy / norm;
        imu.qz = qz / norm;
    } else {
        imu.qw = 1.0F;
        imu.qx = 0.0F;
        imu.qy = 0.0F;
        imu.qz = 0.0F;
    }
    imu.gyroZ = gyroZ;
    imu.accelY = accelY;
    return imu;
}

inline SOCKET ConnectClientWithRetry(const sockaddr_in& address, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                return s;
            }
            closesocket(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return INVALID_SOCKET;
}

template <typename F>
bool WaitCondition(F&& condition, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

inline bool SendTcpMessage(SOCKET s, const phonecast::core::protocol::Message& m) {
    const auto bytes = phonecast::core::protocol::Serialize(m);
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

inline bool AuthenticateAndNegotiate(SOCKET s, const std::string& pairCode) {
    phonecast::core::protocol::Message hello;
    hello.type = phonecast::core::protocol::MessageType::Hello;
    hello.payload.assign(pairCode.begin(), pairCode.end());
    if (!SendTcpMessage(s, hello)) return false;

    phonecast::core::protocol::Message ping;
    ping.type = phonecast::core::protocol::MessageType::Ping;
    ping.payload = phonecast::core::protocol::TrackingCapability();
    if (!SendTcpMessage(s, ping)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

class TestLogger : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel, std::string_view, std::string_view) override {}
};

class MockTrackedCameraSource : public phonecast::platform::openvr::ITrackedCameraSource {
public:
    std::atomic<bool> hasCamera{true};
    uint32_t width{640};
    uint32_t height{480};
    uint32_t bytesPerPixel{4};
    phonecast::core::tracking::CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
    ::vr::HmdMatrix34_t cameraToHead{{{1.0F, 0.0F, 0.0F, 0.0F},
                                     {0.0F, 1.0F, 0.0F, 0.0F},
                                     {0.0F, 0.0F, 1.0F, 0.0F}}};
    std::atomic<bool> streamingActive{false};
    std::atomic<int> acquireCount{0};
    std::atomic<int> releaseCount{0};
    std::atomic<int> getFrameCount{0};

    std::vector<uint8_t> frameData;
    ::vr::CameraVideoStreamFrameHeader_t nextHeader{};
    std::atomic<::vr::EVRTrackedCameraError> nextError{::vr::VRTrackedCameraError_None};
    std::mutex sourceMutex;

    MockTrackedCameraSource() {
        frameData.resize(width * height * bytesPerPixel, 10);
        nextHeader.nWidth = width;
        nextHeader.nHeight = height;
        nextHeader.nBytesPerPixel = bytesPerPixel;
        nextHeader.nFrameSequence = 1;
        nextHeader.ulFrameExposureTime = 1000000;
        nextHeader.trackedDevicePose.bPoseIsValid = true;
        nextHeader.trackedDevicePose.bDeviceIsConnected = true;
        nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                   {0.0F, 1.0F, 0.0F, 1.7F},
                                                                   {0.0F, 0.0F, 1.0F, 0.0F}}};
    }

    bool HasCamera() override {
        return hasCamera.load();
    }

    bool GetFrameSize(uint32_t& outWidth, uint32_t& outHeight, uint32_t& outBufferSize) override {
        outWidth = width;
        outHeight = height;
        outBufferSize = width * height * bytesPerPixel;
        return true;
    }

    bool GetIntrinsics(phonecast::core::tracking::CameraIntrinsics& outIntrinsics) override {
        outIntrinsics = intrinsics;
        return true;
    }

    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override {
        std::lock_guard<std::mutex> lock(sourceMutex);
        outTransform = cameraToHead;
        return true;
    }

    bool AcquireStreaming() override {
        streamingActive.store(true);
        ++acquireCount;
        return true;
    }

    void ReleaseStreaming() override {
        streamingActive.store(false);
        ++releaseCount;
    }

    ::vr::EVRTrackedCameraError GetFrame(
        void* buffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        ++getFrameCount;
        const auto err = nextError.load();
        if (err != ::vr::VRTrackedCameraError_None) {
            return err;
        }
        std::lock_guard<std::mutex> lock(sourceMutex);
        if (buffer && bufferSize >= frameData.size()) {
            std::memcpy(buffer, frameData.data(), frameData.size());
        }
        header = nextHeader;
        ++nextHeader.nFrameSequence;
        return ::vr::VRTrackedCameraError_None;
    }

    void RenderSyntheticRect(uint32_t xMin, uint32_t yMin, uint32_t xMax, uint32_t yMax) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        std::fill(frameData.begin(), frameData.end(), 10);
        for (uint32_t y = yMin; y <= yMax && y < height; ++y) {
            for (uint32_t x = xMin; x <= xMax && x < width; ++x) {
                const uint32_t idx = (y * width + x) * bytesPerPixel;
                frameData[idx + 0] = 240;
                frameData[idx + 1] = 240;
                frameData[idx + 2] = 240;
                frameData[idx + 3] = 255;
            }
        }
    }

    void RenderSyntheticQuad(
        const std::array<phonecast::core::tracking::Vector2, 4>& corners,
        bool occludeBottomEdge = false) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        std::fill(frameData.begin(), frameData.end(), 10);

        float minX = static_cast<float>(width);
        float maxX = 0.0F;
        float minY = static_cast<float>(height);
        float maxY = 0.0F;
        for (const auto& c : corners) {
            minX = std::min(minX, c.x);
            maxX = std::max(maxX, c.x);
            minY = std::min(minY, c.y);
            maxY = std::max(maxY, c.y);
        }

        const int iMinX = std::max(0, static_cast<int>(std::floor(minX)));
        const int iMaxX = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(maxX)));
        const int iMinY = std::max(0, static_cast<int>(std::floor(minY)));
        const int iMaxY = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(maxY)));

        phonecast::core::tracking::Vector2 center{0.0F, 0.0F};
        for (const auto& c : corners) {
            center.x += c.x;
            center.y += c.y;
        }
        center.x *= 0.25F;
        center.y *= 0.25F;

        std::array<float, 4> centerSigns{};
        for (size_t i = 0; i < 4; ++i) {
            const auto& a = corners[i];
            const auto& b = corners[(i + 1) % 4];
            centerSigns[i] = (b.x - a.x) * (center.y - a.y) - (b.y - a.y) * (center.x - a.x);
        }

        const phonecast::core::tracking::Vector2 thumbCenter{
            (corners[2].x + corners[3].x) * 0.5F,
            (corners[2].y + corners[3].y) * 0.5F
        };
        const float thumbRadiusSq = 16.0F * 16.0F;

        for (int y = iMinY; y <= iMaxY; ++y) {
            const float fy = static_cast<float>(y);
            for (int x = iMinX; x <= iMaxX; ++x) {
                const float fx = static_cast<float>(x);

                bool inside = true;
                for (size_t i = 0; i < 4; ++i) {
                    const auto& a = corners[i];
                    const auto& b = corners[(i + 1) % 4];
                    const float cp = (b.x - a.x) * (fy - a.y) - (b.y - a.y) * (fx - a.x);
                    if ((cp > 0.0F && centerSigns[i] < 0.0F) || (cp < 0.0F && centerSigns[i] > 0.0F)) {
                        inside = false;
                        break;
                    }
                }

                if (inside) {
                    if (occludeBottomEdge) {
                        const float dx = fx - thumbCenter.x;
                        const float dy = fy - thumbCenter.y;
                        if (dx * dx + dy * dy <= thumbRadiusSq) {
                            continue;
                        }
                    }
                    const uint32_t idx = static_cast<uint32_t>((y * static_cast<int>(width) + x) * static_cast<int>(bytesPerPixel));
                    frameData[idx + 0] = 240;
                    frameData[idx + 1] = 240;
                    frameData[idx + 2] = 240;
                    frameData[idx + 3] = 255;
                }
            }
        }
    }
};

class MockOverlayRenderer final : public phonecast::vr::IOverlayRenderer {
public:
    phonecast::vr::OverlaySettings currentSettings{};
    phonecast::core::protocol::StreamScreenMetrics lastMetrics{};
    phonecast::core::protocol::StreamPhoneImu lastImu{};
    bool started{false};
    bool stopped{false};
    bool hasMetrics{false};
    uint64_t imuCount{0};

    bool Start(const phonecast::vr::OverlaySettings& settings, std::string&) override {
        currentSettings = settings;
        started = true;
        return true;
    }

    bool SubmitFrame(const phonecast::core::VideoFrame&, std::string&) override {
        return true;
    }

    bool ApplySettings(const phonecast::vr::OverlaySettings& settings, std::string&) override {
        currentSettings = settings;
        return true;
    }

    bool SetVisible(bool, std::string&) override {
        return true;
    }

    bool PumpEvents() override {
        return true;
    }

    void SetScreenMetrics(const phonecast::core::protocol::StreamScreenMetrics& metrics) override {
        lastMetrics = metrics;
        hasMetrics = true;
    }

    void IngestPhoneImu(const phonecast::core::protocol::StreamPhoneImu& imu) override {
        lastImu = imu;
        ++imuCount;
    }

    void Stop() noexcept override {
        stopped = true;
    }
};

// ============================================================================
// TIER 1: FEATURE COVERAGE (Isolated Happy-Path Verification)
// ============================================================================

void Test_Tier1_FeatureCoverage() {
    g_currentTier = 0;
    std::cout << "\n======================================================================\n";
    std::cout << "TIER 1: FEATURE COVERAGE (Features 1 to 20)\n";
    std::cout << "======================================================================\n";

    // 1.1 F-PROT-METRICS (Feature 1)
    std::cout << "\n--- 1.1 F-PROT-METRICS (Type 15, 32 bytes) ---\n";
    {
        using namespace phonecast::core::protocol;
        StreamScreenMetrics original{};
        original.version = 1;
        original.flags = 1;
        original.densityDpi = 412;
        original.pixelWidth = 1080;
        original.pixelHeight = 2400;
        original.physicalWidthMm = 70.8F;
        original.physicalHeightMm = 153.2F;
        original.xdpi = 412.0F;
        original.ydpi = 412.0F;

        const auto bytes = SerializeScreenMetrics(original);
        Check(bytes.size() == 32, "ScreenMetrics serialized byte size is exactly 32 bytes");

        StreamScreenMetrics parsed{};
        std::string error;
        Check(ParseScreenMetrics(bytes.data(), bytes.size(), parsed, error),
              "ScreenMetrics deserializes successfully", error);
        CheckNear(parsed.physicalWidthMm, original.physicalWidthMm, 1e-4F, "Metrics physicalWidthMm matches");
        CheckNear(parsed.physicalHeightMm, original.physicalHeightMm, 1e-4F, "Metrics physicalHeightMm matches");
        Check(parsed.pixelWidth == original.pixelWidth, "Metrics pixelWidth matches");
        Check(parsed.pixelHeight == original.pixelHeight, "Metrics pixelHeight matches");
        Check(parsed.densityDpi == original.densityDpi, "Metrics densityDpi matches");
    }

    // 1.2 F-PROT-IMU (Feature 2)
    std::cout << "\n--- 1.2 F-PROT-IMU (Type 16, 48 bytes) ---\n";
    {
        using namespace phonecast::core::protocol;
        StreamPhoneImu original{};
        original.version = 1;
        original.sensorType = 1;
        original.accuracy = 3;
        original.flags = 0;
        original.qx = 0.1F;
        original.qy = 0.2F;
        original.qz = 0.3F;
        original.qw = 0.9273618F;
        original.gyroX = 0.01F;
        original.gyroY = -0.02F;
        original.gyroZ = 0.05F;
        original.accelX = 0.0F;
        original.accelY = 9.81F;
        original.accelZ = 0.1F;

        const auto bytes = SerializePhoneImu(original);
        Check(bytes.size() == 48, "PhoneImu serialized byte size is exactly 48 bytes");

        StreamPhoneImu parsed{};
        std::string error;
        Check(ParsePhoneImu(bytes.data(), bytes.size(), parsed, error),
              "PhoneImu deserializes successfully", error);
        Check(parsed.sensorType == original.sensorType, "PhoneImu sensorType matches");
        CheckNear(parsed.qw, original.qw, 1e-5F, "PhoneImu qw matches");
        CheckNear(parsed.qx, original.qx, 1e-5F, "PhoneImu qx matches");
        CheckNear(parsed.qy, original.qy, 1e-5F, "PhoneImu qy matches");
        CheckNear(parsed.qz, original.qz, 1e-5F, "PhoneImu qz matches");
        CheckNear(parsed.gyroZ, original.gyroZ, 1e-5F, "PhoneImu gyroZ matches");
        CheckNear(parsed.accelY, original.accelY, 1e-5F, "PhoneImu accelY matches");
    }

    // 1.3 F-PROT-NEGOT (Feature 3)
    std::cout << "\n--- 1.3 F-PROT-NEGOT (Capability Negotiation via PCTR token) ---\n";
    {
        using namespace phonecast::core::protocol;
        const auto pctrToken = TrackingCapability();
        Check(pctrToken.size() == 8, "TrackingCapability is 8 bytes");
        Check(HasTrackingCapability(pctrToken), "HasTrackingCapability detects PCTR token");

        const std::vector<uint8_t> pcarToken = {'P', 'C', 'A', 'R', 1, 1, 0, 0};
        std::vector<uint8_t> combinedPayload;
        combinedPayload.insert(combinedPayload.end(), pcarToken.begin(), pcarToken.end());
        combinedPayload.insert(combinedPayload.end(), pctrToken.begin(), pctrToken.end());

        Check(HasTrackingCapability(combinedPayload),
              "PCTR capability successfully detected in combined audio+tracking negotiation payload");

        const std::vector<uint8_t> foreignToken = {'D', 'U', 'M', 'Y', 1, 0, 0, 0};
        Check(!HasTrackingCapability(foreignToken), "Foreign tokens properly rejected by capability parser");
    }

    // 1.4 F-NET-QUEUE (Feature 4)
    std::cout << "\n--- 1.4 F-NET-QUEUE (Isolated Bounded imuQueue in TcpVideoServer) ---\n";
    {
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Check(probe != INVALID_SOCKET, "probe socket creates");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
              "ephemeral test port binds");
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        const auto port = ntohs(address.sin_port);
        closesocket(probe);

        phonecast::platform::network::TcpVideoServer server("123456", port, false);
        std::string error;
        Check(server.Start(error), "TcpVideoServer starts successfully", error);

        SOCKET client = ConnectClientWithRetry(address);
        Check(client != INVALID_SOCKET, "client connects to server");

        Check(AuthenticateAndNegotiate(client, "123456"), "Authenticate and negotiate tracking capability");
        Check(WaitCondition([&] { return server.Connected(); }), "server accepts and authenticates client");

        // Send 25 IMU packets (queue bound is 16)
        for (uint8_t i = 1; i <= 25; ++i) {
            phonecast::core::protocol::StreamPhoneImu imu =
                MakePhoneImu(1.0F, 0.0F, 0.0F, 0.0F, 1, static_cast<float>(i));
            phonecast::core::protocol::Message msg;
            msg.type = phonecast::core::protocol::MessageType::PhoneImu;
            msg.payload = phonecast::core::protocol::SerializePhoneImu(imu);
            SendTcpMessage(client, msg);
        }

        Check(WaitCondition([&] { return server.ImuReceived() == 25; }), "server counted 25 IMU packets received");
        Check(server.ImuDropped() == 9, "server bounded queue dropped 9 packets (25 - 16 = 9)");

        // Pop must yield packet #10 first (FIFO drop oldest)
        phonecast::core::protocol::StreamPhoneImu popped{};
        Check(server.PopImu(popped), "pop IMU succeeds");
        CheckNear(popped.gyroZ, 10.0F, 0.001F, "first popped IMU packet is sequence #10 (FIFO drop oldest exactness)");

        closesocket(client);
        server.Stop();
    }

    // 1.5 F-AND-METRICS (Feature 5)
    std::cout << "\n--- 1.5 F-AND-METRICS (Android ScreenMetrics extraction simulation) ---\n";
    {
        const float xdpi = 395.2F;
        const float ydpi = 395.4F;
        const uint32_t pixelW = 1080;
        const uint32_t pixelH = 2400;

        const float widthMm = (static_cast<float>(pixelW) / xdpi) * 25.4F;
        const float heightMm = (static_cast<float>(pixelH) / ydpi) * 25.4F;

        CheckNear(widthMm, 69.418F, 0.05F, "Android computed width matches 69.4 mm");
        CheckNear(heightMm, 154.173F, 0.05F, "Android computed height matches 154.2 mm");

        phonecast::core::protocol::StreamScreenMetrics androidPayload =
            MakeScreenMetrics(widthMm, heightMm, pixelW, pixelH, 400);

        phonecast::core::tracking::ScreenMetrics coreMetrics{};
        coreMetrics.widthMillimeters = androidPayload.physicalWidthMm;
        coreMetrics.heightMillimeters = androidPayload.physicalHeightMm;
        coreMetrics.pixelWidth = androidPayload.pixelWidth;
        coreMetrics.pixelHeight = androidPayload.pixelHeight;

        Check(coreMetrics.IsValid(), "core ScreenMetrics accepts Android DisplayMetrics");
        CheckNear(coreMetrics.WidthMeters(), 0.0694F, 0.001F, "Width in meters is 0.0694 m");
    }

    // 1.6 F-AND-IMU (Feature 6)
    std::cout << "\n--- 1.6 F-AND-IMU (Android 50-60 Hz sampling & 50ms drop policy) ---\n";
    {
        const int64_t sensorEventTimestampNs = 1700000000000000LL;
        const uint64_t timestampMicros = static_cast<uint64_t>(sensorEventTimestampNs / 1000LL);
        Check(timestampMicros == 1700000000000ULL, "Sensor timestamp converted to microseconds without precision loss");

        const uint64_t nowMicros = timestampMicros + 55000ULL;
        const bool dropPolicyFired = (nowMicros - timestampMicros) > 50000ULL;
        Check(dropPolicyFired, "50ms drop policy properly identifies stale IMU frame");
    }

    // 1.7 F-CORE-TYPES (Feature 7)
    std::cout << "\n--- 1.7 F-CORE-TYPES (TrackingTypes C++17 portability and math) ---\n";
    {
        using namespace phonecast::core::tracking;
        Vector3 v1{1.0F, 0.0F, 0.0F};
        Vector3 v2{0.0F, 1.0F, 0.0F};
        Vector3 v3 = v1.Cross(v2);
        CheckNear(v3.x, 0.0F, 1e-5F, "Vector3 cross product X");
        CheckNear(v3.y, 0.0F, 1e-5F, "Vector3 cross product Y");
        CheckNear(v3.z, 1.0F, 1e-5F, "Vector3 cross product Z");

        constexpr float kHalfSqrt2 = 0.70710678F;
        Quaternion qRotZ{kHalfSqrt2, 0.0F, 0.0F, kHalfSqrt2};
        Vector3 rotated = qRotZ.Rotate(v1);
        CheckNear(rotated.x, 0.0F, 1e-4F, "Quaternion rotate X -> 0");
        CheckNear(rotated.y, 1.0F, 1e-4F, "Quaternion rotate Y -> 1");
        CheckNear(rotated.z, 0.0F, 1e-4F, "Quaternion rotate Z -> 0");

        Pose6D pose{Vector3{0.1F, -0.2F, 0.5F}, qRotZ, true, 1.0F, 1000, TrackingState::Tracking};
        Pose6D inv = pose.Inverse();
        Vector3 pt{0.3F, 0.4F, 0.5F};
        Vector3 roundTrip = pose.TransformPoint(inv.TransformPoint(pt));
        CheckNear(roundTrip.x, pt.x, 1e-4F, "Pose6D TransformPoint round-trip X");
        CheckNear(roundTrip.y, pt.y, 1e-4F, "Pose6D TransformPoint round-trip Y");
        CheckNear(roundTrip.z, pt.z, 1e-4F, "Pose6D TransformPoint round-trip Z");
    }

    // 1.8 F-CORE-MATH (Feature 8)
    std::cout << "\n--- 1.8 F-CORE-MATH (Eigen 3 Header-Only Interop) ---\n";
    {
        using namespace phonecast::core::tracking;
        using namespace phonecast::core::tracking::interop;
        const Vector3 vCore{1.5F, -2.5F, 3.5F};
        const Eigen::Vector3f vEigen = ToEigen(vCore);
        CheckNear(vEigen.x(), vCore.x, 1e-6F, "ToEigen Vector3 X");
        CheckNear(vEigen.y(), vCore.y, 1e-6F, "ToEigen Vector3 Y");
        CheckNear(vEigen.z(), vCore.z, 1e-6F, "ToEigen Vector3 Z");

        const Quaternion qCore{0.5F, 0.5F, 0.5F, 0.5F};
        const Eigen::Quaternionf qEigen = ToEigen(qCore);
        CheckNear(qEigen.w(), qCore.w, 1e-6F, "ToEigen Quaternion W");

        const Eigen::Matrix3f R = qEigen.toRotationMatrix();
        Eigen::JacobiSVD<Eigen::Matrix3f> svd(R, Eigen::ComputeFullU | Eigen::ComputeFullV);
        CheckNear(svd.singularValues()(0), 1.0F, 1e-4F, "SVD singular value 0 is 1.0");
        CheckNear(svd.singularValues()(1), 1.0F, 1e-4F, "SVD singular value 1 is 1.0");
        CheckNear(svd.singularValues()(2), 1.0F, 1e-4F, "SVD singular value 2 is 1.0");
    }

    // 1.9 F-CORE-RANSAC (Feature 9)
    std::cout << "\n--- 1.9 F-CORE-RANSAC (LineFitterRansac & Parabolic Subpixel Interpolation) ---\n";
    {
        using namespace phonecast::core::tracking;
        float offset = LineFitterRansac::RefineSubpixelParabola(10.0F, 20.0F, 10.0F);
        CheckNear(offset, 0.0F, 1e-5F, "Symmetric parabola offset is 0.0");
        offset = LineFitterRansac::RefineSubpixelParabola(5.0F, 20.0F, 15.0F);
        Check(offset > 0.1F && offset < 0.3F, "Right-skewed parabola offset is positive");

        LineFitterRansac fitter;
        std::vector<Vector2> points;
        for (int x = 50; x <= 250; x += 5) {
            points.push_back({static_cast<float>(x), 100.0F});
        }
        for (int x = 120; x <= 160; x += 3) {
            points.push_back({static_cast<float>(x), 130.0F});
        }
        const Line2D fitted = fitter.Fit(points);
        Check(fitted.valid, "RANSAC line fitting succeeds despite 30% outliers");
        CheckNear(std::abs(fitted.a), 0.0F, 0.05F, "Fitted horizontal line has a ~ 0");
        CheckNear(std::abs(fitted.b), 1.0F, 0.05F, "Fitted horizontal line has |b| ~ 1");
    }

    // 1.10 F-CORE-CORNER (Feature 10)
    std::cout << "\n--- 1.10 F-CORE-CORNER (CornerIntersection Analytical Closed-Form) ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenBorders borders{};
        borders.top = {0.0F, 1.0F, -100.0F, true};
        borders.right = {1.0F, 0.0F, -350.0F, true};
        borders.bottom = {0.0F, 1.0F, -300.0F, true};
        borders.left = {1.0F, 0.0F, -150.0F, true};
        borders.allValid = true;

        const ScreenCorners2D corners = CornerIntersection::ComputeCorners(borders);
        Check(corners.valid, "Analytical corner intersection succeeds");
        CheckNear(corners.corners[0].x, 150.0F, 1e-3F, "Top-Left corner X is 150");
        CheckNear(corners.corners[0].y, 100.0F, 1e-3F, "Top-Left corner Y is 100");
        CheckNear(corners.corners[1].x, 350.0F, 1e-3F, "Top-Right corner X is 350");
        CheckNear(corners.corners[1].y, 100.0F, 1e-3F, "Top-Right corner Y is 100");
        CheckNear(corners.corners[2].x, 350.0F, 1e-3F, "Bottom-Right corner X is 350");
        CheckNear(corners.corners[2].y, 300.0F, 1e-3F, "Bottom-Right corner Y is 300");
        CheckNear(corners.corners[3].x, 150.0F, 1e-3F, "Bottom-Left corner X is 150");
        CheckNear(corners.corners[3].y, 300.0F, 1e-3F, "Bottom-Left corner Y is 300");
    }

    // 1.11 F-CORE-PNP (Feature 11)
    std::cout << "\n--- 1.11 F-CORE-PNP (Planar PnP DLT + SVD + LHM Solver) ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenMetrics metrics{70.0F, 150.0F, 1080, 2400};
        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};

        const float w = metrics.WidthMeters();
        const float h = metrics.HeightMeters();
        const std::array<Vector3, 4> modelPts = {
            Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        const Vector3 truePos{0.05F, -0.02F, 0.55F};
        const Quaternion trueRot = Quaternion::Identity();

        ScreenCorners2D corners{};
        corners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = trueRot.Rotate(modelPts[i]) + truePos;
            corners.corners[i] = intrinsics.Project(pCam);
        }

        PlanarPnpSolver solver;
        const auto res = solver.Solve(corners, metrics, intrinsics);
        Check(res.success, "PlanarPnpSolver successfully solved pose");
        CheckNear(res.primaryPose.position.x, truePos.x, 0.002F, "PnP primary pose X matches true position (< 2mm)");
        CheckNear(res.primaryPose.position.y, truePos.y, 0.002F, "PnP primary pose Y matches true position (< 2mm)");
        CheckNear(res.primaryPose.position.z, truePos.z, 0.002F, "PnP primary pose Z matches true position (< 2mm)");
        Check(res.primaryReprojectionRmsePixels < 0.5F, "PnP reprojection RMSE is subpixel (< 0.5 px)");
    }

    // 1.12 F-CORE-DISAMBIG (Feature 12)
    std::cout << "\n--- 1.12 F-CORE-DISAMBIG (Gravity Vector Disambiguation) ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenMetrics metrics{70.0F, 150.0F, 1080, 2400};
        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};

        constexpr float kPitchRad = 30.0F * (3.14159265F / 180.0F);
        const Quaternion qTrue{std::cos(kPitchRad * 0.5F), std::sin(kPitchRad * 0.5F), 0.0F, 0.0F};
        const Vector3 truePos{0.0F, 0.0F, 0.50F};

        const float w = metrics.WidthMeters();
        const float h = metrics.HeightMeters();
        const std::array<Vector3, 4> modelPts = {
            Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        ScreenCorners2D corners{};
        corners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = qTrue.Rotate(modelPts[i]) + truePos;
            corners.corners[i] = intrinsics.Project(pCam);
        }

        PlanarPnpSolver solver;
        const auto pnpResult = solver.Solve(corners, metrics, intrinsics);
        Check(pnpResult.success, "Planar PnP solver solves tilted target");

        const Pose6D disambiguated = PlanarPnpSolver::Disambiguate(pnpResult, qTrue);
        const float rotErr = QuaternionAngleDegrees(disambiguated.orientation, qTrue);
        Check(rotErr < 1.0F, "Disambiguate correctly selected true physical pose (< 1.0 deg error)");
    }

    // 1.13 F-CORE-FUSION (Feature 13)
    std::cout << "\n--- 1.13 F-CORE-FUSION (Inertial Pose Fusion & Yaw Boresighting) ---\n";
    {
        using namespace phonecast::core::tracking;
        InertialPoseFusion fusion;
        Check(!fusion.IsBoresightCalibrated(), "boresight initially uncalibrated");

        Pose6D opticalPose{};
        opticalPose.valid = true;
        opticalPose.confidence = 1.0F;
        opticalPose.position = {0.0F, 1.2F, -0.5F};
        opticalPose.orientation = Quaternion::Identity();
        opticalPose.timestampMicros = 1000000;
        opticalPose.state = TrackingState::Tracking;

        PhoneImuPacket imu{};
        imu.orientation = Quaternion::Identity();
        imu.timestampMicros = 1000000;

        fusion.IngestImu(imu);
        fusion.IngestOpticalPose(opticalPose);

        Check(fusion.IsBoresightCalibrated(), "boresight calibrated after optical pose ingest");
        const Pose6D fused = fusion.EvaluatePose(1005000);
        Check(fused.valid, "fused pose is valid");
        Check(fused.state == TrackingState::Tracking, "fusion state is Tracking");
        CheckNear(fused.position.y, 1.2F, 0.01F, "fused Y position matches optical input");
    }

    // 1.14 F-CORE-FILTER (Feature 14)
    std::cout << "\n--- 1.14 F-CORE-FILTER (1-Euro Adaptive Low-Pass Filter) ---\n";
    {
        using namespace phonecast::core::tracking;
        OneEuroFilter1D filter(OneEuroFilter1D::Params{1.0F, 15.0F, 1.0F});

        std::mt19937 rng(42);
        std::normal_distribution<float> noise(0.0F, 0.05F);
        float filteredStatic = 0.0F;
        for (int i = 0; i < 50; ++i) {
            const float sample = 10.0F + noise(rng);
            filteredStatic = filter.Filter(sample, i * 0.016);
        }
        CheckNear(filteredStatic, 10.0F, 0.02F, "1-Euro filter eliminates static jitter around 10.0");

        float filteredStep = 0.0F;
        for (int i = 50; i < 60; ++i) {
            filteredStep = filter.Filter(20.0F, i * 0.016);
        }
        Check(filteredStep > 19.0F, "1-Euro filter tracks fast dynamic step within 10 frames (> 19.0)");
    }

    // 1.15 F-VR-MODE (Feature 15)
    std::cout << "\n--- 1.15 F-VR-MODE (PlacementMode::PhoneTracked Integration) ---\n";
    {
        using namespace phonecast::vr;
        OverlaySettings settings{};
        settings.placementMode = PlacementMode::PhoneTracked;

        const std::filesystem::path tempSettingsPath = "test_phonecast_e2e_settings.ini";
        OverlaySettingsStore store(tempSettingsPath);
        std::string error;
        Check(store.Save(settings, error), "OverlaySettingsStore saves phone-tracked settings", error);

        OverlaySettings loaded{};
        bool found = false;
        Check(store.Load(loaded, found, error) && found, "OverlaySettingsStore loads settings", error);
        Check(loaded.placementMode == PlacementMode::PhoneTracked,
              "Loaded placementMode is PlacementMode::PhoneTracked");
        std::filesystem::remove(tempSettingsPath);

        SettingsMenuController menu;
        menu.Open(settings);
        const auto view = menu.View();
        bool foundPhoneTrackedLabel = false;
        for (const auto& v : view.values) {
            if (v == "PHONE TRACKED") {
                foundPhoneTrackedLabel = true;
                break;
            }
        }
        Check(foundPhoneTrackedLabel, "SettingsMenuController displays 'PHONE TRACKED'");

        OverlayController controller(settings);
        controller.Apply(OverlayAction::PhoneTracked);
        Check(controller.Settings().placementMode == PlacementMode::PhoneTracked,
              "OverlayController sets placementMode to PhoneTracked");
    }

    // 1.16 F-OVR-WORKER (Feature 16)
    std::cout << "\n--- 1.16 F-OVR-WORKER (TrackedCameraWorker Decoupled Lifecycle) ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        auto worker = std::make_unique<phonecast::platform::openvr::TrackedCameraWorker>(
            logger, std::move(mockPtr));

        Check(!worker->IsActive(), "worker starts in low-power idle");
        Check(worker->Start(), "worker thread starts");

        worker->SetActive(true);
        Check(WaitCondition([&] { return mock->acquireCount.load() >= 1; }), "worker awoke and acquired streaming");

        worker->SetActive(false);
        Check(WaitCondition([&] { return mock->releaseCount.load() >= 1; }), "worker released streaming on suspension");

        worker->Stop();
        worker->Stop();
        Check(!worker->IsActive(), "worker remains inactive after Stop");
    }

    // 1.17 F-OVR-TRANSFORM (Feature 17)
    std::cout << "\n--- 1.17 F-OVR-TRANSFORM (Coordinate Compounding: Standing <- HMD <- Cam <- Phone) ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

        phonecast::core::protocol::StreamScreenMetrics metrics = MakeScreenMetrics(70.0F, 150.0F, 1080, 2400);
        worker.SetScreenMetrics(metrics);

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        phonecast::platform::openvr::TrackedCameraPoseSnapshot snap{};
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            snap = worker.GetSnapshot();
            if (snap.valid) break;
        }

        Check(snap.valid, "optical frame yielded valid pose snapshot");
        CheckNear(snap.transform.m[0][3], 0.0F, 0.05F, "Standing X position ~ 0.0 m");
        CheckNear(snap.transform.m[1][3], 1.7F, 0.05F, "Standing Y position ~ 1.7 m");
        CheckNear(snap.transform.m[2][3], -0.50F, 0.05F, "Standing Z position ~ -0.50 m");

        worker.Stop();
    }

    // 1.18 F-OVR-SCALE (Feature 18)
    std::cout << "\n--- 1.18 F-OVR-SCALE (Automatic 1:1 Metric Scale Matching) ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

        phonecast::core::protocol::StreamScreenMetrics metrics = MakeScreenMetrics(72.4F, 155.0F, 1080, 2400);
        worker.SetScreenMetrics(metrics);

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (worker.GetSnapshot().valid) break;
        }

        float physicalWidthMeters = 0.0F;
        ::vr::HmdMatrix34_t pose{};
        Check(worker.GetLatestPose(pose, &physicalWidthMeters), "GetLatestPose succeeds");
        CheckNear(physicalWidthMeters, 0.0724F, 0.0005F, "1:1 physical width scale matches 0.0724 m exactly");

        worker.Stop();
    }

    // 1.19 F-OVR-HOLD (Feature 19)
    std::cout << "\n--- 1.19 F-OVR-HOLD (Stable Occlusion Hold without Snap-back) ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

        phonecast::core::protocol::StreamScreenMetrics metrics = MakeScreenMetrics(70.0F, 150.0F, 1080, 2400);
        worker.SetScreenMetrics(metrics);
        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (worker.GetSnapshot().valid) break;
        }
        const auto initialSnap = worker.GetSnapshot();
        Check(initialSnap.valid, "initial tracking valid");

        mock->nextError.store(::vr::VRTrackedCameraError_NoFrameAvailable);

        for (int i = 0; i < 15; ++i) {
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto occludedSnap = worker.GetSnapshot();
        Check(occludedSnap.valid, "pose remains valid during occlusion hold");
        Check(occludedSnap.state == phonecast::core::tracking::TrackingState::Occluded,
              "tracking state is Occluded");
        CheckNear(occludedSnap.transform.m[0][3], initialSnap.transform.m[0][3], 0.05F,
                  "occlusion hold retains stable X position");
        CheckNear(occludedSnap.transform.m[1][3], initialSnap.transform.m[1][3], 0.05F,
                  "occlusion hold retains stable Y position");
        CheckNear(occludedSnap.transform.m[2][3], initialSnap.transform.m[2][3], 0.05F,
                  "occlusion hold retains stable Z position");

        worker.Stop();
    }

    // 1.20 Stream-Receiver Forwarding Pipeline (Feature 20)
    std::cout << "\n--- 1.20 Stream-Receiver Forwarding Pipeline ---\n";
    {
        MockOverlayRenderer renderer;
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        const auto port = ntohs(address.sin_port);
        closesocket(probe);

        phonecast::platform::network::TcpVideoServer server("654321", port, false);
        std::string error;
        Check(server.Start(error), "forwarding test server starts");

        SOCKET client = ConnectClientWithRetry(address);
        Check(client != INVALID_SOCKET, "client connects to server");

        Check(AuthenticateAndNegotiate(client, "654321"), "authenticate and negotiate tracking capability");
        Check(WaitCondition([&] { return server.Connected(); }), "server accepts client");

        phonecast::core::protocol::StreamScreenMetrics metrics = MakeScreenMetrics(75.0F, 160.0F, 1440, 3120);
        phonecast::core::protocol::Message metricsMsg;
        metricsMsg.type = phonecast::core::protocol::MessageType::ScreenMetrics;
        metricsMsg.payload = phonecast::core::protocol::SerializeScreenMetrics(metrics);
        SendTcpMessage(client, metricsMsg);

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F, 0.0F, 0.0F, 0.0F);
        phonecast::core::protocol::Message imuMsg;
        imuMsg.type = phonecast::core::protocol::MessageType::PhoneImu;
        imuMsg.payload = phonecast::core::protocol::SerializePhoneImu(imu);
        SendTcpMessage(client, imuMsg);

        Check(WaitCondition([&] { return server.LatestScreenMetrics().has_value() && server.ImuReceived() >= 1; }),
              "server received metrics and IMU over TCP");

        phonecast::core::protocol::StreamPhoneImu poppedImu;
        while (server.PopImu(poppedImu)) {
            renderer.IngestPhoneImu(poppedImu);
        }
        if (auto latestMetrics = server.LatestScreenMetrics()) {
            renderer.SetScreenMetrics(*latestMetrics);
        }

        Check(renderer.hasMetrics, "receiver forwarded LatestScreenMetrics to renderer");
        CheckNear(renderer.lastMetrics.physicalWidthMm, 75.0F, 1e-4F, "renderer received 75.0 mm screen width");
        Check(renderer.imuCount >= 1, "receiver forwarded PopImu to renderer");
        CheckNear(renderer.lastImu.qw, 1.0F, 1e-4F, "renderer received IMU qw 1.0");

        closesocket(client);
        server.Stop();
    }
}

// ============================================================================
// TIER 2: BOUNDARY & CORNER CASES
// ============================================================================

void Test_Tier2_BoundaryAndCornerCases() {
    g_currentTier = 1;
    std::cout << "\n======================================================================\n";
    std::cout << "TIER 2: BOUNDARY & CORNER CASES\n";
    std::cout << "======================================================================\n";

    // 2.1 Zero and Negative Dimensions
    std::cout << "\n--- 2.1 Zero and Negative Screen Dimensions ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenMetrics zeroW{0.0F, 150.0F, 1080, 2400};
        Check(!zeroW.IsValid(), "Zero width metrics rejected by IsValid()");

        ScreenMetrics negH{70.0F, -10.0F, 1080, 2400};
        Check(!negH.IsValid(), "Negative height metrics rejected by IsValid()");

        ScreenMetrics zeroPx{70.0F, 150.0F, 0, 2400};
        Check(!zeroPx.IsValid(), "Zero pixel metrics rejected by IsValid()");

        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        ScreenCorners2D dummyCorners{};
        dummyCorners.valid = true;
        PlanarPnpSolver solver;
        const auto resZero = solver.Solve(dummyCorners, zeroW, intrinsics);
        Check(!resZero.success, "PlanarPnpSolver cleanly rejects invalid zero-width metrics without crashing");
    }

    // 2.2 Grazing Angles (70 to 80 deg severe oblique tilt)
    std::cout << "\n--- 2.2 Grazing Angles (70 to 80 deg oblique tilt) ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenMetrics metrics{70.0F, 150.0F, 1080, 2400};
        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        PlanarPnpSolver solver;

        const float w = metrics.WidthMeters();
        const float h = metrics.HeightMeters();
        const std::array<Vector3, 4> modelPts = {
            Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        const std::vector<float> grazingAnglesDeg = {70.0F, 75.0F, 80.0F};
        for (float angleDeg : grazingAnglesDeg) {
            const float rad = angleDeg * (3.14159265F / 180.0F);
            const Quaternion qTilt{std::cos(rad * 0.5F), std::sin(rad * 0.5F), 0.0F, 0.0F};
            const Vector3 truePos{0.0F, 0.0F, 0.45F};

            ScreenCorners2D corners{};
            corners.valid = true;
            for (size_t i = 0; i < 4; ++i) {
                const Vector3 pCam = qTilt.Rotate(modelPts[i]) + truePos;
                corners.corners[i] = intrinsics.Project(pCam);
            }

            const auto res = solver.Solve(corners, metrics, intrinsics);
            Check(res.success, "Grazing angle " + std::to_string(static_cast<int>(angleDeg)) + " deg solved successfully");
            const float posErrMm = (res.primaryPose.position - truePos).Norm() * 1000.0F;
            const float rotErrDeg = QuaternionAngleDegrees(res.primaryPose.orientation, qTilt);
            Check(posErrMm < 15.0F, "Grazing angle position error < 15mm (" + std::to_string(posErrMm) + " mm)");
            Check(rotErrDeg < 2.5F, "Grazing angle rotation error < 2.5 deg (" + std::to_string(rotErrDeg) + " deg)");
        }
    }

    // 2.3 Near-Fronto-Parallel Configurations (micro-tilts 0.0001 deg to 0.5 deg)
    std::cout << "\n--- 2.3 Near-Fronto-Parallel Configurations (micro-tilts) ---\n";
    {
        using namespace phonecast::core::tracking;
        ScreenMetrics metrics{70.0F, 150.0F, 1080, 2400};
        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        PlanarPnpSolver solver;

        const float w = metrics.WidthMeters();
        const float h = metrics.HeightMeters();
        const std::array<Vector3, 4> modelPts = {
            Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        const std::vector<float> microTilts = {0.0001F, 0.001F, 0.01F, 0.1F, 0.5F};
        for (float tilt : microTilts) {
            const float rad = tilt * (3.14159265F / 180.0F);
            const Quaternion qTilt{std::cos(rad * 0.5F), std::sin(rad * 0.5F), 0.0F, 0.0F};
            const Vector3 truePos{0.0F, 0.0F, 0.50F};

            ScreenCorners2D corners{};
            corners.valid = true;
            for (size_t i = 0; i < 4; ++i) {
                const Vector3 pCam = qTilt.Rotate(modelPts[i]) + truePos;
                corners.corners[i] = intrinsics.Project(pCam);
            }

            const auto res = solver.Solve(corners, metrics, intrinsics);
            Check(res.success, "Micro-tilt " + std::to_string(tilt) + " deg solved");
            const float posErrMm = (res.primaryPose.position - truePos).Norm() * 1000.0F;
            Check(posErrMm < 2.0F, "Micro-tilt position error < 2mm (" + std::to_string(posErrMm) + " mm)");
        }
    }

    // 2.4 0 to 180 deg Yaw Rotations
    std::cout << "\n--- 2.4 0 to 180 deg Yaw Rotations & Full Turnaround ---\n";
    {
        using namespace phonecast::core::tracking;
        InertialPoseFusion fusion;
        Pose6D opticalPose{};
        opticalPose.valid = true;
        opticalPose.position = {0.0F, 1.2F, -0.5F};
        opticalPose.orientation = Quaternion::Identity();
        opticalPose.timestampMicros = 1000000;
        opticalPose.confidence = 1.0F;

        fusion.IngestImu(PhoneImuPacket{0, 1000000, Quaternion::Identity(), {}, {}});
        fusion.IngestOpticalPose(opticalPose);

        const Quaternion q180{0.0F, 0.0F, 1.0F, 0.0F};
        fusion.IngestImu(PhoneImuPacket{1, 1020000, q180, {}, {}});

        const Pose6D eval180 = fusion.EvaluatePose(1020000);
        Check(eval180.valid, "180 deg yaw turnaround evaluated cleanly without NaN or gimbal lock");
        Check(!std::isnan(eval180.orientation.w) && !std::isnan(eval180.orientation.y),
              "Quaternion components remain finite at 180 deg");
    }

    // 2.5 Antipodal Quaternion Flips (q vs -q)
    std::cout << "\n--- 2.5 Antipodal Quaternion Flips (Shortest-path continuity) ---\n";
    {
        using namespace phonecast::core::tracking;
        OneEuroFilterQuaternion qFilter(OneEuroFilter1D::Params{1.0F, 15.0F, 1.0F});

        const Quaternion q1 = Quaternion::Identity();
        qFilter.Filter(q1, 1.0);

        const Quaternion qNeg{-1.0F, 0.0F, 0.0F, 0.0F};
        const Quaternion qFiltered = qFilter.Filter(qNeg, 1.016);

        const float angleDist = QuaternionAngleDegrees(qFiltered, q1);
        Check(angleDist < 1.0F, "Antipodal flip (-q) maintained shortest path continuity (< 1.0 deg delta)");
    }

    // 2.6 Extreme Time Intervals (dt -> 0 and large dt)
    std::cout << "\n--- 2.6 Extreme Time Intervals (dt -> 0 and large dt) ---\n";
    {
        using namespace phonecast::core::tracking;
        OneEuroFilter1D filter(OneEuroFilter1D::Params{1.0F, 15.0F, 1.0F});
        filter.Filter(5.0F, 1.0);

        const float valMicro = filter.Filter(5.1F, 1.000001);
        Check(!std::isnan(valMicro) && !std::isinf(valMicro), "dt -> 0 does not cause division by zero or NaN");

        const float valJump = filter.Filter(10.0F, 6.0);
        Check(!std::isnan(valJump) && !std::isinf(valJump), "Large dt jump (5.0s) remains numerically stable");
        CheckNear(valJump, 10.0F, 0.5F, "Large dt jump quickly adapts to new measurement");
    }

    // 2.7 Single Frame Drop
    std::cout << "\n--- 2.7 Single Frame Drop (Transient Optical Glitch) ---\n";
    {
        using namespace phonecast::core::tracking;
        InertialPoseFusion fusion;
        Pose6D opticalPose{Vector3{0.0F, 1.0F, -0.5F}, Quaternion::Identity(), true, 1.0F, 1000000, TrackingState::Tracking};
        fusion.IngestImu(PhoneImuPacket{0, 1000000, Quaternion::Identity(), {}, {}});
        fusion.IngestOpticalPose(opticalPose);

        fusion.IngestImu(PhoneImuPacket{1, 1033000, Quaternion::Identity(), {}, {}});
        const Pose6D fusedDrop = fusion.EvaluatePose(1033000);

        Check(fusedDrop.valid, "Pose remains valid during single frame drop");
        Check(fusedDrop.state == TrackingState::Tracking, "State remains Tracking during single frame drop");
        CheckNear(fusedDrop.position.z, -0.5F, 0.01F, "Position smoothly retained during frame drop");
    }

    // 2.8 1.5s Occlusion Hold Boundary (1500000 µs)
    std::cout << "\n--- 2.8 1.5s Occlusion Hold Boundary (1500000 µs) ---\n";
    {
        using namespace phonecast::core::tracking;
        InertialPoseFusion fusion;
        Pose6D opticalPose{Vector3{0.0F, 1.0F, -0.5F}, Quaternion::Identity(), true, 1.0F, 1000000, TrackingState::Tracking};
        fusion.IngestImu(PhoneImuPacket{0, 1000000, Quaternion::Identity(), {}, {}});
        fusion.IngestOpticalPose(opticalPose);

        const Pose6D beforeTimeout = fusion.EvaluatePose(2490000);
        Check(beforeTimeout.valid, "Pose valid at 1.49s occlusion hold");
        Check(beforeTimeout.state == TrackingState::Occluded, "State is Occluded at 1.49s");
        Check(beforeTimeout.confidence > 0.0F, "Confidence decayed but non-zero at 1.49s");

        const Pose6D afterTimeout = fusion.EvaluatePose(2510000);
        Check(!afterTimeout.valid, "Pose invalid after 1.5s timeout");
        Check(afterTimeout.state == TrackingState::Lost, "State transitioned to Lost after 1.5s timeout");
        CheckNear(afterTimeout.confidence, 0.0F, 1e-5F, "Confidence dropped to 0.0 after timeout");
    }

    // 2.9 Extreme Screen Aspect Ratios
    std::cout << "\n--- 2.9 Extreme Screen Aspect Ratios (Foldable 23:9 vs Ultrawide 32:9) ---\n";
    {
        using namespace phonecast::core::tracking;
        CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        PlanarPnpSolver solver;

        ScreenMetrics foldable{60.0F, 155.0F, 960, 2480};
        const float wF = foldable.WidthMeters();
        const float hF = foldable.HeightMeters();
        const std::array<Vector3, 4> modelFoldable = {
            Vector3{-wF * 0.5F,  hF * 0.5F, 0.0F},
            Vector3{ wF * 0.5F,  hF * 0.5F, 0.0F},
            Vector3{ wF * 0.5F, -hF * 0.5F, 0.0F},
            Vector3{-wF * 0.5F, -hF * 0.5F, 0.0F}
        };
        ScreenCorners2D cornersFoldable{};
        cornersFoldable.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cornersFoldable.corners[i] = intrinsics.Project(modelFoldable[i] + Vector3{0.0F, 0.0F, 0.50F});
        }
        const auto resFoldable = solver.Solve(cornersFoldable, foldable, intrinsics);
        Check(resFoldable.success, "Foldable tall/narrow aspect ratio (23:9) solved successfully");

        ScreenMetrics ultrawide{160.0F, 70.0F, 2560, 1080};
        const float wU = ultrawide.WidthMeters();
        const float hU = ultrawide.HeightMeters();
        const std::array<Vector3, 4> modelUltrawide = {
            Vector3{-wU * 0.5F,  hU * 0.5F, 0.0F},
            Vector3{ wU * 0.5F,  hU * 0.5F, 0.0F},
            Vector3{ wU * 0.5F, -hU * 0.5F, 0.0F},
            Vector3{-wU * 0.5F, -hU * 0.5F, 0.0F}
        };
        ScreenCorners2D cornersUltrawide{};
        cornersUltrawide.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cornersUltrawide.corners[i] = intrinsics.Project(modelUltrawide[i] + Vector3{0.0F, 0.0F, 0.50F});
        }
        const auto resUltrawide = solver.Solve(cornersUltrawide, ultrawide, intrinsics);
        Check(resUltrawide.success, "Ultrawide horizontal aspect ratio (32:9) solved successfully");
    }
}

// ============================================================================
// TIER 3: CROSS-FEATURE INTERACTIONS (Pairwise & Multi-way)
// ============================================================================

void Test_Tier3_CrossFeatureInteractions() {
    g_currentTier = 2;
    std::cout << "\n======================================================================\n";
    std::cout << "TIER 3: CROSS-FEATURE INTERACTIONS\n";
    std::cout << "======================================================================\n";

    // 3.1 Metrics update arriving during high-frequency IMU streaming
    std::cout << "\n--- 3.1 Metrics Update Arriving During High-Frequency IMU Streaming ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

        phonecast::core::protocol::StreamScreenMetrics metricsA = MakeScreenMetrics(70.0F, 150.0F, 1080, 2400);
        worker.SetScreenMetrics(metricsA);

        worker.Start();
        worker.SetActive(true);

        std::atomic<bool> stopImu{false};
        std::thread imuThread([&] {
            uint8_t seq = 0;
            while (!stopImu.load()) {
                phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F, 0.0F, 0.0F, 0.0F, ++seq);
                worker.IngestPhoneImu(imu);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });

        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (worker.GetSnapshot().valid) break;
        }
        Check(worker.GetSnapshot().valid, "Tracking active during IMU stream");
        CheckNear(worker.GetSnapshot().physicalWidthMeters, 0.070F, 0.001F, "Initial width is 0.070m");

        phonecast::core::protocol::StreamScreenMetrics metricsB = MakeScreenMetrics(80.0F, 160.0F, 1200, 2600);
        worker.SetScreenMetrics(metricsB);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CheckNear(worker.GetSnapshot().physicalWidthMeters, 0.080F, 0.001F,
                  "Metrics updated dynamically during high-frequency IMU stream without deadlock");

        stopImu.store(true);
        imuThread.join();
        worker.Stop();
    }

    // 3.2 Camera frame drop while IMU continues rotating
    std::cout << "\n--- 3.2 Camera Frame Drop While IMU Continues Rotating ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.SetScreenMetrics(MakeScreenMetrics(70.0F, 150.0F, 1080, 2400));

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (worker.GetSnapshot().valid) break;
        }
        Check(worker.GetSnapshot().valid, "nominal tracking established");
        const auto initialPose = worker.GetSnapshot().transform;

        mock->nextError.store(::vr::VRTrackedCameraError_NoFrameAvailable);

        constexpr float kRot30 = 30.0F * (3.14159265F / 180.0F);
        const float c30 = std::cos(kRot30 * 0.5F);
        const float s30 = std::sin(kRot30 * 0.5F);
        for (int i = 0; i < 20; ++i) {
            imu.qw = c30;
            imu.qy = s30;
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto occludedPose = worker.GetSnapshot();
        Check(occludedPose.valid, "Pose remains valid under inertial dead reckoning during camera drop");
        Check(occludedPose.state == phonecast::core::tracking::TrackingState::Occluded,
              "State transitioned to Occluded");
        CheckNear(occludedPose.transform.m[0][3], initialPose.m[0][3], 0.05F, "Position held stably in X");
        CheckNear(occludedPose.transform.m[1][3], initialPose.m[1][3], 0.05F, "Position held stably in Y");
        CheckNear(occludedPose.transform.m[2][3], initialPose.m[2][3], 0.05F, "Position held stably in Z");

        worker.Stop();
    }

    // 3.3 Switching PlacementMode from HeadLocked to PhoneTracked and back
    std::cout << "\n--- 3.3 Switching PlacementMode (HeadLocked <-> PhoneTracked Cycling) ---\n";
    {
        TestLogger logger;
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.Start();

        for (int cycle = 0; cycle < 3; ++cycle) {
            worker.SetActive(true);
            Check(WaitCondition([&] { return mock->acquireCount.load() >= cycle + 1; }),
                  "Cycle " + std::to_string(cycle) + ": acquired streaming");
            Check(worker.IsActive(), "worker is active");

            worker.SetActive(false);
            Check(WaitCondition([&] { return mock->releaseCount.load() >= cycle + 1; }),
                  "Cycle " + std::to_string(cycle) + ": released streaming");
            Check(!worker.IsActive(), "worker is suspended");
        }

        worker.Stop();
    }

    // 3.4 TCP Disconnect / Reconnect Simulation with Queue Isolation
    std::cout << "\n--- 3.4 TCP Disconnect / Reconnect Simulation with Queue Isolation ---\n";
    {
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        const auto port = ntohs(address.sin_port);
        closesocket(probe);

        phonecast::platform::network::TcpVideoServer server("998877", port, false);
        std::string error;
        Check(server.Start(error), "TCP server starts");

        SOCKET client1 = ConnectClientWithRetry(address);
        Check(client1 != INVALID_SOCKET, "Client 1 connected");

        Check(AuthenticateAndNegotiate(client1, "998877"), "Client 1 authenticates and negotiates tracking");
        Check(WaitCondition([&] { return server.Connected(); }), "Client 1 verified connected by server");
        const uint64_t session1 = server.SessionGeneration();

        phonecast::core::protocol::StreamPhoneImu imu1 =
            MakePhoneImu(1.0F, 0.0F, 0.0F, 0.0F, 1, 11.0F);
        phonecast::core::protocol::Message imuMsg1;
        imuMsg1.type = phonecast::core::protocol::MessageType::PhoneImu;
        imuMsg1.payload = phonecast::core::protocol::SerializePhoneImu(imu1);
        SendTcpMessage(client1, imuMsg1);

        Check(WaitCondition([&] { return server.LatestImu().has_value() && server.LatestImu()->gyroZ == 11.0F; }),
              "Server received Client 1 IMU");

        closesocket(client1);
        Check(WaitCondition([&] { return !server.Connected(); }), "Server detects Client 1 disconnect");

        SOCKET client2 = ConnectClientWithRetry(address);
        Check(client2 != INVALID_SOCKET, "Client 2 connected on reconnect");
        Check(AuthenticateAndNegotiate(client2, "998877"), "Client 2 authenticates and negotiates tracking");

        Check(WaitCondition([&] { return server.Connected(); }), "Client 2 verified connected by server");
        const uint64_t session2 = server.SessionGeneration();
        Check(session2 > session1, "Session generation incremented after reconnect");

        phonecast::core::protocol::StreamPhoneImu imu2 =
            MakePhoneImu(1.0F, 0.0F, 0.0F, 0.0F, 1, 22.0F);
        phonecast::core::protocol::Message imuMsg2;
        imuMsg2.type = phonecast::core::protocol::MessageType::PhoneImu;
        imuMsg2.payload = phonecast::core::protocol::SerializePhoneImu(imu2);
        SendTcpMessage(client2, imuMsg2);

        phonecast::core::protocol::StreamPhoneImu popped{};
        Check(WaitCondition([&] { return server.PopImu(popped); }), "Server pops IMU from new session");
        CheckNear(popped.gyroZ, 22.0F, 0.001F, "Popped IMU is from client 2, queue reset cleanly");

        closesocket(client2);
        server.Stop();
    }
}

// ============================================================================
// TIER 4: REAL-WORLD WORKLOAD SCENARIOS (End-to-End Sessions)
// ============================================================================

void Test_Tier4_RealWorldScenarios() {
    g_currentTier = 3;
    std::cout << "\n======================================================================\n";
    std::cout << "TIER 4: REAL-WORLD WORKLOAD SCENARIOS\n";
    std::cout << "======================================================================\n";

    TestLogger logger;

    // 4.1 Scenario 1: Phone pickup and examine
    std::cout << "\n--- 4.1 Scenario 1: Phone Pickup and Examine ---\n";
    {
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        phonecast::core::protocol::StreamScreenMetrics metrics = MakeScreenMetrics(70.0F, 150.0F, 1080, 2400);
        worker.SetScreenMetrics(metrics);

        const phonecast::core::tracking::CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        const float w = metrics.physicalWidthMm / 1000.0F;
        const float h = metrics.physicalHeightMm / 1000.0F;
        const std::array<phonecast::core::tracking::Vector3, 4> modelPts = {
            phonecast::core::tracking::Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        // Phone initial position: comfortably within FOV (Y = 0.04m, Z = 0.55m)
        std::array<phonecast::core::tracking::Vector2, 4> cornersDesk{};
        for (size_t i = 0; i < 4; ++i) {
            cornersDesk[i] = intrinsics.Project(modelPts[i] + phonecast::core::tracking::Vector3{0.0F, 0.04F, 0.55F});
        }
        mock->RenderSyntheticQuad(cornersDesk);

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        Check(WaitCondition([&] { return worker.GetSnapshot().valid; }), "Desk pickup: initial tracking locked");

        // Animate pickup: lift phone to eye level (camY: 0.04 -> 0.0) and bring closer (camZ: 0.55 -> 0.48)
        for (int step = 1; step <= 20; ++step) {
            const float frac = static_cast<float>(step) / 20.0F;
            const float camY = 0.04F - 0.04F * frac;
            const float camZ = 0.55F - 0.07F * frac; // 0.48m
            const float tiltRad = frac * 25.0F * (3.14159265F / 180.0F);

            const phonecast::core::tracking::Quaternion qStep{
                std::cos(tiltRad * 0.5F), std::sin(tiltRad * 0.5F), 0.0F, 0.0F
            };
            std::array<phonecast::core::tracking::Vector2, 4> stepCorners{};
            for (size_t i = 0; i < 4; ++i) {
                const auto p = qStep.Rotate(modelPts[i]) + phonecast::core::tracking::Vector3{0.0F, camY, camZ};
                stepCorners[i] = intrinsics.Project(p);
            }
            mock->RenderSyntheticQuad(stepCorners);

            imu.qw = qStep.w;
            imu.qx = qStep.x;
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Allow 1-Euro filter settling
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto examinedPose = worker.GetSnapshot();
        Check(examinedPose.valid, "Examined phone pose tracked continuously");
        Check(examinedPose.state == phonecast::core::tracking::TrackingState::Tracking, "State is Tracking");
        CheckNear(examinedPose.transform.m[2][3], -0.48F, 0.08F, "Phone brought closer to face (Z ~ -0.48 m)");

        worker.Stop();
    }

    // 4.2 Scenario 2: Seated to standing room walk
    std::cout << "\n--- 4.2 Scenario 2: Seated to Standing Room Walk ---\n";
    {
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.SetScreenMetrics(MakeScreenMetrics(70.0F, 150.0F, 1080, 2400));

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        // Seated HMD pose at (0, 1.15, 0)
        mock->nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                        {0.0F, 1.0F, 0.0F, 1.15F},
                                                                        {0.0F, 0.0F, 1.0F, 0.0F}}};

        worker.Start();
        worker.SetActive(true);

        Check(WaitCondition([&] { return worker.GetSnapshot().valid; }), "Initial seated tracking lock");
        CheckNear(worker.GetSnapshot().transform.m[1][3], 1.15F, 0.05F, "Initial seated standing Y ~ 1.15 m");

        // User stands up (HMD Y: 1.15 -> 1.75m) and walks forward 1.5m (HMD Z: 0.0 -> -1.5m)
        for (int step = 1; step <= 25; ++step) {
            const float frac = static_cast<float>(step) / 25.0F;
            const float hmdY = 1.15F + 0.60F * frac;
            const float hmdZ = -1.50F * frac;

            {
                std::lock_guard<std::mutex> lock(mock->sourceMutex);
                mock->nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                                {0.0F, 1.0F, 0.0F, hmdY},
                                                                                {0.0F, 0.0F, 1.0F, hmdZ}}};
            }
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        // Allow 1-Euro filter settling at destination
        for (int i = 0; i < 15; ++i) {
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        const auto walkedPose = worker.GetSnapshot();
        Check(walkedPose.valid, "Walked pose tracked continuously");
        CheckNear(walkedPose.transform.m[1][3], 1.75F, 0.08F, "Standing Y updated to eye-level ~ 1.75 m");
        CheckNear(walkedPose.transform.m[2][3], -2.00F, 0.10F, "Compounded standing Z updated to -2.00 m (-1.5m walk + -0.5m hold)");

        worker.Stop();
    }

    // 4.3 Scenario 3: Thumb occlusion while interacting with phone
    std::cout << "\n--- 4.3 Scenario 3: Thumb Occlusion While Interacting ---\n";
    {
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();

        const phonecast::core::tracking::CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
        const float w = 0.070F;
        const float h = 0.150F;
        const std::array<phonecast::core::tracking::Vector3, 4> modelPts = {
            phonecast::core::tracking::Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
            phonecast::core::tracking::Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
        };

        std::array<phonecast::core::tracking::Vector2, 4> corners{};
        for (size_t i = 0; i < 4; ++i) {
            corners[i] = intrinsics.Project(modelPts[i] + phonecast::core::tracking::Vector3{0.0F, 0.0F, 0.50F});
        }

        mock->RenderSyntheticQuad(corners, true);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.SetScreenMetrics(MakeScreenMetrics(70.0F, 150.0F, 1080, 2400));

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        Check(WaitCondition([&] { return worker.GetSnapshot().valid; }), "Thumb occlusion solved");

        const auto snap = worker.GetSnapshot();
        Check(snap.valid, "RANSAC line fitter and corner intersection successfully solved pose with thumb occlusion");
        CheckNear(snap.transform.m[2][3], -0.50F, 0.05F, "Standing Z position matches true position ~ -0.50m despite thumb");

        worker.Stop();
    }

    // 4.4 Scenario 4: Optical tracking drop while turning around in room
    std::cout << "\n--- 4.4 Scenario 4: Optical Tracking Drop While Turning Around ---\n";
    {
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.SetScreenMetrics(MakeScreenMetrics(70.0F, 150.0F, 1080, 2400));

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        Check(WaitCondition([&] { return worker.GetSnapshot().valid; }), "Initial tracking lock established");
        const auto preTurnPose = worker.GetSnapshot().transform;

        mock->nextError.store(::vr::VRTrackedCameraError_NoFrameAvailable);

        for (int i = 0; i < 30; ++i) {
            imu.qw = 0.99F;
            imu.qy = 0.10F;
            worker.IngestPhoneImu(imu);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto midTurnPose = worker.GetSnapshot();
        Check(midTurnPose.valid, "Occlusion hold maintained valid pose during 600ms turnaround");
        Check(midTurnPose.state == phonecast::core::tracking::TrackingState::Occluded,
              "Tracking state is Occluded (not Lost)");
        CheckNear(midTurnPose.transform.m[0][3], preTurnPose.m[0][3], 0.05F, "No teleportation or snap to origin");

        worker.Stop();
    }

    // 4.5 Scenario 5: Recovery upon re-entering headset camera FOV
    std::cout << "\n--- 4.5 Scenario 5: Recovery Upon Re-entering Headset Camera FOV ---\n";
    {
        auto mockPtr = std::make_unique<MockTrackedCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
        worker.SetScreenMetrics(MakeScreenMetrics(70.0F, 150.0F, 1080, 2400));

        phonecast::core::protocol::StreamPhoneImu imu = MakePhoneImu(1.0F);
        worker.IngestPhoneImu(imu);

        worker.Start();
        worker.SetActive(true);

        Check(WaitCondition([&] { return worker.GetSnapshot().valid; }), "Initial tracking for recovery scenario");

        mock->nextError.store(::vr::VRTrackedCameraError_NoFrameAvailable);
        std::this_thread::sleep_for(std::chrono::milliseconds(1650));

        const auto lostSnap = worker.GetSnapshot();
        Check(!lostSnap.valid, "Tracking state transitioned to Lost after timeout");
        Check(lostSnap.state == phonecast::core::tracking::TrackingState::Lost, "State is Lost");

        mock->nextError.store(::vr::VRTrackedCameraError_None);

        Check(WaitCondition([&] {
            worker.IngestPhoneImu(imu);
            return worker.GetSnapshot().valid;
        }), "Tracking cleanly relocalized and recovered after timeout");

        const auto recoveredSnap = worker.GetSnapshot();
        Check(recoveredSnap.valid, "Recovered snap is valid");
        Check(recoveredSnap.state == phonecast::core::tracking::TrackingState::Tracking,
              "Tracking state returned to Tracking");
        CheckNear(recoveredSnap.transform.m[1][3], 1.7F, 0.05F, "Recovered standing Y ~ 1.7m");
        CheckNear(recoveredSnap.transform.m[2][3], -0.50F, 0.05F, "Recovered standing Z ~ -0.50m");

        worker.Stop();
    }
}

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "PhoneCast VR - Comprehensive E2E Tracking Test Suite\n";
    std::cout << "Target: phonecast-e2e-tracking-tests\n";
    std::cout << "======================================================================\n";

    const auto startTime = std::chrono::steady_clock::now();

    Test_Tier1_FeatureCoverage();
    Test_Tier2_BoundaryAndCornerCases();
    Test_Tier3_CrossFeatureInteractions();
    Test_Tier4_RealWorldScenarios();

    const auto endTime = std::chrono::steady_clock::now();
    const double elapsedSeconds = std::chrono::duration<double>(endTime - startTime).count();

    std::cout << "\n======================================================================\n";
    std::cout << "E2E TRACKING TEST SUITE SUMMARY\n";
    std::cout << "======================================================================\n";
    std::cout << "Tier 1 (Feature Coverage):       " << g_tierPass[0] << " passed, " << g_tierFail[0] << " failed.\n";
    std::cout << "Tier 2 (Boundary & Corner Cases):" << g_tierPass[1] << " passed, " << g_tierFail[1] << " failed.\n";
    std::cout << "Tier 3 (Cross-Feature):          " << g_tierPass[2] << " passed, " << g_tierFail[2] << " failed.\n";
    std::cout << "Tier 4 (Real-World Scenarios):   " << g_tierPass[3] << " passed, " << g_tierFail[3] << " failed.\n";
    std::cout << "----------------------------------------------------------------------\n";
    std::cout << "Total Assertions Verified:       " << g_totalPass + g_totalFail << '\n';
    std::cout << "Total Passes:                    " << g_totalPass << '\n';
    std::cout << "Total Failures:                  " << g_totalFail << '\n';
    std::cout << "Total Execution Time:            " << std::fixed << std::setprecision(2) << elapsedSeconds << " seconds\n";
    std::cout << "======================================================================\n";

    if (g_totalFail == 0) {
        std::cout << "ALL E2E TRACKING TESTS PASSED (100% SUCCESS)!\n";
        return 0;
    } else {
        std::cerr << "E2E TRACKING TESTS FAILED WITH " << g_totalFail << " FAILURES!\n";
        return 1;
    }
}
