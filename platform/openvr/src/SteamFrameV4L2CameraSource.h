#pragma once

#include "ITrackedCameraSource.h"
#include "phonecast/core/logging/ILogger.h"
#include <openvr.h>
#include <memory>
#include <string>

namespace phonecast::platform::openvr {

class SteamFrameV4L2CameraSource : public ITrackedCameraSource {
public:
    SteamFrameV4L2CameraSource(core::ILogger& logger, ::vr::IVRSystem* system);
    ~SteamFrameV4L2CameraSource() override;

    const char* GetSourceName() const override;
    bool HasCamera() override;
    bool GetFrameSize(uint32_t& width, uint32_t& height, uint32_t& bufferSize) override;
    bool GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) override;
    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override;
    bool AcquireStreaming() override;
    void ReleaseStreaming() override;
    ::vr::EVRTrackedCameraError GetFrame(
        void* frameBuffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override;

    // Discovers the Sony IMX616 Steam Frame sensor video node under /sys/class/video4linux
    static std::string FindSteamFrameCameraDevice();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecast::platform::openvr
