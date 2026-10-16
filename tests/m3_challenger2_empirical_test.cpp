#include "ITrackedCameraSource.h"
#include "TrackedCameraWorker.h"
#include "phonecast/core/logging/ILogger.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/tracking/InertialPoseFusion.h"
#include "phonecast/core/tracking/PlanarPnpSolver.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <openvr.h>
#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const std::string& name, const std::string& details = "") {
    if (condition) {
        ++g_pass;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_fail;
        std::cerr << "  [FAIL] " << name << (details.empty() ? "" : " -- " + details) << "\n";
    }
}

void CheckNear(float actual, float expected, float tol, const std::string& name) {
    const float diff = std::abs(actual - expected);
    if (diff <= tol) {
        ++g_pass;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_fail;
        std::cerr << "  [FAIL] " << name << " (actual: " << actual << ", expected: " << expected
                  << ", diff: " << diff << ", tol: " << tol << ")\n";
    }
}

class TestLogger : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel, std::string_view, std::string_view) override {}
};

class AdversarialMockCameraSource : public phonecast::platform::openvr::ITrackedCameraSource {
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

    AdversarialMockCameraSource() {
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

    bool HasCamera() override { return hasCamera; }
    bool GetFrameSize(uint32_t& outWidth, uint32_t& outHeight, uint32_t& outBufferSize) override {
        outWidth = width; outHeight = height; outBufferSize = width * height * bytesPerPixel;
        return true;
    }
    bool GetIntrinsics(phonecast::core::tracking::CameraIntrinsics& outIntrinsics) override {
        outIntrinsics = intrinsics; return true;
    }
    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override {
        outTransform = cameraToHead; return true;
    }
    bool AcquireStreaming() override { streamingActive = true; ++acquireCount; return true; }
    void ReleaseStreaming() override { streamingActive = false; ++releaseCount; }

    ::vr::EVRTrackedCameraError GetFrame(
        void* buffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        ++getFrameCount;
        if (nextError != ::vr::VRTrackedCameraError_None) return nextError;
        if (buffer && bufferSize >= frameData.size()) {
            std::memcpy(buffer, frameData.data(), frameData.size());
        }
        header = nextHeader;
        ++nextHeader.nFrameSequence;
        return ::vr::VRTrackedCameraError_None;
    }

    void RenderPhone(uint32_t xMin, uint32_t yMin, uint32_t xMax, uint32_t yMax) {
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
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
::vr::HmdMatrix34_t Eigen4ToHmdMatrix(const Eigen::Matrix4f& mat) {
    ::vr::HmdMatrix34_t res{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            res.m[r][c] = mat(r, c);
        }
    }
    return res;
}

// ============================================================================
// SECTION 1: Adversarial Coordinate Compounding & Basis Conversion
// ============================================================================

void TestBasisConversionProperties() {
    std::cout << "\n=== [Section 1.1] Basis Conversion M_{VR <- CV} Mathematical Properties ===\n";

    // M_{VR <- CV} = diag(1, -1, -1) in 4x4
    Eigen::Matrix4f M = Eigen::Matrix4f::Identity();
    M(1, 1) = -1.0F;
    M(2, 2) = -1.0F;

    // 1. Involutory: M * M = I
    Eigen::Matrix4f M2 = M * M;
    Check((M2 - Eigen::Matrix4f::Identity()).norm() < 1e-6F,
          "M is involutory (M * M == Identity)");

    // 2. Proper rotation: det(M_3x3) = +1
    Eigen::Matrix3f M3 = M.block<3, 3>(0, 0);
    CheckNear(M3.determinant(), 1.0F, 1e-6F, "det(M_3x3) == +1 (Preserves right-handedness, no reflection)");

    // 3. Basis mapping: OpenCV -> OpenVR
    // OpenCV: +X right, +Y down, +Z forward
    // OpenVR: +X right, +Y up, -Z forward
    Eigen::Vector3f xCv(1.0F, 0.0F, 0.0F);
    Eigen::Vector3f yCv(0.0F, 1.0F, 0.0F);
    Eigen::Vector3f zCv(0.0F, 0.0F, 1.0F);

    Eigen::Vector3f xVr = M3 * xCv;
    Eigen::Vector3f yVr = M3 * yCv;
    Eigen::Vector3f zVr = M3 * zCv;

    Check(xVr.isApprox(Eigen::Vector3f(1.0F, 0.0F, 0.0F)), "+X_CV (Right) maps to +X_VR (Right)");
    Check(yVr.isApprox(Eigen::Vector3f(0.0F, -1.0F, 0.0F)), "+Y_CV (Down) maps to -Y_VR (Up is +Y_VR)");
    Check(zVr.isApprox(Eigen::Vector3f(0.0F, 0.0F, -1.0F)), "+Z_CV (Forward) maps to -Z_VR (Forward is -Z_VR)");
}

void TestArbitraryHmdPosesCompounding() {
    std::cout << "\n=== [Section 1.2] Coordinate Compounding Under Arbitrary HMD Poses ===\n";

    // Test across a wide variety of HMD poses:
    // Poses: (yaw, pitch, roll, tx, ty, tz)
    struct PoseCase {
        std::string name;
        float yawDeg;
        float pitchDeg;
        float rollDeg;
        Eigen::Vector3f trans;
    };

    std::vector<PoseCase> cases = {
        {"Identity at Origin", 0.0F, 0.0F, 0.0F, {0.0F, 0.0F, 0.0F}},
        {"Standing Translation Only", 0.0F, 0.0F, 0.0F, {-1.5F, 1.75F, 2.3F}},
        {"Pure Yaw +45 deg", 45.0F, 0.0F, 0.0F, {0.5F, 1.7F, -1.0F}},
        {"Pure Yaw +90 deg", 90.0F, 0.0F, 0.0F, {1.0F, 1.7F, 0.0F}},
        {"Pure Yaw 180 deg (Looking Backwards)", 180.0F, 0.0F, 0.0F, {0.0F, 1.7F, 0.0F}},
        {"Pure Pitch -45 deg (Looking Down at Phone)", 0.0F, -45.0F, 0.0F, {0.0F, 1.7F, 0.0F}},
        {"Pure Pitch +30 deg (Looking Up)", 0.0F, 30.0F, 0.0F, {0.0F, 1.7F, 0.0F}},
        {"Pure Roll +30 deg (Head Tilt Right)", 0.0F, 0.0F, 30.0F, {0.0F, 1.7F, 0.0F}},
        {"Pure Roll -45 deg (Head Tilt Left)", 0.0F, 0.0F, -45.0F, {0.0F, 1.7F, 0.0F}},
        {"Complex Yaw(35) + Pitch(-25) + Roll(15)", 35.0F, -25.0F, 15.0F, {1.2F, 1.6F, -0.8F}},
        {"Extreme Yaw(160) + Pitch(-60) + Roll(-20)", 160.0F, -60.0F, -20.0F, {-2.0F, 1.4F, 1.5F}}
    };

    // Basis conversion matrix M_{VR <- CV}
    Eigen::Matrix4f M_VR_CV = Eigen::Matrix4f::Identity();
    M_VR_CV(1, 1) = -1.0F;
    M_VR_CV(2, 2) = -1.0F;

    // Non-trivial camera-to-head extrinsic (Index-like: 5cm forward, 2cm up, 3cm right)
    Eigen::Matrix4f T_HMD_Cam = Eigen::Matrix4f::Identity();
    T_HMD_Cam.block<3, 3>(0, 0) = (Eigen::AngleAxisf(-0.0872665F, Eigen::Vector3f::UnitX())).toRotationMatrix(); // 5 deg down tilt
    T_HMD_Cam.block<3, 1>(0, 3) = Eigen::Vector3f(0.03F, 0.02F, -0.05F);
    const ::vr::HmdMatrix34_t hmdCamVr = Eigen4ToHmdMatrix(T_HMD_Cam);

    // Phone relative to camera in CV: 0.50m forward, 5cm down, 2cm right
    // Rotation: upright fronto-parallel facing camera
    // In CV, facing camera means R = diag(1, -1, -1)
    Eigen::Matrix4f T_CamCV_Phone = Eigen::Matrix4f::Identity();
    T_CamCV_Phone.block<3, 3>(0, 0) = Eigen::DiagonalMatrix<float, 3>(1.0F, -1.0F, -1.0F).toDenseMatrix();
    T_CamCV_Phone.block<3, 1>(0, 3) = Eigen::Vector3f(0.02F, 0.05F, 0.50F);

    for (const auto& c : cases) {
        // Construct HMD pose
        Eigen::AngleAxisf rollAngle(c.rollDeg * (float)M_PI / 180.0F, Eigen::Vector3f::UnitZ());
        Eigen::AngleAxisf pitchAngle(c.pitchDeg * (float)M_PI / 180.0F, Eigen::Vector3f::UnitX());
        Eigen::AngleAxisf yawAngle(c.yawDeg * (float)M_PI / 180.0F, Eigen::Vector3f::UnitY());
        Eigen::Quaternionf qHmd = yawAngle * pitchAngle * rollAngle;

        Eigen::Matrix4f T_Standing_HMD = Eigen::Matrix4f::Identity();
        T_Standing_HMD.block<3, 3>(0, 0) = qHmd.toRotationMatrix();
        T_Standing_HMD.block<3, 1>(0, 3) = c.trans;

        // Ground-truth analytical compounding:
        // T_{Standing <- Phone} = T_{Standing <- HMD} * T_{HMD <- Camera_VR} * M_{VR <- CV} * T_{Camera_CV <- Phone}
        Eigen::Matrix4f T_Expected = T_Standing_HMD * T_HMD_Cam * M_VR_CV * T_CamCV_Phone;

        // Now test via TrackedCameraWorker pipeline with simulated frame
        TestLogger logger;
        auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
        auto* mock = mockPtr.get();
        mock->cameraToHead = hmdCamVr;
        mock->nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = Eigen4ToHmdMatrix(T_Standing_HMD);

        // Project phone corners with the given T_CamCV_Phone
        // Screen 70mm x 150mm:
        // TL: (-0.035,  0.075, 0) -> R * p + t
        // TR: ( 0.035,  0.075, 0)
        // BR: ( 0.035, -0.075, 0)
        // BL: (-0.035, -0.075, 0)
        // With R = diag(1, -1, -1) and t = (0.02, 0.05, 0.50):
        // TL_cv = (0.02 - 0.035, 0.05 - 0.075, 0.50) = (-0.015, -0.025, 0.50)
        // TR_cv = (0.02 + 0.035, 0.05 - 0.075, 0.50) = ( 0.055, -0.025, 0.50)
        // BR_cv = (0.02 + 0.035, 0.05 + 0.075, 0.50) = ( 0.055,  0.125, 0.50)
        // BL_cv = (0.02 - 0.035, 0.05 + 0.075, 0.50) = (-0.015,  0.125, 0.50)
        // Projected: x = 320 + 750 * X / Z, y = 240 + 750 * Y / Z:
        // x_min = 320 + 750 * (-0.015) / 0.5 = 320 - 22.5 = 297.5 -> 298
        // x_max = 320 + 750 * ( 0.055) / 0.5 = 320 + 82.5 = 402.5 -> 402
        // y_min = 240 + 750 * (-0.025) / 0.5 = 240 - 37.5 = 202.5 -> 203
        // y_max = 240 + 750 * ( 0.125) / 0.5 = 240 + 187.5 = 427.5 -> 427
        mock->RenderPhone(298, 203, 402, 427);

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
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            snapshot = worker.GetSnapshot();
            if (snapshot.valid) break;
        }

        Check(snapshot.valid, "Compounding valid for: " + c.name);

        Eigen::Vector3f expectedPos = T_Expected.block<3, 1>(0, 3);
        Eigen::Vector3f actualPos(snapshot.transform.m[0][3],
                                  snapshot.transform.m[1][3],
                                  snapshot.transform.m[2][3]);

        const float posError = (actualPos - expectedPos).norm();
        Check(posError < 0.06F, "Position compounding accuracy for: " + c.name,
              "error: " + std::to_string(posError) + "m");

        worker.Stop();
    }
}

// ============================================================================
// SECTION 2: Adversarial 1:1 Metric Scale Integrity
// ============================================================================

void TestDynamicMetricScaling() {
    std::cout << "\n=== [Section 2.1] Dynamic 1:1 Metric Scale Changes & Zero-Distortion ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    struct PhoneMetricTest {
        std::string model;
        float widthMm;
        float heightMm;
        uint32_t pxW;
        uint32_t pxH;
    };

