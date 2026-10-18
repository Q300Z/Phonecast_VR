#include "TrackedCameraWorker.h"
#if defined(__linux__)
#include "SteamFrameV4L2CameraSource.h"
#endif

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>

namespace phonecast::platform::openvr {
namespace {

vr::HmdMatrix34_t Multiply(const vr::HmdMatrix34_t& left, const vr::HmdMatrix34_t& right) {
    vr::HmdMatrix34_t result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t inner = 0; inner < 3; ++inner)
                result.m[row][column] += left.m[row][inner] * right.m[inner][column];
        }
        result.m[row][3] = left.m[row][3];
        for (std::size_t inner = 0; inner < 3; ++inner)
            result.m[row][3] += left.m[row][inner] * right.m[inner][3];
    }
    return result;
}

vr::HmdMatrix34_t Pose6DToHmdMatrix(const core::tracking::Pose6D& pose) {
    const auto& q = pose.orientation;
    const auto& p = pose.position;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = 1.0F - 2.0F * (q.y * q.y + q.z * q.z);
    m.m[0][1] = 2.0F * (q.x * q.y - q.w * q.z);
    m.m[0][2] = 2.0F * (q.x * q.z + q.w * q.y);
    m.m[0][3] = p.x;

    m.m[1][0] = 2.0F * (q.x * q.y + q.w * q.z);
    m.m[1][1] = 1.0F - 2.0F * (q.x * q.x + q.z * q.z);
    m.m[1][2] = 2.0F * (q.y * q.z - q.w * q.x);
    m.m[1][3] = p.y;

    m.m[2][0] = 2.0F * (q.x * q.z - q.w * q.y);
    m.m[2][1] = 2.0F * (q.y * q.z + q.w * q.x);
    m.m[2][2] = 1.0F - 2.0F * (q.x * q.x + q.y * q.y);
    m.m[2][3] = p.z;
    return m;
}

core::tracking::Pose6D HmdMatrixToPose6D(const vr::HmdMatrix34_t& m) {
    core::tracking::Pose6D pose{};
    pose.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
    Eigen::Matrix3f R;
    R << m.m[0][0], m.m[0][1], m.m[0][2],
         m.m[1][0], m.m[1][1], m.m[1][2],
         m.m[2][0], m.m[2][1], m.m[2][2];
    Eigen::Quaternionf q(R);
    q.normalize();
    pose.orientation = {q.w(), q.x(), q.y(), q.z()};
    pose.valid = true;
    return pose;
}

vr::HmdMatrix34_t ApplyM_VR_CV(const vr::HmdMatrix34_t& cvMatrix) {
    vr::HmdMatrix34_t vrMatrix = cvMatrix;
    for (int col = 0; col < 4; ++col) {
        vrMatrix.m[1][col] = -vrMatrix.m[1][col];
        vrMatrix.m[2][col] = -vrMatrix.m[2][col];
    }
    return vrMatrix;
}

class OpenVrTrackedCameraSource : public ITrackedCameraSource {
public:
    OpenVrTrackedCameraSource(::vr::IVRSystem* system, ::vr::IVRTrackedCamera* cameraApi)
        : system_(system), cameraApi_(cameraApi) {}

    ~OpenVrTrackedCameraSource() override {
        ReleaseStreaming();
    }

    const char* GetSourceName() const override { return "OpenVR IVRTrackedCamera"; }

    bool HasCamera() override {
        if (!cameraApi_) return false;
        bool hasCam = false;
        const auto err = cameraApi_->HasCamera(::vr::k_unTrackedDeviceIndex_Hmd, &hasCam);
        return (err == ::vr::VRTrackedCameraError_None) && hasCam;
    }

    bool GetFrameSize(uint32_t& width, uint32_t& height, uint32_t& bufferSize) override {
        if (!cameraApi_) return false;
        const auto err = cameraApi_->GetCameraFrameSize(
            ::vr::k_unTrackedDeviceIndex_Hmd,
            ::vr::VRTrackedCameraFrameType_Undistorted,
            &width, &height, &bufferSize);
        return err == ::vr::VRTrackedCameraError_None;
    }

