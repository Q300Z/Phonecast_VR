#include "SteamFrameV4L2CameraSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace phonecast::platform::openvr {

#if defined(__linux__)

namespace {

// Converts YUYV422 (Y0, U, Y1, V) to RGBA32
void ConvertYUYVToRGBA(const uint8_t* yuyv, uint8_t* rgba, uint32_t width, uint32_t height) {
    const size_t totalPixels = static_cast<size_t>(width) * height;
    const size_t pairs = totalPixels / 2;

    const auto clampByte = [](int value) noexcept -> uint8_t {
        return static_cast<uint8_t>(std::max(0, std::min(255, value)));
    };

    for (size_t i = 0; i < pairs; ++i) {
        const int y0 = yuyv[i * 4 + 0];
        const int u  = yuyv[i * 4 + 1] - 128;
        const int y1 = yuyv[i * 4 + 2];
        const int v  = yuyv[i * 4 + 3] - 128;

        // Pixel 0
        const int r0 = y0 + ((359 * v) >> 8);
        const int g0 = y0 - ((88 * u + 183 * v) >> 8);
        const int b0 = y0 + ((454 * u) >> 8);

        rgba[i * 8 + 0] = clampByte(r0);
        rgba[i * 8 + 1] = clampByte(g0);
        rgba[i * 8 + 2] = clampByte(b0);
        rgba[i * 8 + 3] = 255;

        // Pixel 1
        const int r1 = y1 + ((359 * v) >> 8);
        const int g1 = y1 - ((88 * u + 183 * v) >> 8);
        const int b1 = y1 + ((454 * u) >> 8);

        rgba[i * 8 + 4] = clampByte(r1);
        rgba[i * 8 + 5] = clampByte(g1);
        rgba[i * 8 + 6] = clampByte(b1);
        rgba[i * 8 + 7] = 255;
    }
}

void ConvertRGB24ToRGBA(const uint8_t* rgb, uint8_t* rgba, uint32_t width, uint32_t height) {
    const size_t totalPixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < totalPixels; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
}

}  // namespace

class SteamFrameV4L2CameraSource::Impl {
public:
    core::ILogger& logger;
    ::vr::IVRSystem* system{nullptr};
    std::string devicePath;
    int fd{-1};
    uint32_t width{640};
    uint32_t height{480};
    uint32_t pixelFormat{V4L2_PIX_FMT_YUYV};
    bool streamingActive{false};
    uint32_t sequence{0};

    struct MmapBuffer {
        void* start{nullptr};
        size_t length{0};
    };
    std::vector<MmapBuffer> buffers;

    Impl(core::ILogger& logger, ::vr::IVRSystem* system)
        : logger(logger), system(system) {
        devicePath = FindSteamFrameCameraDevice();
    }

    ~Impl() {
        ReleaseStreaming();
    }

    bool HasCamera() {
        if (!devicePath.empty()) return true;
        devicePath = FindSteamFrameCameraDevice();
        return !devicePath.empty();
    }

    bool GetFrameSize(uint32_t& outWidth, uint32_t& outHeight, uint32_t& outBufferSize) {
        outWidth = width;
        outHeight = height;
        outBufferSize = width * height * 4U;
        return true;
    }

