#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"

namespace phonecast::core::tracking {

class PlanarPnpSolver {
public:
    struct Result {
        Pose6D primaryPose{};
        Pose6D secondaryPose{}; // Planar duality solution
        float primaryReprojectionRmsePixels{0.0F};
        float secondaryReprojectionRmsePixels{0.0F};
        bool success{false};
    };

    struct Params {
        uint32_t maxLhmIterations{10};
        float lhmTolerance{1e-5F};
        float minPlaneDistanceMeters{0.05F};
        float maxPlaneDistanceMeters{3.0F};

        Params() = default;
    };



    PlanarPnpSolver() noexcept;
    explicit PlanarPnpSolver(Params params) noexcept;


    // Computes camera-relative pose of the phone screen from 4 virtual corners,
    // physical dimensions, and camera pinhole intrinsics.
    [[nodiscard]] Result Solve(
        const ScreenCorners2D& corners,
        const ScreenMetrics& metrics,
        const CameraIntrinsics& intrinsics) const;

    // Resolves planar duality using the phone IMU gravity prior.
    // cameraGravity defaults to [0, 1, 0]^T (OpenCV/camera pinhole convention: +Y is down, matching gravity).
    [[nodiscard]] static Pose6D Disambiguate(
        const Result& result,
        const Quaternion& imuOrientation,
        const Vector3& cameraGravity = {0.0F, 1.0F, 0.0F}) noexcept;

private:
    Params params_;
};

} // namespace phonecast::core::tracking