    bool GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) override {
        if (!cameraApi_) return false;
        ::vr::HmdVector2_t focalLength{}, center{};
        const auto err = cameraApi_->GetCameraIntrinsics(
            ::vr::k_unTrackedDeviceIndex_Hmd,
            0,
            ::vr::VRTrackedCameraFrameType_Undistorted,
            &focalLength, &center);
        if (err != ::vr::VRTrackedCameraError_None) return false;
        intrinsics.fx = focalLength.v[0];
        intrinsics.fy = focalLength.v[1];
        intrinsics.cx = center.v[0];
        intrinsics.cy = center.v[1];
        uint32_t w = 0, h = 0, b = 0;
        if (GetFrameSize(w, h, b)) {
            intrinsics.imageWidth = w;
            intrinsics.imageHeight = h;
        }
        return true;
    }

    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override {
        if (!system_) return false;
        ::vr::ETrackedPropertyError propErr = ::vr::TrackedProp_Success;
        outTransform = system_->GetMatrix34TrackedDeviceProperty(
            ::vr::k_unTrackedDeviceIndex_Hmd,
            ::vr::Prop_CameraToHeadTransform_Matrix34,
            &propErr);
        if (propErr != ::vr::TrackedProp_Success) {
            outTransform = ::vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                                {0.0F, 1.0F, 0.0F, 0.0F},
                                                {0.0F, 0.0F, 1.0F, 0.0F}}};
        }
        return true;
    }

    bool AcquireStreaming() override {
        if (!cameraApi_) {
            lastError_ = "OpenVR IVRTrackedCamera API unavailable";
            return false;
        }
        if (handle_ != 0) return true;
        cameraApi_->SetCameraTrackingSpace(::vr::TrackingUniverseStanding);
        const auto err = cameraApi_->AcquireVideoStreamingService(
            ::vr::k_unTrackedDeviceIndex_Hmd, &handle_);
        if (err != ::vr::VRTrackedCameraError_None || handle_ == 0) {
            const char* errName = cameraApi_->GetCameraErrorNameFromEnum(err);
            lastError_ = errName ? errName : "Acquire failed";
            return false;
        }
        lastError_.clear();
        return true;
    }

    void ReleaseStreaming() override {
        if (cameraApi_ && handle_ != 0) {
            cameraApi_->ReleaseVideoStreamingService(handle_);
            handle_ = 0;
        }
        lastError_.clear();
    }

    const char* GetLastError() const override { return lastError_.c_str(); }

    ::vr::EVRTrackedCameraError GetFrame(
        void* frameBuffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        if (!cameraApi_ || handle_ == 0) return ::vr::VRTrackedCameraError_InvalidHandle;
        return cameraApi_->GetVideoStreamFrameBuffer(
            handle_,
            ::vr::VRTrackedCameraFrameType_Undistorted,
            frameBuffer,
            bufferSize,
            &header,
            sizeof(header));
    }

private:
    ::vr::IVRSystem* system_{nullptr};
    ::vr::IVRTrackedCamera* cameraApi_{nullptr};
    ::vr::TrackedCameraHandle_t handle_{0};
    std::string lastError_;
};

class AutoTrackedCameraSource : public ITrackedCameraSource {
public:
    AutoTrackedCameraSource(core::ILogger& logger, ::vr::IVRSystem* system, ::vr::IVRTrackedCamera* cameraApi)
        : logger_(logger) {
        openvrSource_ = std::make_unique<OpenVrTrackedCameraSource>(system, cameraApi);
#if defined(__linux__)
        v4l2Source_ = std::make_unique<SteamFrameV4L2CameraSource>(logger, system);
#endif
    }

    const char* GetSourceName() const override {
        if (activeSource_) return activeSource_->GetSourceName();
        return "Auto (Searching Camera...)";
    }

    bool HasCamera() override {
        if (openvrSource_ && openvrSource_->HasCamera()) return true;
#if defined(__linux__)
        if (v4l2Source_ && v4l2Source_->HasCamera()) return true;
#endif
        return false;
    }

    bool GetFrameSize(uint32_t& width, uint32_t& height, uint32_t& bufferSize) override {
        if (activeSource_) return activeSource_->GetFrameSize(width, height, bufferSize);
        if (openvrSource_ && openvrSource_->HasCamera()) return openvrSource_->GetFrameSize(width, height, bufferSize);
#if defined(__linux__)
        if (v4l2Source_ && v4l2Source_->HasCamera()) return v4l2Source_->GetFrameSize(width, height, bufferSize);
#endif
        width = 640;
        height = 480;
        bufferSize = 640 * 480 * 4;
        return true;
    }

