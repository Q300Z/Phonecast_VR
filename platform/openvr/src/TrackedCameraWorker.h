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

    std::vector<uint8_t> frameBuffer_;
    uint32_t frameWidth_{0};
    uint32_t frameHeight_{0};
    uint32_t frameBufferSize_{0};
    uint32_t lastSequence_{0};
};

} // namespace phonecast::platform::openvr
