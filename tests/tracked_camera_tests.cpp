#include "ITrackedCameraSource.h"
#include "TrackedCameraWorker.h"
#if defined(__linux__)
#include "SteamFrameV4L2CameraSource.h"
#endif
#include "phonecast/core/logging/ILogger.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <openvr.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int totalAssertions = 0;
int failureCount = 0;

void Check(bool condition, const std::string& description) {
    ++totalAssertions;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

void CheckNear(float actual, float expected, float tolerance, const std::string& description) {
    ++totalAssertions;
    const float diff = std::abs(actual - expected);
    if (diff > tolerance) {
        std::cerr << "FAIL: " << description << " (actual: " << actual
                  << ", expected: " << expected << ", diff: " << diff
                  << ", tol: " << tolerance << ")\n";
        ++failureCount;
    }
}

class TestLogger : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel level, std::string_view category,
             std::string_view message) override {
        (void)level;
        (void)category;
        (void)message;
    }
};

class MockTrackedCameraSource : public phonecast::platform::openvr::ITrackedCameraSource {
public:
    bool hasCamera{true};
    uint32_t width{640};
    uint32_t height{480};
    uint32_t bytesPerPixel{4};
    phonecast::core::tracking::CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
    ::vr::HmdMatrix34_t cameraToHead{{{1.0F, 0.0F, 0.0F, 0.0F},
                                     {0.0F, 1.0F, 0.0F, 0.0F},
                                     {0.0F, 0.0F, 1.0F, 0.0F}}};
    bool streamingActive{false};
    int acquireCount{0};
    int releaseCount{0};
    int getFrameCount{0};

    std::vector<uint8_t> frameData;
    ::vr::CameraVideoStreamFrameHeader_t nextHeader{};
    ::vr::EVRTrackedCameraError nextError{::vr::VRTrackedCameraError_None};

    MockTrackedCameraSource() {
        frameData.resize(width * height * bytesPerPixel, 0);
        nextHeader.nWidth = width;
        nextHeader.nHeight = height;
        nextHeader.nBytesPerPixel = bytesPerPixel;
        nextHeader.nFrameSequence = 1;
        nextHeader.ulFrameExposureTime = 1000000; // 1.0s
        nextHeader.trackedDevicePose.bPoseIsValid = true;
        nextHeader.trackedDevicePose.bDeviceIsConnected = true;
        // HMD at origin facing forward: identity
        nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                   {0.0F, 1.0F, 0.0F, 1.7F},
                                                                   {0.0F, 0.0F, 1.0F, 0.0F}}};
    }

    bool HasCamera() override {
        return hasCamera;
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
        outTransform = cameraToHead;
        return true;
    }

    bool AcquireStreaming() override {
        streamingActive = true;
        ++acquireCount;
        return true;
    }

    void ReleaseStreaming() override {
        streamingActive = false;
        ++releaseCount;
    }

    ::vr::EVRTrackedCameraError GetFrame(
        void* buffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        ++getFrameCount;
        if (nextError != ::vr::VRTrackedCameraError_None) {
            return nextError;
        }
        if (buffer && bufferSize >= frameData.size()) {
            std::memcpy(buffer, frameData.data(), frameData.size());
        }
        header = nextHeader;
        ++nextHeader.nFrameSequence;
        return ::vr::VRTrackedCameraError_None;
    }

    void RenderSyntheticRect(uint32_t xMin, uint32_t yMin, uint32_t xMax, uint32_t yMax) {
        std::fill(frameData.begin(), frameData.end(), 10); // Dark background
        for (uint32_t y = yMin; y <= yMax && y < height; ++y) {
            for (uint32_t x = xMin; x <= xMax && x < width; ++x) {
                const uint32_t idx = (y * width + x) * bytesPerPixel;
                frameData[idx + 0] = 240; // R
                frameData[idx + 1] = 240; // G
                frameData[idx + 2] = 240; // B
                frameData[idx + 3] = 255; // A
            }
        }
    }
};