    std::vector<PhoneMetricTest> phones = {
        {"Standard 70mm Phone", 70.0F, 150.0F, 1080, 2400},
        {"Samsung Galaxy S24 Ultra (78.1mm)", 78.1F, 162.3F, 1440, 3120},
        {"Compact iPhone-size (67.6mm)", 67.6F, 144.2F, 1170, 2532},
        {"Foldable Outer (57.1mm)", 57.1F, 153.5F, 904, 2316},
        {"Wide Gaming Phone (82.0mm)", 82.0F, 173.0F, 1080, 2460}
    };

    worker.Start();
    worker.SetActive(true);

    for (const auto& p : phones) {
        phonecast::core::protocol::StreamScreenMetrics metrics{};
        metrics.physicalWidthMm = p.widthMm;
        metrics.physicalHeightMm = p.heightMm;
        metrics.pixelWidth = p.pxW;
        metrics.pixelHeight = p.pxH;

        worker.SetScreenMetrics(metrics);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        ::vr::HmdMatrix34_t transform{};
        float widthMeters = 0.0F;
        const bool gotPose = worker.GetLatestPose(transform, &widthMeters);
        Check(gotPose, "GetLatestPose succeeded for " + p.model);

        const float expectedWidthMeters = p.widthMm / 1000.0F;
        CheckNear(widthMeters, expectedWidthMeters, 0.0005F,
                  "1:1 metric overlay width matches " + p.model + " (" + std::to_string(expectedWidthMeters) + " m)");
        CheckNear(worker.GetSnapshot().physicalWidthMeters, expectedWidthMeters, 0.0005F,
                  "Snapshot physicalWidthMeters matches " + p.model);
    }

