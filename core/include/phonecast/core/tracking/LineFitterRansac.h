#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phonecast::core::tracking {

class LineFitterRansac {
public:
    struct Params {
        float inlierThresholdPixels{1.5F};      // Max distance to consider a point an inlier
        uint32_t maxIterations{40};             // RANSAC iterations
        float minInlierRatio{0.35F};            // Minimum fraction of candidate points required
        uint32_t minInlierCount{10};            // Absolute minimum number of inliers required
        float minSampleSeparationPixels{10.0F}; // Minimum distance between the 2 RANSAC sample points
        float minEigenvalueRatio{15.0F};        // Elongation threshold: lambda_max / (lambda_min + eps)
        uint32_t randomSeed{1337};              // Deterministic seed for reproducible testing

        Params() = default;
    };

    struct ScanlineParams {
        uint32_t numScanlines{40};              // Number of perpendicular scanlines along edge
        float scanlineHalfLengthPixels{16.0F};  // Length = 2 * halfLength + 1
        float minGradientMagnitude{20.0F};     // Minimum contrast threshold (0-255)
        float marginRatio{0.15F};               // Trim outer 15% on both ends to bypass rounded corners

        ScanlineParams() = default;
    };


    LineFitterRansac() noexcept;
    explicit LineFitterRansac(Params params) noexcept;

    // Fits a 2D line to an existing set of candidate points using RANSAC + TLS refinement
    [[nodiscard]] Line2D Fit(const std::vector<Vector2>& points) const;

    // Detects subpixel edge candidates along perpendicular scanlines across a frame segment
    [[nodiscard]] std::vector<Vector2> DetectEdgePoints(
        const CameraFrameView& frame,
        const Vector2& start,
        const Vector2& end) const;

    [[nodiscard]] std::vector<Vector2> DetectEdgePoints(
        const CameraFrameView& frame,
        const Vector2& start,
        const Vector2& end,
        const ScanlineParams& scanParams) const;

    // High-level pipeline: detects edge points along segment and fits robust line
    [[nodiscard]] Line2D DetectAndFit(
        const CameraFrameView& frame,
        const Vector2& start,
        const Vector2& end) const;

    [[nodiscard]] Line2D DetectAndFit(
        const CameraFrameView& frame,
        const Vector2& start,
        const Vector2& end,
        const ScanlineParams& scanParams) const;


    // 3-point parabolic subpixel peak interpolation
    // Returns offset u in [-0.5, +0.5] relative to peak sample
    [[nodiscard]] static float RefineSubpixelParabola(
        float yPrev, float yPeak, float yNext) noexcept;

    // Closed-form Total Least Squares (TLS) orthogonal line fitting on inlier indices
    [[nodiscard]] static Line2D FitTls(
        const std::vector<Vector2>& points,
        const std::vector<size_t>& inlierIndices,
        float minEigenvalueRatio = 15.0F) noexcept;

private:
    Params params_;
};

} // namespace phonecast::core::tracking