    bool GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) override {
        if (activeSource_) return activeSource_->GetIntrinsics(intrinsics);
        if (openvrSource_ && openvrSource_->HasCamera()) return openvrSource_->GetIntrinsics(intrinsics);
#if defined(__linux__)
        if (v4l2Source_ && v4l2Source_->HasCamera()) return v4l2Source_->GetIntrinsics(intrinsics);
#endif
        intrinsics = {384.0F, 384.0F, 320.0F, 240.0F, 640, 480};
        return true;
    }

    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override {
        if (activeSource_) return activeSource_->GetCameraToHeadTransform(outTransform);
        if (openvrSource_ && openvrSource_->HasCamera()) return openvrSource_->GetCameraToHeadTransform(outTransform);
#if defined(__linux__)
        if (v4l2Source_ && v4l2Source_->HasCamera()) return v4l2Source_->GetCameraToHeadTransform(outTransform);
#endif
        outTransform = ::vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F, 0.0F}}};
        return true;
    }

    bool AcquireStreaming() override {
        if (activeSource_) return true;
#if defined(__linux__)
        if (std::getenv("PHONECAST_CAMERA_DEVICE") != nullptr && v4l2Source_ && v4l2Source_->AcquireStreaming()) {
            activeSource_ = v4l2Source_.get();
            logger_.Log(core::LogLevel::Info, "camera-worker", "Acquired camera streaming via explicit Linux V4L2 device.");
            return true;
        }
#endif
        if (openvrSource_ && openvrSource_->AcquireStreaming()) {
            activeSource_ = openvrSource_.get();
            logger_.Log(core::LogLevel::Info, "camera-worker", "Acquired camera streaming via OpenVR IVRTrackedCamera.");
            return true;
        }
#if defined(__linux__)
        if (v4l2Source_ && v4l2Source_->AcquireStreaming()) {
            activeSource_ = v4l2Source_.get();
            logger_.Log(core::LogLevel::Info, "camera-worker", "Acquired camera streaming via direct Linux V4L2 (Steam Frame hardware).");
            return true;
        }
#endif
        return false;
    }

    void ReleaseStreaming() override {
        if (activeSource_) {
            activeSource_->ReleaseStreaming();
            activeSource_ = nullptr;
        }
    }

    ::vr::EVRTrackedCameraError GetFrame(
        void* frameBuffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        if (activeSource_) return activeSource_->GetFrame(frameBuffer, bufferSize, header);
        return ::vr::VRTrackedCameraError_InvalidHandle;
    }

    const char* GetLastError() const override {
        if (activeSource_) return activeSource_->GetLastError();
        if (openvrSource_ && std::strlen(openvrSource_->GetLastError()) > 0)
            return openvrSource_->GetLastError();
#if defined(__linux__)
        if (v4l2Source_ && std::strlen(v4l2Source_->GetLastError()) > 0)
            return v4l2Source_->GetLastError();
#endif
        return "";
    }

private:
    core::ILogger& logger_;
    std::unique_ptr<OpenVrTrackedCameraSource> openvrSource_;
#if defined(__linux__)
    std::unique_ptr<SteamFrameV4L2CameraSource> v4l2Source_;
#endif
    ITrackedCameraSource* activeSource_{nullptr};
};

} // namespace

TrackedCameraWorker::TrackedCameraWorker(core::ILogger& logger, ::vr::IVRSystem* system, ::vr::IVRTrackedCamera* cameraApi)
    : TrackedCameraWorker(logger, std::make_unique<AutoTrackedCameraSource>(logger, system, cameraApi)) {}

TrackedCameraWorker::TrackedCameraWorker(core::ILogger& logger, std::unique_ptr<ITrackedCameraSource> cameraSource)
    : logger_(logger), cameraSource_(std::move(cameraSource)) {
    cameraToHeadTransform_ = ::vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                                 {0.0F, 1.0F, 0.0F, 0.0F},
                                                 {0.0F, 0.0F, 1.0F, 0.0F}}};
    if (cameraSource_) {
        cameraSource_->GetFrameSize(frameWidth_, frameHeight_, frameBufferSize_);
        cameraSource_->GetIntrinsics(intrinsics_);
        cameraSource_->GetCameraToHeadTransform(cameraToHeadTransform_);
    }
    if (frameBufferSize_ == 0) {
        frameWidth_ = 640;
        frameHeight_ = 480;
        frameBufferSize_ = frameWidth_ * frameHeight_ * 4;
    }
    frameBuffer_.resize(frameBufferSize_, 0);
}

TrackedCameraWorker::~TrackedCameraWorker() {
    Stop();
}

bool TrackedCameraWorker::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    workerThread_ = std::thread(&TrackedCameraWorker::WorkerLoop, this);
    logger_.Log(core::LogLevel::Info, "camera-worker", "TrackedCameraWorker background thread started.");
    return true;
}

void TrackedCameraWorker::SetActive(bool active) {
    if (active_.exchange(active) != active) {
        if (active) {
            wakeCv_.notify_all();
            logger_.Log(core::LogLevel::Info, "camera-worker", "TrackedCameraWorker activated.");
        } else {
            logger_.Log(core::LogLevel::Info, "camera-worker", "TrackedCameraWorker suspended to low-power idle.");
        }
    }
}

bool TrackedCameraWorker::IsActive() const noexcept {
    return active_.load();
}

void TrackedCameraWorker::SetScreenMetrics(const core::protocol::StreamScreenMetrics& metrics) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    screenMetrics_.widthMillimeters = metrics.physicalWidthMm;
    screenMetrics_.heightMillimeters = metrics.physicalHeightMm;
    screenMetrics_.pixelWidth = metrics.pixelWidth;
    screenMetrics_.pixelHeight = metrics.pixelHeight;
    hasMetrics_ = screenMetrics_.IsValid();
    {
        std::lock_guard<std::mutex> snapLock(snapshotMutex_);
        if (screenMetrics_.WidthMeters() > 0.0F) {
            latestSnapshot_.physicalWidthMeters = screenMetrics_.WidthMeters();
        }
        latestDebugInfo_.hasMetrics = hasMetrics_;
        latestDebugInfo_.metricsWidthMm = screenMetrics_.widthMillimeters;
        latestDebugInfo_.metricsHeightMm = screenMetrics_.heightMillimeters;
        latestDebugInfo_.physicalWidthMeters = screenMetrics_.WidthMeters();
        latestDebugInfo_.physicalHeightMeters = screenMetrics_.HeightMeters();
    }
}