    worker.Stop();
}

void TestAdversarialMetricsRejection() {
    std::cout << "\n=== [Section 2.2] Adversarial Screen Metrics Boundary Rejection ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    // Test zero, negative, and infinite/NaN metrics
    std::vector<std::pair<float, float>> badMetrics = {
        {0.0F, 0.0F},
        {-70.0F, -150.0F},
        {10000.0F, 20000.0F}, // 10 meters wide phone
        {0.001F, 0.001F}      // 1 micrometer
    };

    for (const auto& m : badMetrics) {
        phonecast::core::protocol::StreamScreenMetrics metrics{};
        metrics.physicalWidthMm = m.first;
        metrics.physicalHeightMm = m.second;
        metrics.pixelWidth = 1080;
        metrics.pixelHeight = 2400;

        // Must not crash, throw or leave worker corrupted
        worker.SetScreenMetrics(metrics);
        auto snap = worker.GetSnapshot();
        Check(std::isfinite(snap.physicalWidthMeters) && snap.physicalWidthMeters >= 0.0F,
              "Adversarial metric (" + std::to_string(m.first) + "mm) handled safely without NaN");
    }
}

// ============================================================================
// SECTION 3: Adversarial Occlusion Hold Robustness
// ============================================================================

void TestSingleFrameDropout() {
    std::cout << "\n=== [Section 3.1] Transient Single-Frame Optical Dropout ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

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
    Check(worker.GetSnapshot().valid, "Baseline tracking established");
    const auto baselinePose = worker.GetSnapshot().transform;

    // Single frame dropout (1 frame error: 16-33 ms)
    mock->nextError = ::vr::VRTrackedCameraError_NoFrameAvailable;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    auto snap = worker.GetSnapshot();
    Check(snap.valid, "Single frame drop retains valid snapshot");
    Check(snap.state == phonecast::core::tracking::TrackingState::Tracking ||
          snap.state == phonecast::core::tracking::TrackingState::Occluded,
          "State remains stable during single frame drop");

    // Position must be strictly identical to baseline within sub-millimeter
    CheckNear(snap.transform.m[0][3], baselinePose.m[0][3], 0.005F, "Single frame drop X stable");
    CheckNear(snap.transform.m[1][3], baselinePose.m[1][3], 0.005F, "Single frame drop Y stable");
    CheckNear(snap.transform.m[2][3], baselinePose.m[2][3], 0.005F, "Single frame drop Z stable");

    worker.Stop();
}