    bool GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) {
        intrinsics.imageWidth = width;
        intrinsics.imageHeight = height;
        // Default nominal wide FOV (~90 deg) for Steam Frame IMX616 front sensor
        intrinsics.fx = static_cast<float>(width) * 0.60F;
        intrinsics.fy = static_cast<float>(height) * 0.60F;
        intrinsics.cx = static_cast<float>(width) * 0.50F;
        intrinsics.cy = static_cast<float>(height) * 0.50F;
        return true;
    }

    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) {
        if (system != nullptr) {
            ::vr::ETrackedPropertyError propErr = ::vr::TrackedProp_Success;
            outTransform = system->GetMatrix34TrackedDeviceProperty(
                ::vr::k_unTrackedDeviceIndex_Hmd,
                ::vr::Prop_CameraToHeadTransform_Matrix34,
                &propErr);
            if (propErr == ::vr::TrackedProp_Success) return true;
        }
        outTransform = ::vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F, 0.0F}}};
        return true;
    }

    bool AcquireStreaming() {
        if (streamingActive && fd >= 0) return true;
        if (!HasCamera()) {
            return false;
        }

        fd = ::open(devicePath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            logger.Log(core::LogLevel::Warning, "v4l2-camera",
                       "Failed to open camera device node: " + devicePath);
            return false;
        }

        v4l2_capability cap{};
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            logger.Log(core::LogLevel::Warning, "v4l2-camera", "VIDIOC_QUERYCAP failed on " + devicePath);
            ::close(fd);
            fd = -1;
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
            logger.Log(core::LogLevel::Warning, "v4l2-camera", "Device does not support streaming video capture.");
            ::close(fd);
            fd = -1;
            return false;
        }

        // Try negotiated formats: YUYV first, fallback RGB24
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 640;
        fmt.fmt.pix.height = 480;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (::ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
            if (::ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
                logger.Log(core::LogLevel::Warning, "v4l2-camera", "VIDIOC_S_FMT failed.");
                ::close(fd);
                fd = -1;
                return false;
            }
        }

        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
        pixelFormat = fmt.fmt.pix.pixelformat;

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
            logger.Log(core::LogLevel::Warning, "v4l2-camera", "VIDIOC_REQBUFS failed.");
            ::close(fd);
            fd = -1;
            return false;
        }

        buffers.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (::ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                ReleaseStreaming();
                return false;
            }
            buffers[i].length = buf.length;
            buffers[i].start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
            if (buffers[i].start == MAP_FAILED) {
                ReleaseStreaming();
                return false;
            }
            if (::ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                ReleaseStreaming();
                return false;
            }
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            logger.Log(core::LogLevel::Warning, "v4l2-camera", "VIDIOC_STREAMON failed.");
            ReleaseStreaming();
            return false;
        }

        streamingActive = true;
        logger.Log(core::LogLevel::Info, "v4l2-camera",
                   "Direct V4L2 streaming started on " + devicePath + " (" +
                   std::to_string(width) + "x" + std::to_string(height) + ").");
        return true;
    }

    void ReleaseStreaming() {
        if (streamingActive && fd >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ::ioctl(fd, VIDIOC_STREAMOFF, &type);
            streamingActive = false;
        }
        for (auto& buf : buffers) {
            if (buf.start != nullptr && buf.start != MAP_FAILED) {
                ::munmap(buf.start, buf.length);
            }
        }
        buffers.clear();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    ::vr::EVRTrackedCameraError GetFrame(
        void* frameBuffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) {
        if (!streamingActive || fd < 0 || frameBuffer == nullptr) {
            return ::vr::VRTrackedCameraError_InvalidHandle;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            return ::vr::VRTrackedCameraError_OperationFailed;
        }

        if (buf.index < buffers.size()) {
            const auto* src = static_cast<const uint8_t*>(buffers[buf.index].start);
            auto* dst = static_cast<uint8_t*>(frameBuffer);
            const uint32_t requiredSize = width * height * 4U;

            if (bufferSize >= requiredSize) {
                if (pixelFormat == V4L2_PIX_FMT_YUYV) {
                    ConvertYUYVToRGBA(src, dst, width, height);
                } else if (pixelFormat == V4L2_PIX_FMT_RGB24) {
                    ConvertRGB24ToRGBA(src, dst, width, height);
                } else {
                    std::memcpy(dst, src, std::min<size_t>(bufferSize, buf.bytesused));
                }
            }
        }

        // Requeue buffer immediately
        ::ioctl(fd, VIDIOC_QBUF, &buf);

        // Populate OpenVR header
        header.nWidth = width;
        header.nHeight = height;
        header.nBytesPerPixel = 4;
        header.nFrameSequence = sequence++;
        header.ulFrameExposureTime = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        header.trackedDevicePose.bPoseIsValid = false;
        header.trackedDevicePose.bDeviceIsConnected = true;
        header.trackedDevicePose.mDeviceToAbsoluteTracking = ::vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                                 {0.0F, 1.0F, 0.0F, 0.0F},
                                                                                 {0.0F, 0.0F, 1.0F, 0.0F}}};
        if (system != nullptr) {
            ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount]{};
            system->GetDeviceToAbsoluteTrackingPose(::vr::TrackingUniverseStanding, 0.0F, poses, ::vr::k_unMaxTrackedDeviceCount);
            if (poses[::vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
                header.trackedDevicePose = poses[::vr::k_unTrackedDeviceIndex_Hmd];
            }
        }

        return ::vr::VRTrackedCameraError_None;
    }
};