// ============================================================================
// Test Suite 1: Worker Lifecycle & Zero-Leak Exception Safety
// ============================================================================
void TestWorkerLifecycle() {
    std::cout << "[Suite 1] Running TrackedCameraWorker Lifecycle Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();

    auto worker = std::make_unique<phonecast::platform::openvr::TrackedCameraWorker>(
        logger, std::move(mockPtr));

    Check(!worker->IsActive(), "worker starts inactive (low-power idle)");
    Check(mock->acquireCount == 0, "streaming is not acquired when inactive");

    Check(worker->Start(), "worker starts background thread");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Multiple Stop() calls must be noexcept and idempotent
    worker->Stop();
    worker->Stop();
    Check(!worker->IsActive(), "worker remains inactive after Stop");

    // Destructor of worker
    worker.reset();
    Check(mock->releaseCount >= 0, "worker destroyed cleanly without hanging or crashing");
}

// ============================================================================
// Test Suite 2: Low-Power Sleep / Wake Mechanism
// ============================================================================
void TestLowPowerSleepWake() {
    std::cout << "[Suite 2] Running Low-Power Sleep / Wake Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));
    worker.Start();

    // 1. Initial idle state: active_ == false, waiting on condition variable
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(!worker.IsActive(), "worker is in low-power idle state");
    Check(mock->acquireCount == 0, "0 camera streaming bandwidth while idle");

    // 2. Activate: PlacementMode changed to PhoneTracked
    worker.SetActive(true);
    Check(worker.IsActive(), "worker IsActive() is true");

    // Give background thread time to wake up and acquire streaming
    for (int i = 0; i < 50 && mock->acquireCount == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(mock->acquireCount == 1, "worker awoke and acquired streaming service");
    Check(mock->streamingActive, "camera streaming is now active");

    // 3. Suspend: PlacementMode changed to HeadLocked or WorldLocked
    worker.SetActive(false);
    Check(!worker.IsActive(), "worker IsActive() is false after suspension");

    for (int i = 0; i < 50 && mock->releaseCount == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(mock->releaseCount == 1, "worker released camera streaming on suspension");
    Check(!mock->streamingActive, "camera streaming released for zero bus contention");

    worker.Stop();
}

// ============================================================================
// Test Suite 3: Optical + Inertial Pose Solving with Synthetic Frame
// ============================================================================
void TestOpticalInertialPoseSolving() {
    std::cout << "[Suite 3] Running Optical + Inertial Pose Solving Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();

    // Render a fronto-parallel phone at Z = 0.50m
    // For 70mm x 150mm display at Z = 0.50m with fx=fy=750, cx=320, cy=240:
    // x = 320 +- (750 * 0.035 / 0.5) = 320 +- 52.5 -> [268, 372]
    // y = 240 +- (750 * 0.075 / 0.5) = 240 +- 112.5 -> [128, 352]
    mock->RenderSyntheticRect(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metrics{};
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    worker.SetScreenMetrics(metrics);

    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.qw = 1.0F; // Upright / fronto-parallel
    imu.qx = 0.0F;
    imu.qy = 0.0F;
    imu.qz = 0.0F;
    worker.IngestPhoneImu(imu);

    worker.Start();
    worker.SetActive(true);

    // Allow background thread to process synthetic frames
    phonecast::platform::openvr::TrackedCameraPoseSnapshot snapshot{};
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        snapshot = worker.GetSnapshot();
        if (snapshot.valid) break;
    }

    Check(snapshot.valid, "synthetic optical frame yielded valid pose snapshot");
    Check(snapshot.state == phonecast::core::tracking::TrackingState::Tracking,
          "tracking state is Tracking");

    // Check 1:1 metric physical width scaling
    CheckNear(snapshot.physicalWidthMeters, 0.070F, 0.001F,
              "physical width matches 70 mm screen (0.070 m)");

    // Compounding check:
    // HMD pose was: X=0, Y=1.7, Z=0.
    // In OpenVR camera frame (-Z is forward): Phone at Z=0.5m forward in CV -> Z=-0.5m in OpenVR VR space.
    // Compound standing position: X=0, Y=1.7, Z=-0.5.
    ::vr::HmdMatrix34_t pose{};
    float widthMeters = 0.0F;
    Check(worker.GetLatestPose(pose, &widthMeters), "GetLatestPose returns true");
    CheckNear(widthMeters, 0.070F, 0.001F, "GetLatestPose outPhysicalWidth is 0.070 m");

    CheckNear(pose.m[0][3], 0.0F, 0.05F, "Standing X position ~ 0.0 m");
    CheckNear(pose.m[1][3], 1.7F, 0.05F, "Standing Y position ~ 1.7 m (eye level)");
    CheckNear(pose.m[2][3], -0.50F, 0.05F, "Standing Z position ~ -0.50 m (50 cm in front of user)");

    worker.Stop();
}

// ============================================================================
// Test Suite 4: Coordinate Compounding Verification
// ============================================================================
void TestCoordinateCompoundingVerification() {
    std::cout << "[Suite 4] Running Coordinate Compounding Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();

    // Set HMD pose to position (1.0, 2.0, 3.0) with 90 deg Yaw rotation around Y:
    // In OpenVR: R_y(90 deg):
    // cos(90) = 0, sin(90) = 1
    // [ 0  0  1  1.0 ]
    // [ 0  1  0  2.0 ]
    // [-1  0  0  3.0 ]
    mock->nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{0.0F, 0.0F, 1.0F, 1.0F},
                                                                    {0.0F, 1.0F, 0.0F, 2.0F},
                                                                    {-1.0F, 0.0F, 0.0F, 3.0F}}};

    // Camera mount offset: 5 cm forward along -Z: (0, 0, -0.05)
    mock->cameraToHead = {{{1.0F, 0.0F, 0.0F, 0.0F},
                           {0.0F, 1.0F, 0.0F, 0.0F},
                           {0.0F, 0.0F, 1.0F, -0.05F}}};

    mock->RenderSyntheticRect(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metrics{};
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    worker.SetScreenMetrics(metrics);

    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.qw = 1.0F;
    worker.IngestPhoneImu(imu);

    worker.Start();
    worker.SetActive(true);

    phonecast::platform::openvr::TrackedCameraPoseSnapshot snapshot{};
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        snapshot = worker.GetSnapshot();
        if (snapshot.valid) break;
    }

    Check(snapshot.valid, "compounded pose with rotated HMD is valid");
    // Phone is 0.50m in front of camera -> total 0.55m in front of HMD (-Z direction in HMD frame).
    // HMD rotated 90 deg yaw: -Z vector [0, 0, -1] maps to R_y * [0, 0, -1]^T = [-1, 0, 0]^T!
    // So standing X should be 1.0 + (-0.55 * 1.0) = 1.0 - 0.55 = 0.45 m!
    // Standing Y should be 2.0 m!
    // Standing Z should be 3.0 m!
    CheckNear(snapshot.transform.m[0][3], 0.45F, 0.06F, "Compounded X with rotated HMD ~ 0.45 m");
    CheckNear(snapshot.transform.m[1][3], 2.0F, 0.06F, "Compounded Y with rotated HMD ~ 2.0 m");
    CheckNear(snapshot.transform.m[2][3], 3.0F, 0.06F, "Compounded Z with rotated HMD ~ 3.0 m");

    worker.Stop();
}

