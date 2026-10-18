#pragma once

#include "ITrackedCameraSource.h"
#include "phonecast/core/logging/ILogger.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/tracking/CornerIntersection.h"
#include "phonecast/core/tracking/InertialPoseFusion.h"
#include "phonecast/core/tracking/LineFitterRansac.h"
#include "phonecast/core/tracking/PlanarPnpSolver.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <openvr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace phonecast::platform::openvr {

struct TrackedCameraPoseSnapshot {
    bool valid{false};
    core::tracking::TrackingState state{core::tracking::TrackingState::Lost};
    ::vr::HmdMatrix34_t transform{}; // T_{Standing <- Phone}
    float physicalWidthMeters{0.0F};
    float confidence{0.0F};
    std::uint64_t sequence{0};
    std::chrono::steady_clock::time_point timestamp{};
};

struct CameraFrameSnapshot {
    bool valid{false};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgbaBuffer;
    core::tracking::ScreenCorners2D corners{};
    std::uint64_t sequence{0};
    core::tracking::TrackingState state{core::tracking::TrackingState::Lost};
    float confidence{1.0F};
};

struct TrackedCameraDebugInfo {
    bool active{false};
    bool streamingAcquired{false};
    char sourceName[64]{"none"};
    char cameraError[64]{""};
    uint32_t frameWidth{0};
    uint32_t frameHeight{0};
    uint32_t frameSequence{0};
    float fps{0.0F};

    // Optical pipeline stats
    uint8_t peakLuma{0};
    uint8_t lumaThreshold{0};
    uint32_t brightPixelCount{0};
    bool borderTop{false};
    bool borderRight{false};
    bool borderBottom{false};
    bool borderLeft{false};
    bool cornersValid{false};

    // Tracking state & pose
    core::tracking::TrackingState state{core::tracking::TrackingState::Lost};
    bool poseValid{false};
    float confidence{0.0F};
    float physicalWidthMeters{0.0F};
    float physicalHeightMeters{0.0F};

    // Standing space coordinates
    float standingX{0.0F};
    float standingY{0.0F};
    float standingZ{0.0F};
    float hmdDistanceMeters{0.0F};
    float hmdRelativeX{0.0F};
    float hmdRelativeY{0.0F};
    float hmdRelativeZ{0.0F};
    float pitchDeg{0.0F};
    float yawDeg{0.0F};
    float rollDeg{0.0F};

    // Phone telemetry
    bool hasImu{false};
    uint32_t imuPacketsReceived{0};
    bool hasMetrics{false};
    float metricsWidthMm{0.0F};
    float metricsHeightMm{0.0F};
};

class TrackedCameraWorker final {
public:
    // Production constructor using OpenVR IVRTrackedCamera API
    TrackedCameraWorker(core::ILogger& logger, ::vr::IVRSystem* system, ::vr::IVRTrackedCamera* cameraApi);
    // Dependency injection constructor for automated testing with simulated camera feeds
    TrackedCameraWorker(core::ILogger& logger, std::unique_ptr<ITrackedCameraSource> cameraSource);
    ~TrackedCameraWorker();

    TrackedCameraWorker(const TrackedCameraWorker&) = delete;
    TrackedCameraWorker& operator=(const TrackedCameraWorker&) = delete;

    bool Start();
    void SetActive(bool active);
    [[nodiscard]] bool IsActive() const noexcept;

    void SetScreenMetrics(const core::protocol::StreamScreenMetrics& metrics);
    void IngestPhoneImu(const core::protocol::StreamPhoneImu& imu);

    // Lock-free read of latest pose snapshot for render thread (< 15 ns)
    bool GetLatestPose(::vr::HmdMatrix34_t& outTransform, float* outPhysicalWidth = nullptr);
    TrackedCameraPoseSnapshot GetSnapshot();
    bool GetLatestCameraFrame(CameraFrameSnapshot& outSnapshot);
    TrackedCameraDebugInfo GetDebugInfo() const;

    void Stop() noexcept;

    // Process a single frame directly (used by worker loop and headless test harnesses)
    void ProcessFrame(const uint8_t* buffer, const ::vr::CameraVideoStreamFrameHeader_t& header);

private:
    void WorkerLoop();
    bool DetectScreenCorners(const core::tracking::CameraFrameView& frame,
                            core::tracking::ScreenCorners2D& outCorners);

    core::ILogger& logger_;
    std::unique_ptr<ITrackedCameraSource> cameraSource_;

    // Concurrency control
    std::thread workerThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;

    // Thread-safe snapshot output
    mutable std::mutex snapshotMutex_;
    TrackedCameraPoseSnapshot latestSnapshot_{};
    CameraFrameSnapshot latestFrameSnapshot_{};
    TrackedCameraDebugInfo latestDebugInfo_{};
    uint32_t totalImuPackets_{0};
    uint32_t fpsFrameCount_{0};
    float currentFps_{0.0F};
    std::chrono::steady_clock::time_point lastFpsTime_{};

    // Tracking pipeline
    core::tracking::LineFitterRansac lineFitter_;
    core::tracking::PlanarPnpSolver pnpSolver_;
    core::tracking::InertialPoseFusion fusion_;

    core::tracking::CameraIntrinsics intrinsics_{};
    core::tracking::ScreenMetrics screenMetrics_{};
    ::vr::HmdMatrix34_t cameraToHeadTransform_{}; // T_{HMD <- Camera_VR}

    std::mutex dataMutex_;
    std::deque<core::tracking::PhoneImuPacket> pendingImu_;
    core::tracking::PhoneImuPacket latestImu_{};
    bool hasImu_{false};
    bool hasMetrics_{false};

    core::tracking::ScreenCorners2D priorCorners_{};
    core::tracking::ScreenCorners2D lastValidCorners_{};
    uint32_t occlusionFrameCount_{0};

    std::vector<uint8_t> frameBuffer_;
    uint32_t frameWidth_{0};
    uint32_t frameHeight_{0};
    uint32_t frameBufferSize_{0};
    uint32_t lastSequence_{0};
};

} // namespace phonecast::platform::openvr