std::string SteamFrameV4L2CameraSource::FindSteamFrameCameraDevice() {
    std::error_code ec;
    auto iter = std::filesystem::directory_iterator("/sys/class/video4linux", ec);
    if (ec) return {};
    const auto end = std::filesystem::directory_iterator();

    for (; iter != end; iter.increment(ec)) {
        if (ec) return {};
        std::ifstream nameFile(iter->path() / "name");
        std::string sensorName;
        std::getline(nameFile, sensorName);
        if (sensorName.rfind("arcimx616 ", 0) != 0 && sensorName.rfind("imx616", 0) != 0) {
            continue;
        }

        const auto devPath = std::filesystem::path("/dev") / iter->path().filename();
        const int testFd = ::open(devPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (testFd < 0) continue;

        // Custom hardware ioctl used on Steam Frame camera kernel driver to check sensor connectivity
        uint32_t sensorConnected = 0;
        const int ioctlRes = ::ioctl(testFd, 0x800456c1UL, &sensorConnected);
        ::close(testFd);

        if (ioctlRes == 0) {
            if (sensorConnected == 1) return devPath.string();
        } else {
            // Driver node exists even if ioctl is unsupported in older firmware
            return devPath.string();
        }
    }
    return {};
}

#else

// Non-Linux stub
class SteamFrameV4L2CameraSource::Impl {
public:
    Impl(core::ILogger&, ::vr::IVRSystem*) {}
    bool HasCamera() { return false; }
    bool GetFrameSize(uint32_t&, uint32_t&, uint32_t&) { return false; }
    bool GetIntrinsics(core::tracking::CameraIntrinsics&) { return false; }
    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t&) { return false; }
    bool AcquireStreaming() { return false; }
    void ReleaseStreaming() {}
    ::vr::EVRTrackedCameraError GetFrame(void*, uint32_t, ::vr::CameraVideoStreamFrameHeader_t&) {
        return ::vr::VRTrackedCameraError_NotSupportedForDevice;
    }
};

std::string SteamFrameV4L2CameraSource::FindSteamFrameCameraDevice() {
    return {};
}

#endif

SteamFrameV4L2CameraSource::SteamFrameV4L2CameraSource(core::ILogger& logger, ::vr::IVRSystem* system)
    : impl_(std::make_unique<Impl>(logger, system)) {}

SteamFrameV4L2CameraSource::~SteamFrameV4L2CameraSource() = default;

bool SteamFrameV4L2CameraSource::HasCamera() {
    return impl_->HasCamera();
}

bool SteamFrameV4L2CameraSource::GetFrameSize(uint32_t& width, uint32_t& height, uint32_t& bufferSize) {
    return impl_->GetFrameSize(width, height, bufferSize);
}

bool SteamFrameV4L2CameraSource::GetIntrinsics(core::tracking::CameraIntrinsics& intrinsics) {
    return impl_->GetIntrinsics(intrinsics);
}

bool SteamFrameV4L2CameraSource::GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) {
    return impl_->GetCameraToHeadTransform(outTransform);
}

bool SteamFrameV4L2CameraSource::AcquireStreaming() {
    return impl_->AcquireStreaming();
}

void SteamFrameV4L2CameraSource::ReleaseStreaming() {
    impl_->ReleaseStreaming();
}

::vr::EVRTrackedCameraError SteamFrameV4L2CameraSource::GetFrame(
    void* frameBuffer, uint32_t bufferSize,
    ::vr::CameraVideoStreamFrameHeader_t& header) {
    return impl_->GetFrame(frameBuffer, bufferSize, header);
}

}  // namespace phonecast::platform::openvr