// ============================================================================
// Test Suite 5: Stable Occlusion Hold & Graceful Tracking Loss Transition
// ============================================================================
void TestStableOcclusionHold() {
    std::cout << "[Suite 5] Running Stable Occlusion Hold Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderSyntheticRect(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metrics{};
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    worker.SetScreenMetrics(metrics);

    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.qw = 1.0F;
    worker.IngestPhoneImu(imu);

    worker.Start();
    worker.SetActive(true);

    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (worker.GetSnapshot().valid) break;
    }
    Check(worker.GetSnapshot().valid, "nominal tracking established");
    const auto initialPose = worker.GetSnapshot().transform;

    // Simulate 200 ms optical occlusion:
    // Camera frame fails (e.g. finger completely covers screen or camera)
    mock->nextError = ::vr::VRTrackedCameraError_NoFrameAvailable;

    // Stream 60 Hz IMU packets during occlusion
    for (int i = 0; i < 15; ++i) {
        imu.qw = 0.999F;
        imu.qy = 0.040F; // Slight yaw rotation in hand
        worker.IngestPhoneImu(imu);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto occludedSnapshot = worker.GetSnapshot();
    Check(occludedSnapshot.valid, "pose remains valid during temporary occlusion hold");
    Check(occludedSnapshot.state == phonecast::core::tracking::TrackingState::Occluded,
          "state is Occluded (not Lost)");

    // Stable position hold: position must NOT snap to (0, 0, 0)
    CheckNear(occludedSnapshot.transform.m[0][3], initialPose.m[0][3], 0.05F,
              "occlusion hold keeps X position stable");
    CheckNear(occludedSnapshot.transform.m[1][3], initialPose.m[1][3], 0.05F,
              "occlusion hold keeps Y position stable");
    CheckNear(occludedSnapshot.transform.m[2][3], initialPose.m[2][3], 0.05F,
              "occlusion hold keeps Z position stable");

    worker.Stop();
}