void TrackedCameraWorker::IngestPhoneImu(const core::protocol::StreamPhoneImu& imu) {
    core::tracking::PhoneImuPacket packet{};
    packet.sequence = 0;
    packet.timestampMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    packet.orientation = core::tracking::Quaternion{imu.qw, imu.qx, imu.qy, imu.qz};
    packet.angularVelocity = core::tracking::Vector3{imu.gyroX, imu.gyroY, imu.gyroZ};
    packet.linearAcceleration = core::tracking::Vector3{imu.accelX, imu.accelY, imu.accelZ};

    std::lock_guard<std::mutex> lock(dataMutex_);
    if (pendingImu_.size() >= 32) {
        pendingImu_.pop_front();
    }
    pendingImu_.push_back(packet);
    latestImu_ = packet;
    hasImu_ = true;
    ++totalImuPackets_;

    {
        std::lock_guard<std::mutex> snapLock(snapshotMutex_);
        latestDebugInfo_.hasImu = true;
        latestDebugInfo_.imuPacketsReceived = totalImuPackets_;
    }
}

bool TrackedCameraWorker::GetLatestPose(::vr::HmdMatrix34_t& outTransform, float* outPhysicalWidth) {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (!latestSnapshot_.valid) {
        return false;
    }
    outTransform = latestSnapshot_.transform;
    if (outPhysicalWidth != nullptr) {
        *outPhysicalWidth = latestSnapshot_.physicalWidthMeters;
    }
    return true;
}

TrackedCameraPoseSnapshot TrackedCameraWorker::GetSnapshot() {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return latestSnapshot_;
}

bool TrackedCameraWorker::GetLatestCameraFrame(CameraFrameSnapshot& outSnapshot) {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (!latestFrameSnapshot_.valid) return false;
    outSnapshot = latestFrameSnapshot_;
    return true;
}

TrackedCameraDebugInfo TrackedCameraWorker::GetDebugInfo() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return latestDebugInfo_;
}

void TrackedCameraWorker::Stop() noexcept {
    if (running_.exchange(false)) {
        active_.store(false);
        wakeCv_.notify_all();
        if (workerThread_.joinable()) {
            try {
                workerThread_.join();
            } catch (...) {}
        }
        if (cameraSource_) {
            try {
                cameraSource_->ReleaseStreaming();
            } catch (...) {}
        }
    }
}