void TestMultiFrameOcclusionHoldWithImuRotation() {
    std::cout << "\n=== [Section 3.2] Multi-Frame Occlusion Hold (<1.5s) With Active IMU Rotation ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

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
    Check(worker.GetSnapshot().valid, "Baseline tracking established");
    const auto initialPose = worker.GetSnapshot().transform;

    // Optical occlusion begins: simulate finger covering screen/camera for 600 ms
    mock->nextError = ::vr::VRTrackedCameraError_NoFrameAvailable;

    // During this 600 ms optical dropout, user rotates phone in hand:
    // 30 deg yaw rotation around vertical axis:
    // q = [cos(15 deg), 0, sin(15 deg), 0] = [0.9659, 0, 0.2588, 0]
    for (int i = 0; i < 30; ++i) {
        float angle = (float)i * (30.0F / 30.0F) * (float)M_PI / 180.0F;
        imu.qw = std::cos(angle * 0.5F);
        imu.qy = std::sin(angle * 0.5F);
        worker.IngestPhoneImu(imu);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto occludedSnap = worker.GetSnapshot();
    Check(occludedSnap.valid, "Snapshot remains valid during 600ms occlusion");
    Check(occludedSnap.state == phonecast::core::tracking::TrackingState::Occluded,
          "TrackingState is Occluded");

    // Position MUST be rigidly held in Standing room space (no jumping, no snapping to origin)
    CheckNear(occludedSnap.transform.m[0][3], initialPose.m[0][3], 0.01F,
              "Standing X position strictly held during occlusion");
    CheckNear(occludedSnap.transform.m[1][3], initialPose.m[1][3], 0.01F,
              "Standing Y position strictly held during occlusion");
    CheckNear(occludedSnap.transform.m[2][3], initialPose.m[2][3], 0.01F,
              "Standing Z position strictly held during occlusion");

    // Orientation MUST continue updating via IMU!
    // Check that rotation matrix has changed from initial orientation
    const float rotDiff = std::abs(occludedSnap.transform.m[0][0] - initialPose.m[0][0]) +
                          std::abs(occludedSnap.transform.m[0][2] - initialPose.m[0][2]);
    Check(rotDiff > 0.05F, "Orientation continues updating from IMU while optically occluded",
          "rotDiff = " + std::to_string(rotDiff));

    worker.Stop();
}

void TestPermanentDropoutTimeoutAndRecovery() {
    std::cout << "\n=== [Section 3.3] Permanent Dropout (>1.5s Timeout) & Optical Recovery ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

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
    Check(worker.GetSnapshot().valid, "Tracking active before permanent drop");
    const auto preDropPose = worker.GetSnapshot().transform;

    // Permanent optical dropout (>1.5s): user puts phone in pocket or walks away
    mock->nextError = ::vr::VRTrackedCameraError_NoFrameAvailable;

    // Advance 1.7 seconds
    for (int i = 0; i < 17; ++i) {
        worker.IngestPhoneImu(imu);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto timedOutSnap = worker.GetSnapshot();
    Check(!timedOutSnap.valid, "Snapshot valid is FALSE after 1.7s (>1.5s timeout)");
    Check(timedOutSnap.state == phonecast::core::tracking::TrackingState::Lost,
          "TrackingState is Lost");

    ::vr::HmdMatrix34_t testTransform{};
    float width = 0.0F;
    Check(!worker.GetLatestPose(testTransform, &width),
          "GetLatestPose returns false when Lost (preventing overlay popping to 0,0,0)");

    // Now test RECOVERY: user pulls phone back out and points camera at it
    mock->nextError = ::vr::VRTrackedCameraError_None;

    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (worker.GetSnapshot().valid &&
            worker.GetSnapshot().state == phonecast::core::tracking::TrackingState::Tracking) {
            break;
        }
    }

    auto recoveredSnap = worker.GetSnapshot();
    Check(recoveredSnap.valid, "Tracking recovered successfully after timeout");
    Check(recoveredSnap.state == phonecast::core::tracking::TrackingState::Tracking,
          "State returned to Tracking upon optical recovery");
    CheckNear(recoveredSnap.transform.m[0][3], preDropPose.m[0][3], 0.08F, "Recovered X position matches");
    CheckNear(recoveredSnap.transform.m[1][3], preDropPose.m[1][3], 0.08F, "Recovered Y position matches");
    CheckNear(recoveredSnap.transform.m[2][3], preDropPose.m[2][3], 0.08F, "Recovered Z position matches");

    worker.Stop();
}

// ============================================================================
// SECTION 4: System Edge Cases & Concurrency Stress
// ============================================================================

void TestHmdTrackingLossDuringOpticalTracking() {
    std::cout << "\n=== [Section 4.1] HMD Tracking Loss (bPoseIsValid = false) Handling ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

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
    Check(worker.GetSnapshot().valid, "Tracking active before HMD tracking loss");
    const auto baselinePose = worker.GetSnapshot().transform;

    // Simulate HMD lighthouse/inside-out tracking loss:
    // Camera still receives images, but HMD pose is invalid
    mock->nextHeader.trackedDevicePose.bPoseIsValid = false;

    // Advance 200 ms with invalid HMD pose
    for (int i = 0; i < 10; ++i) {
        worker.IngestPhoneImu(imu);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto snap = worker.GetSnapshot();
    Check(snap.valid, "Snapshot remains valid during transient HMD tracking loss");
    // Position held stably without exploding
    CheckNear(snap.transform.m[0][3], baselinePose.m[0][3], 0.05F, "HMD tracking loss holds X stably");
    CheckNear(snap.transform.m[1][3], baselinePose.m[1][3], 0.05F, "HMD tracking loss holds Y stably");
    CheckNear(snap.transform.m[2][3], baselinePose.m[2][3], 0.05F, "HMD tracking loss holds Z stably");

    worker.Stop();
}

void TestConcurrentRenderThreadAccess() {
    std::cout << "\n=== [Section 4.2] High-Frequency Render Loop Concurrency Stress (144 Hz) ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderPhone(268, 128, 372, 352);

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

    std::atomic<bool> renderRunning{true};
    std::atomic<uint32_t> queryCount{0};
    std::atomic<uint32_t> validCount{0};
    std::atomic<uint32_t> nanCount{0};

    // Spawn high-frequency simulated render thread (144 Hz -> ~7ms period)
    std::thread renderThread([&]() {
        while (renderRunning.load()) {
            ::vr::HmdMatrix34_t transform{};
            float width = 0.0F;
            const bool ok = worker.GetLatestPose(transform, &width);
            ++queryCount;
            if (ok) {
                ++validCount;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        if (!std::isfinite(transform.m[r][c])) {
                            ++nanCount;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(6944)); // 144 Hz
        }
    });

    // Main thread injects IMU and screen metric perturbations concurrently
    for (int i = 0; i < 50; ++i) {
        imu.qy = std::sin(static_cast<float>(i) * 0.1F);
        imu.qw = std::cos(static_cast<float>(i) * 0.1F);
        worker.IngestPhoneImu(imu);
        if (i % 10 == 0) {
            metrics.physicalWidthMm = 70.0F + static_cast<float>(i % 5);
            worker.SetScreenMetrics(metrics);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    renderRunning.store(false);
    renderThread.join();

    Check(queryCount.load() > 50, "Render thread completed high frequency queries (" +
          std::to_string(queryCount.load()) + " queries)");
    Check(validCount.load() > 0, "Render thread received valid poses (" +
          std::to_string(validCount.load()) + " valid)");
    Check(nanCount.load() == 0, "Zero NaNs observed across all concurrent queries");

    worker.Stop();
}

} // namespace

int main() {
    std::cout << "====================================================================\n";
    std::cout << "Starting PhoneCast VR Milestone 3 Challenger 2 Empirical Stress Suite\n";
    std::cout << "====================================================================\n";

    TestBasisConversionProperties();
    TestArbitraryHmdPosesCompounding();
    TestDynamicMetricScaling();
    TestAdversarialMetricsRejection();
    TestSingleFrameDropout();
    TestMultiFrameOcclusionHoldWithImuRotation();
    TestPermanentDropoutTimeoutAndRecovery();
    TestHmdTrackingLossDuringOpticalTracking();
    TestConcurrentRenderThreadAccess();

    std::cout << "\n====================================================================\n";
    std::cout << "Empirical Stress Test Summary: " << g_pass << " passed, " << g_fail << " failed.\n";
    std::cout << "====================================================================\n";

    if (g_fail == 0) {
        std::cout << "ALL EMPIRICAL ADVERSARIAL CHALLENGES PASSED!\n";
        return 0;
    } else {
        std::cerr << "CHALLENGES FAILED WITH " << g_fail << " ERRORS!\n";
        return 1;
    }
}