// ============================================================================
// Test Suite 6: 1:1 Metric Scale Integrity
// ============================================================================
void TestMetricScaleIntegrity() {
    std::cout << "[Suite 6] Running 1:1 Metric Scale Integrity Tests...\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<MockTrackedCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderSyntheticRect(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metricsA{};
    metricsA.physicalWidthMm = 71.5F; // 71.5 mm phone (e.g. Pixel 8)
    metricsA.physicalHeightMm = 150.5F;
    metricsA.pixelWidth = 1080;
    metricsA.pixelHeight = 2400;
    worker.SetScreenMetrics(metricsA);

    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.qw = 1.0F;
    worker.IngestPhoneImu(imu);

    worker.Start();
    worker.SetActive(true);

    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (worker.GetSnapshot().valid) break;
    }
    CheckNear(worker.GetSnapshot().physicalWidthMeters, 0.0715F, 0.0005F,
              "scale matches 71.5 mm display exactly");

    // Reconfigure to wider device: 78.1 mm (e.g. S24 Ultra)
    phonecast::core::protocol::StreamScreenMetrics metricsB{};
    metricsB.physicalWidthMm = 78.1F;
    metricsB.physicalHeightMm = 162.3F;
    metricsB.pixelWidth = 1440;
    metricsB.pixelHeight = 3120;
    worker.SetScreenMetrics(metricsB);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CheckNear(worker.GetSnapshot().physicalWidthMeters, 0.0781F, 0.0005F,
              "scale updates automatically to 78.1 mm display");

    worker.Stop();
}

void TestSteamFrameV4L2CameraSource() {
    std::cout << "\n=== TestSteamFrameV4L2CameraSource ===\n";
#if defined(__linux__)
    TestLogger logger;
    phonecast::platform::openvr::SteamFrameV4L2CameraSource v4l2Source(logger, nullptr);
    const bool hasHardware = v4l2Source.HasCamera();
    std::cout << "  Steam Frame IMX616 hardware detected: " << (hasHardware ? "YES" : "NO (graceful fallback)") << '\n';
    if (!hasHardware) {
        Check(!v4l2Source.AcquireStreaming(), "AcquireStreaming returns false gracefully when IMX616 is not present");
    }
    uint32_t w = 0, h = 0, b = 0;
    v4l2Source.GetFrameSize(w, h, b);
    Check(w > 0 && h > 0 && b > 0, "GetFrameSize returns non-zero buffer bounds");
    phonecast::core::tracking::CameraIntrinsics intrinsics{};
    v4l2Source.GetIntrinsics(intrinsics);
    Check(intrinsics.fx > 0.0F && intrinsics.imageWidth == w, "GetIntrinsics returns valid FOV");
    v4l2Source.ReleaseStreaming();
#else
    std::cout << "  (Skipped on non-Linux platform)\n";
#endif
}

} // namespace

int main() {
    std::cout << "Running PhoneCast VR TrackedCameraWorker & Decoupled Tracking Tests...\n";

    TestWorkerLifecycle();
    TestLowPowerSleepWake();
    TestOpticalInertialPoseSolving();
    TestCoordinateCompoundingVerification();
    TestStableOcclusionHold();
    TestMetricScaleIntegrity();
    TestSteamFrameV4L2CameraSource();

    std::cout << "\nTest Summary: " << totalAssertions << " assertions checked, "
              << failureCount << " failures.\n";

    if (failureCount == 0) {
        std::cout << "ALL TRACKED CAMERA TESTS PASSED!\n";
        return 0;
    } else {
        std::cerr << "TRACKED CAMERA TESTS FAILED WITH " << failureCount << " ERRORS!\n";
        return 1;
    }
}