bool TrackedCameraWorker::DetectScreenCorners(
    const core::tracking::CameraFrameView& frame,
    core::tracking::ScreenCorners2D& outCorners) {
    core::tracking::LineFitterRansac::ScanlineParams scanParams{};
    scanParams.numScanlines = 30;
    scanParams.scanlineHalfLengthPixels = priorCorners_.valid ? 24.0F : 32.0F;
    scanParams.minGradientMagnitude = priorCorners_.valid ? 20.0F : 16.0F;
    scanParams.marginRatio = 0.15F;

    core::tracking::Vector2 ptTL{}, ptTR{}, ptBR{}, ptBL{};
    bool seedFound = false;

    // Dynamic luma threshold calculation:
    // In real rooms, ambient light can easily exceed 60 across the FOV.
    // We find the peak luma in the image and set a relative threshold clamped between [60, 200].
    uint8_t maxLuma = 0;
    for (uint32_t y = 16; y < frame.height - 16; y += 16) {
        for (uint32_t x = 16; x < frame.width - 16; x += 16) {
            const uint8_t lum = frame.GetLuma(x, y);
            if (lum > maxLuma) maxLuma = lum;
        }
    }

    const uint8_t lumaThreshold = std::clamp(
        static_cast<uint8_t>(maxLuma * 0.75F),
        static_cast<uint8_t>(60),
        static_cast<uint8_t>(200));

    // Fast coarse grid scan (step 16) to find phone display bounding region
    float minX = static_cast<float>(frame.width);
    float maxX = 0.0F;
    float minY = static_cast<float>(frame.height);
    float maxY = 0.0F;
    uint32_t brightCount = 0;

    for (uint32_t y = 16; y < frame.height - 16; y += 16) {
        for (uint32_t x = 16; x < frame.width - 16; x += 16) {
            if (frame.GetLuma(x, y) >= lumaThreshold) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                minX = std::min(minX, fx);
                maxX = std::max(maxX, fx);
                minY = std::min(minY, fy);
                maxY = std::max(maxY, fy);
                ++brightCount;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        latestDebugInfo_.peakLuma = maxLuma;
        latestDebugInfo_.lumaThreshold = lumaThreshold;
        latestDebugInfo_.brightPixelCount = brightCount;
    }

    if (priorCorners_.valid) {
        ptTL = priorCorners_.corners[0];
        ptTR = priorCorners_.corners[1];
        ptBR = priorCorners_.corners[2];
        ptBL = priorCorners_.corners[3];
        seedFound = true;
    }

    if (!seedFound) {
        const float boxW = maxX - minX;
        const float boxH = maxY - minY;
        const float frameW = static_cast<float>(frame.width);
        const float frameH = static_cast<float>(frame.height);

        // Check for plausible phone bounding box dimensions (not taking up the whole image or a tiny speck)
        if (brightCount >= 6 && boxW >= 30.0F && boxH >= 30.0F && boxW < frameW * 0.90F && boxH < frameH * 0.90F) {
            ptTL = {minX, minY};
            ptTR = {maxX, minY};
            ptBR = {maxX, maxY};
            ptBL = {minX, maxY};
            seedFound = true;
        } else {
            // Default nominal seed in view center
            const float cx = static_cast<float>(frame.width) * 0.5F;
            const float cy = static_cast<float>(frame.height) * 0.5F;
            const float halfW = static_cast<float>(frame.width) * 0.20F;
            const float halfH = static_cast<float>(frame.height) * 0.25F;
            ptTL = {cx - halfW, cy - halfH};
            ptTR = {cx + halfW, cy - halfH};
            ptBR = {cx + halfW, cy + halfH};
            ptBL = {cx - halfW, cy + halfH};
            seedFound = true;
        }
    }

    core::tracking::ScreenBorders borders{};
    if (seedFound) {
        borders.top = lineFitter_.DetectAndFit(frame, ptTL, ptTR, scanParams);
        borders.right = lineFitter_.DetectAndFit(frame, ptTR, ptBR, scanParams);
        borders.bottom = lineFitter_.DetectAndFit(frame, ptBL, ptBR, scanParams);
        borders.left = lineFitter_.DetectAndFit(frame, ptTL, ptBL, scanParams);
        borders.allValid = borders.top.valid && borders.right.valid &&
                           borders.bottom.valid && borders.left.valid;
    }

    if (!borders.IsValid() && !priorCorners_.valid) {
        // Fallback: try nominal center seed in lower-center area where phone is typically held
        const float cx = static_cast<float>(frame.width) * 0.5F;
        const float cy = static_cast<float>(frame.height) * 0.52F;
        const float halfW = static_cast<float>(frame.width) * 0.18F;
        const float halfH = static_cast<float>(frame.height) * 0.25F;
        const auto nTL = core::tracking::Vector2{cx - halfW, cy - halfH};
        const auto nTR = core::tracking::Vector2{cx + halfW, cy - halfH};
        const auto nBR = core::tracking::Vector2{cx + halfW, cy + halfH};
        const auto nBL = core::tracking::Vector2{cx - halfW, cy + halfH};

        borders.top = lineFitter_.DetectAndFit(frame, nTL, nTR, scanParams);
        borders.right = lineFitter_.DetectAndFit(frame, nTR, nBR, scanParams);
        borders.bottom = lineFitter_.DetectAndFit(frame, nBL, nBR, scanParams);
        borders.left = lineFitter_.DetectAndFit(frame, nTL, nBL, scanParams);
        borders.allValid = borders.top.valid && borders.right.valid &&
                           borders.bottom.valid && borders.left.valid;
    }

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        latestDebugInfo_.borderTop = borders.top.valid;
        latestDebugInfo_.borderRight = borders.right.valid;
        latestDebugInfo_.borderBottom = borders.bottom.valid;
        latestDebugInfo_.borderLeft = borders.left.valid;
        latestDebugInfo_.cornersValid = outCorners.valid;
    }

    if (borders.IsValid()) {
        outCorners = core::tracking::CornerIntersection::ComputeCorners(borders);
        if (outCorners.valid) {
            {
                std::lock_guard<std::mutex> lock(snapshotMutex_);
                latestDebugInfo_.cornersValid = true;
            }
            priorCorners_ = outCorners;
            lastValidCorners_ = outCorners;
            occlusionFrameCount_ = 0;
            return true;
        }
    }

    ++occlusionFrameCount_;
    if (occlusionFrameCount_ > 45) {
        priorCorners_.valid = false;
        lastValidCorners_.valid = false;
    }
    return false;
}

