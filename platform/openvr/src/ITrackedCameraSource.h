#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"
#include <openvr.h>
#include <cstdint>

namespace phonecast::platform::openvr {

class ITrackedCameraSource {
public:
    virtual ~ITrackedCameraSource() = default;

    // Name or device path of the camera source
    virtual const char* GetSourceName() const { return "Camera"; }

    // Checks if the tracking system has an operational camera available on the HMD
    virtual bool HasCamera() = 0;

    // Queries frame buffer dimensions and required buffer size for undistorted frames
    virtual bool GetFrameSize(uint32_t& width, uint32_t& height, uint32_t& bufferSize) = 0;

    // Queries focal lengths and optical center intrinsics for undistorted frames
    virtual bool GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) = 0;

    // Queries rigid body extrinsic transform from HMD center to camera frame (T_{HMD <- Camera_VR})
    virtual bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) = 0;

    // Starts video streaming service on the camera device
    virtual bool AcquireStreaming() = 0;

    // Releases video streaming service cleanly
    virtual void ReleaseStreaming() = 0;

    // Retrieves next available frame buffer and frame header with mid-exposure HMD pose
    virtual ::vr::EVRTrackedCameraError GetFrame(
        void* frameBuffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) = 0;

    // Returns detailed error or status message if acquisition or streaming failed
    virtual const char* GetLastError() const { return ""; }
};

}  // namespace phonecast::platform::openvr