void TrackedCameraWorker::ProcessFrame(
    const uint8_t* buffer,
    const ::vr::CameraVideoStreamFrameHeader_t& header) {
    // 1. Drain pending IMU packets into fusion engine
    std::vector<core::tracking::PhoneImuPacket> imusToDrain;
    core::tracking::ScreenMetrics currentMetrics{};
    core::tracking::Quaternion imuOrientation = core::tracking::Quaternion::Identity();
    bool hasImu = false;
    bool hasMetrics = false;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        while (!pendingImu_.empty()) {
            imusToDrain.push_back(pendingImu_.front());
            pendingImu_.pop_front();
        }
        currentMetrics = screenMetrics_;
        hasMetrics = hasMetrics_;
        if (!hasMetrics || !currentMetrics.IsValid()) {
            currentMetrics = core::tracking::ScreenMetrics{71.0F, 155.0F, 1080, 2400};
            hasMetrics = true;
        }
        hasImu = hasImu_;
        if (hasImu) {
            imuOrientation = latestImu_.orientation;
        }
    }

    for (const auto& imu : imusToDrain) {
        fusion_.IngestImu(imu);
    }

    // 2. Optical pipeline
    core::tracking::CameraFrameView frameView{};
    frameView.data = buffer;
    frameView.width = header.nWidth > 0 ? header.nWidth : frameWidth_;
    frameView.height = header.nHeight > 0 ? header.nHeight : frameHeight_;
    frameView.bytesPerPixel = header.nBytesPerPixel > 0 ? header.nBytesPerPixel : 4;
    frameView.strideBytes = frameView.width * frameView.bytesPerPixel;
    frameView.timestampMicros = header.ulFrameExposureTime;
    frameView.sequence = header.nFrameSequence;

    if (!intrinsics_.IsValid()) {
        intrinsics_.fx = static_cast<float>(frameView.width) * 1.1F;
        intrinsics_.fy = static_cast<float>(frameView.width) * 1.1F;
        intrinsics_.cx = static_cast<float>(frameView.width) * 0.5F;
        intrinsics_.cy = static_cast<float>(frameView.height) * 0.5F;
        intrinsics_.imageWidth = frameView.width;
        intrinsics_.imageHeight = frameView.height;
    }

    core::tracking::ScreenCorners2D corners{};
    const bool cornersFound = DetectScreenCorners(frameView, corners);

    if (cornersFound && hasMetrics && currentMetrics.IsValid()) {
        const auto pnpResult = pnpSolver_.Solve(corners, currentMetrics, intrinsics_);
        if (pnpResult.success) {
            core::tracking::Vector3 cameraGravity{0.0F, 1.0F, 0.0F};
            const auto& hmdPose = header.trackedDevicePose;
            if (hmdPose.bPoseIsValid) {
                const vr::HmdMatrix34_t tHmdCamera = cameraToHeadTransform_;
                const vr::HmdMatrix34_t tStandingHmd = hmdPose.mDeviceToAbsoluteTracking;
                const vr::HmdMatrix34_t tStandingCamera = Multiply(tStandingHmd, tHmdCamera);

                // World gravity in SteamVR Standing universe is [0, -1, 0]^T (along -Y).
                // Project into Camera VR frame via R_{Standing <- Cam}^T * [0, -1, 0]^T:
                //   g_cam_vr = [-tStandingCamera.m[1][0], -tStandingCamera.m[1][1], -tStandingCamera.m[1][2]]
                // And convert from OpenVR Camera (+Y up, +Z back) to CV (+Y down, +Z forward):
                //   g_cam_cv = [g_cam_vr.x, -g_cam_vr.y, -g_cam_vr.z]
                cameraGravity = {
                    -tStandingCamera.m[1][0],
                     tStandingCamera.m[1][1],
                     tStandingCamera.m[1][2]
                };
            }

            core::tracking::Pose6D opticalCamPose = core::tracking::PlanarPnpSolver::Disambiguate(
                pnpResult, imuOrientation, cameraGravity);

            // Compound: T_{Standing <- Phone} = T_{Standing <- HMD} * T_{HMD <- Camera_VR} * M_{VR <- CV} * T_{Camera_CV <- Phone}
            const vr::HmdMatrix34_t tCamCvPhone = Pose6DToHmdMatrix(opticalCamPose);
            const vr::HmdMatrix34_t tCamVrPhone = ApplyM_VR_CV(tCamCvPhone);

            if (hmdPose.bPoseIsValid) {
                const vr::HmdMatrix34_t tHmdCamera = cameraToHeadTransform_;
                const vr::HmdMatrix34_t tStandingHmd = hmdPose.mDeviceToAbsoluteTracking;
                const vr::HmdMatrix34_t tStandingPhone = Multiply(Multiply(tStandingHmd, tHmdCamera), tCamVrPhone);

                core::tracking::Pose6D standingOpticalPose = HmdMatrixToPose6D(tStandingPhone);
                const uint64_t nowMicros = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                standingOpticalPose.timestampMicros = nowMicros;
                standingOpticalPose.confidence = opticalCamPose.confidence;
                fusion_.IngestOpticalPose(standingOpticalPose);
            }
        }
    }

    // 3. Evaluate fused pose (handles stable occlusion hold & 1-Euro smoothing)
    const uint64_t nowMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const core::tracking::Pose6D fusedPose = fusion_.EvaluatePose(nowMicros);

    // 4. Update snapshot under mutex
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        latestSnapshot_.valid = fusedPose.valid;
        latestSnapshot_.state = fusedPose.state;
        latestSnapshot_.confidence = fusedPose.confidence;
        if (fusedPose.valid) {
            latestSnapshot_.transform = Pose6DToHmdMatrix(fusedPose);
        }
        latestSnapshot_.physicalWidthMeters = currentMetrics.WidthMeters() > 0.0F
            ? currentMetrics.WidthMeters()
            : 0.072F;
        latestSnapshot_.sequence = header.nFrameSequence;
        latestSnapshot_.timestamp = std::chrono::steady_clock::now();

        latestDebugInfo_.active = active_.load();
        latestDebugInfo_.streamingAcquired = true;
        if (cameraSource_) {
            const char* srcName = cameraSource_->GetSourceName();
            if (srcName != nullptr) {
                std::strncpy(latestDebugInfo_.sourceName, srcName, sizeof(latestDebugInfo_.sourceName) - 1);
                latestDebugInfo_.sourceName[sizeof(latestDebugInfo_.sourceName) - 1] = '\0';
            }
        }
        latestDebugInfo_.frameWidth = frameView.width;
        latestDebugInfo_.frameHeight = frameView.height;
        latestDebugInfo_.frameSequence = header.nFrameSequence;
        latestDebugInfo_.fps = currentFps_;

        latestDebugInfo_.state = fusedPose.state;
        latestDebugInfo_.poseValid = fusedPose.valid;
        latestDebugInfo_.confidence = fusedPose.confidence;
        latestDebugInfo_.physicalWidthMeters = latestSnapshot_.physicalWidthMeters;
        latestDebugInfo_.physicalHeightMeters = currentMetrics.heightMillimeters * 0.001F;

        if (fusedPose.valid) {
            latestDebugInfo_.standingX = fusedPose.position.x;
            latestDebugInfo_.standingY = fusedPose.position.y;
            latestDebugInfo_.standingZ = fusedPose.position.z;

            const auto& hmdPose = header.trackedDevicePose;
            if (hmdPose.bPoseIsValid) {
                const float hmdX = hmdPose.mDeviceToAbsoluteTracking.m[0][3];
                const float hmdY = hmdPose.mDeviceToAbsoluteTracking.m[1][3];
                const float hmdZ = hmdPose.mDeviceToAbsoluteTracking.m[2][3];
                latestDebugInfo_.hmdRelativeX = fusedPose.position.x - hmdX;
                latestDebugInfo_.hmdRelativeY = fusedPose.position.y - hmdY;
                latestDebugInfo_.hmdRelativeZ = fusedPose.position.z - hmdZ;
                latestDebugInfo_.hmdDistanceMeters = std::sqrt(
                    latestDebugInfo_.hmdRelativeX * latestDebugInfo_.hmdRelativeX +
                    latestDebugInfo_.hmdRelativeY * latestDebugInfo_.hmdRelativeY +
                    latestDebugInfo_.hmdRelativeZ * latestDebugInfo_.hmdRelativeZ);
            }

            const auto& m = latestSnapshot_.transform.m;
            latestDebugInfo_.pitchDeg = std::atan2(-m[1][2], m[2][2]) * 180.0F / 3.14159265F;
            latestDebugInfo_.yawDeg = std::asin(std::clamp(m[0][2], -1.0F, 1.0F)) * 180.0F / 3.14159265F;
            latestDebugInfo_.rollDeg = std::atan2(-m[0][1], m[0][0]) * 180.0F / 3.14159265F;
        }

        latestDebugInfo_.hasImu = hasImu;
        latestDebugInfo_.imuPacketsReceived = totalImuPackets_;
        latestDebugInfo_.hasMetrics = hasMetrics;
        latestDebugInfo_.metricsWidthMm = currentMetrics.widthMillimeters;
        latestDebugInfo_.metricsHeightMm = currentMetrics.heightMillimeters;

        if (active_ && buffer != nullptr && frameView.width > 0 && frameView.height > 0) {
            latestFrameSnapshot_.valid = true;
            latestFrameSnapshot_.width = frameView.width;
            latestFrameSnapshot_.height = frameView.height;
            latestFrameSnapshot_.sequence = header.nFrameSequence;
            latestFrameSnapshot_.state = fusedPose.state;
            latestFrameSnapshot_.confidence = fusedPose.confidence;

            if (cornersFound) {
                latestFrameSnapshot_.corners = corners;
            } else if (fusedPose.state == core::tracking::TrackingState::Occluded && lastValidCorners_.valid) {
                // Hold stable corners during occlusion so passthrough cutout remains anchored to the phone
                latestFrameSnapshot_.corners = lastValidCorners_;
            } else {
                latestFrameSnapshot_.corners = {};
            }

            const std::size_t byteCount = static_cast<std::size_t>(frameView.width) * frameView.height * 4U;
            if (latestFrameSnapshot_.rgbaBuffer.size() != byteCount) {
                latestFrameSnapshot_.rgbaBuffer.resize(byteCount);
            }
            std::memcpy(latestFrameSnapshot_.rgbaBuffer.data(), buffer, byteCount);
        }
    }
}

void TrackedCameraWorker::WorkerLoop() {
    bool streamingAcquired = false;
    auto lastFailedLogTime = std::chrono::steady_clock::time_point{};

    while (running_) {
        if (!active_) {
            if (streamingAcquired) {
                if (cameraSource_) {
                    cameraSource_->ReleaseStreaming();
                }
                streamingAcquired = false;
                priorCorners_ = {};
                lastValidCorners_ = {};
                occlusionFrameCount_ = 0;
            }
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wakeCv_.wait(lock, [this] { return !running_ || active_; });
            if (!running_) break;
        }

        if (!streamingAcquired && cameraSource_) {
            if (cameraSource_->AcquireStreaming()) {
                streamingAcquired = true;
                logger_.Log(core::LogLevel::Info, "camera-worker",
                            "Headset camera (IVRTrackedCamera) streaming acquired successfully.");
                cameraSource_->GetIntrinsics(intrinsics_);
                cameraSource_->GetCameraToHeadTransform(cameraToHeadTransform_);
                uint32_t w = 0, h = 0, b = 0;
                if (cameraSource_->GetFrameSize(w, h, b) && b > 0) {
                    frameWidth_ = w;
                    frameHeight_ = h;
                    frameBufferSize_ = b;
                    frameBuffer_.resize(frameBufferSize_, 0);
                }
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastFailedLogTime > std::chrono::seconds(5)) {
                    lastFailedLogTime = now;
                    logger_.Log(core::LogLevel::Warning, "camera-worker",
                                "IVRTrackedCamera acquisition failed: headset camera is unavailable or "
                                "disabled in SteamVR settings (or running on standalone Steam Frame).");
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            latestDebugInfo_.active = active_.load();
            latestDebugInfo_.streamingAcquired = streamingAcquired;
            latestDebugInfo_.fps = currentFps_;
            latestDebugInfo_.frameWidth = frameWidth_;
            latestDebugInfo_.frameHeight = frameHeight_;
            latestDebugInfo_.hasImu = hasImu_;
            latestDebugInfo_.imuPacketsReceived = totalImuPackets_;
            latestDebugInfo_.hasMetrics = hasMetrics_;
            latestDebugInfo_.metricsWidthMm = screenMetrics_.widthMillimeters;
            latestDebugInfo_.metricsHeightMm = screenMetrics_.heightMillimeters;
            latestDebugInfo_.physicalWidthMeters = screenMetrics_.WidthMeters();
            latestDebugInfo_.physicalHeightMeters = screenMetrics_.HeightMeters();

            if (cameraSource_) {
                const char* srcName = cameraSource_->GetSourceName();
                if (srcName != nullptr) {
                    std::strncpy(latestDebugInfo_.sourceName, srcName, sizeof(latestDebugInfo_.sourceName) - 1);
                    latestDebugInfo_.sourceName[sizeof(latestDebugInfo_.sourceName) - 1] = '\0';
                }
                const char* errStr = cameraSource_->GetLastError();
                if (errStr != nullptr) {
                    std::strncpy(latestDebugInfo_.cameraError, errStr, sizeof(latestDebugInfo_.cameraError) - 1);
                    latestDebugInfo_.cameraError[sizeof(latestDebugInfo_.cameraError) - 1] = '\0';
                }
            }
        }

        bool frameProcessed = false;
        if (streamingAcquired && cameraSource_) {
            ::vr::CameraVideoStreamFrameHeader_t header{};
            const auto err = cameraSource_->GetFrame(
                frameBuffer_.data(), static_cast<uint32_t>(frameBuffer_.size()), header);
            if (err == ::vr::VRTrackedCameraError_None && header.nFrameSequence != lastSequence_) {
                lastSequence_ = header.nFrameSequence;
                const auto now = std::chrono::steady_clock::now();
                ++fpsFrameCount_;
                const auto elapsedSec = std::chrono::duration<float>(now - lastFpsTime_).count();
                if (elapsedSec >= 1.0F) {
                    currentFps_ = static_cast<float>(fpsFrameCount_) / elapsedSec;
                    fpsFrameCount_ = 0;
                    lastFpsTime_ = now;
                }
                ProcessFrame(frameBuffer_.data(), header);
                frameProcessed = true;
            }
        }

        if (!frameProcessed) {
            // Between camera frames or during camera dropout/offline: drain IMU & update dead reckoning
            std::vector<core::tracking::PhoneImuPacket> imusToDrain;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                while (!pendingImu_.empty()) {
                    imusToDrain.push_back(pendingImu_.front());
                    pendingImu_.pop_front();
                }
            }
            for (const auto& imu : imusToDrain) {
                fusion_.IngestImu(imu);
            }
            const uint64_t nowMicros = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const auto fused = fusion_.EvaluatePose(nowMicros);
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            latestSnapshot_.valid = fused.valid;
            latestSnapshot_.state = fused.state;
            latestSnapshot_.confidence = fused.confidence;
            if (fused.valid) {
                latestSnapshot_.transform = Pose6DToHmdMatrix(fused);
                latestDebugInfo_.standingX = fused.position.x;
                latestDebugInfo_.standingY = fused.position.y;
                latestDebugInfo_.standingZ = fused.position.z;
            }
            latestDebugInfo_.state = fused.state;
            latestDebugInfo_.poseValid = fused.valid;
            latestDebugInfo_.confidence = fused.confidence;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (streamingAcquired && cameraSource_) {
        cameraSource_->ReleaseStreaming();
    }
}

} // namespace phonecast::platform::openvr
