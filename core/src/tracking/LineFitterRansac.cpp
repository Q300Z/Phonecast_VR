#include "phonecast/core/tracking/LineFitterRansac.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace phonecast::core::tracking {

LineFitterRansac::LineFitterRansac() noexcept : params_() {}

LineFitterRansac::LineFitterRansac(Params params) noexcept : params_(params) {}


float LineFitterRansac::RefineSubpixelParabola(float yPrev, float yPeak, float yNext) noexcept {
    if (!std::isfinite(yPrev) || !std::isfinite(yPeak) || !std::isfinite(yNext)) {
        return 0.0F;
    }
    const float denom = 2.0F * (yPrev - 2.0F * yPeak + yNext);
    if (!std::isfinite(denom) || std::abs(denom) < 1e-6F) {
        return 0.0F;
    }
    const float u = (yPrev - yNext) / denom;
    if (!std::isfinite(u) || u < -0.5F || u > 0.5F) {
        return 0.0F;
    }
    return u;
}

Line2D LineFitterRansac::FitTls(
    const std::vector<Vector2>& points,
    const std::vector<size_t>& inlierIndices,
    float minEigenvalueRatio) noexcept {
    if (points.empty() || !std::isfinite(minEigenvalueRatio) || minEigenvalueRatio < 0.0F) {
        return {};
    }
    const size_t count = inlierIndices.empty() ? points.size() : inlierIndices.size();
    if (count < 2) {
        return {};
    }

    // 1. Centroid calculation with bounds and finiteness validation
    double sumX = 0.0;
    double sumY = 0.0;
    for (size_t idx = 0; idx < count; ++idx) {
        const size_t ptIdx = inlierIndices.empty() ? idx : inlierIndices[idx];
        if (ptIdx >= points.size()) {
            return {};
        }
        const float px = points[ptIdx].x;
        const float py = points[ptIdx].y;
        if (!std::isfinite(px) || !std::isfinite(py)) {
            return {};
        }
        sumX += static_cast<double>(px);
        sumY += static_cast<double>(py);
    }
    const double meanX = sumX / static_cast<double>(count);
    const double meanY = sumY / static_cast<double>(count);
    if (!std::isfinite(meanX) || !std::isfinite(meanY)) {
        return {};
    }

    // 2. Scatter matrix
    double cxx = 0.0;
    double cyy = 0.0;
    double cxy = 0.0;
    for (size_t idx = 0; idx < count; ++idx) {
        const size_t ptIdx = inlierIndices.empty() ? idx : inlierIndices[idx];
        const double dx = static_cast<double>(points[ptIdx].x) - meanX;
        const double dy = static_cast<double>(points[ptIdx].y) - meanY;
        cxx += dx * dx;
        cyy += dy * dy;
        cxy += dx * dy;
    }

    if (!std::isfinite(cxx) || !std::isfinite(cyy) || !std::isfinite(cxy) || (cxx + cyy < 1e-12)) {
        return {};
    }

    // 3. Eigenvalues
    const double diff = cxx - cyy;
    const double disc = diff * diff + 4.0 * cxy * cxy;
    if (!std::isfinite(disc) || disc < 0.0) {
        return {};
    }
    const double root = std::sqrt(disc);
    const double lambdaMax = 0.5 * (cxx + cyy + root);
    const double lambdaMin = 0.5 * (cxx + cyy - root);

    if (!std::isfinite(lambdaMax) || !std::isfinite(lambdaMin) ||
        lambdaMax < 10.0 || lambdaMax / (lambdaMin + 1e-6) < static_cast<double>(minEigenvalueRatio)) {
        return {};
    }

    // 4. Closed-form normal orientation minimizing orthogonal distance
    const double theta = 0.5 * std::atan2(-2.0 * cxy, cyy - cxx);
    const double a = std::cos(theta);
    const double b = std::sin(theta);
    const double c = -(a * meanX + b * meanY);

    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) {
        return {};
    }

    return Line2D{
        static_cast<float>(a),
        static_cast<float>(b),
        static_cast<float>(c),
        true
    };
}

Line2D LineFitterRansac::Fit(const std::vector<Vector2>& points) const {
    const size_t n = points.size();
    if (n < params_.minInlierCount) {
        return {};
    }

    std::mt19937 rng(params_.randomSeed);
    std::uniform_int_distribution<size_t> dist(0, n - 1);

    size_t bestInlierCount = 0;
    float bestSumSqDist = 1e9F;
    std::vector<size_t> bestInliers;
    bestInliers.reserve(n);

    std::vector<size_t> currentInliers;
    currentInliers.reserve(n);

    const float minSepSq = params_.minSampleSeparationPixels * params_.minSampleSeparationPixels;
    const float thresh = params_.inlierThresholdPixels;

    for (uint32_t iter = 0; iter < params_.maxIterations; ++iter) {
        size_t idxA = dist(rng);
        size_t idxB = dist(rng);
        size_t attempts = 0;
        while (idxA == idxB && attempts < 10) {
            idxB = dist(rng);
            ++attempts;
        }
        if (idxA == idxB) {
            continue;
        }

        const Vector2& pA = points[idxA];
        const Vector2& pB = points[idxB];
        const float dx = pB.x - pA.x;
        const float dy = pB.y - pA.y;
        const float sepSq = dx * dx + dy * dy;
        if (!std::isfinite(sepSq) || sepSq < minSepSq) {
            continue;
        }

        const float invLen = 1.0F / std::sqrt(sepSq);
        const float a = -dy * invLen;
        const float b = dx * invLen;
        const float c = -(a * pA.x + b * pA.y);
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) {
            continue;
        }

        currentInliers.clear();
        float sumSqDist = 0.0F;

        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
                continue;
            }
            const float d = std::abs(a * points[i].x + b * points[i].y + c);
            if (d <= thresh) {
                currentInliers.push_back(i);
                sumSqDist += d * d;
            }
        }

        if (currentInliers.size() > bestInlierCount ||
            (currentInliers.size() == bestInlierCount && sumSqDist < bestSumSqDist)) {
            bestInlierCount = currentInliers.size();
            bestSumSqDist = sumSqDist;
            bestInliers = currentInliers;
        }
    }

    const float inlierRatio = static_cast<float>(bestInlierCount) / static_cast<float>(n);
    if (bestInlierCount < params_.minInlierCount || inlierRatio < params_.minInlierRatio) {
        return {};
    }

    return FitTls(points, bestInliers, params_.minEigenvalueRatio);
}

std::vector<Vector2> LineFitterRansac::DetectEdgePoints(
    const CameraFrameView& frame,
    const Vector2& start,
    const Vector2& end,
    const ScanlineParams& scanParams) const {
    std::vector<Vector2> edgePoints;
    if (frame.data == nullptr || frame.width == 0 || frame.height == 0) {
        return edgePoints;
    }

    if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y)) {
        return edgePoints;
    }

    const Vector2 seg = end - start;
    const float len = seg.Norm();
    if (!std::isfinite(len) || len < 10.0F) {
        return edgePoints;
    }

    const Vector2 dir = seg / len;
    const Vector2 normal{-dir.y, dir.x};

    // Trim outer margin to bypass rounded glass corners
    if (!std::isfinite(scanParams.marginRatio) ||
        scanParams.marginRatio < 0.0F || scanParams.marginRatio >= 0.5F ||
        scanParams.numScanlines == 0) {
        return edgePoints;
    }

    const float margin = len * scanParams.marginRatio;
    const Vector2 trimmedStart = start + dir * margin;
    const float effectiveLen = len * (1.0F - 2.0F * scanParams.marginRatio);
    if (!std::isfinite(effectiveLen) || effectiveLen < 5.0F) {
        return edgePoints;
    }

    // Validate scanlineHalfLengthPixels to prevent unsigned underflow or std::length_error
    if (!std::isfinite(scanParams.scanlineHalfLengthPixels) ||
        scanParams.scanlineHalfLengthPixels < 2.0F ||
        scanParams.scanlineHalfLengthPixels > 1024.0F) {
        return edgePoints;
    }

    if (!std::isfinite(scanParams.minGradientMagnitude) ||
        scanParams.minGradientMagnitude < 0.0F) {
        return edgePoints;
    }

    const int32_t halfL = static_cast<int32_t>(scanParams.scanlineHalfLengthPixels);
    const int32_t scanlineSpan = 2 * halfL + 1;
    std::vector<float> luma(static_cast<size_t>(scanlineSpan));
    std::vector<float> grad(static_cast<size_t>(scanlineSpan), 0.0F);

    edgePoints.reserve(scanParams.numScanlines);

    for (uint32_t m = 0; m < scanParams.numScanlines; ++m) {
        const float t = (static_cast<float>(m) + 0.5F) / static_cast<float>(scanParams.numScanlines);
        const Vector2 center = trimmedStart + dir * (t * effectiveLen);

        bool scanlineValid = true;
        for (int32_t s = -halfL; s <= halfL; ++s) {
            const Vector2 samplePos = center + normal * static_cast<float>(s);
            const auto px = static_cast<int32_t>(std::round(samplePos.x));
            const auto py = static_cast<int32_t>(std::round(samplePos.y));

            if (px < 0 || px >= static_cast<int32_t>(frame.width) ||
                py < 0 || py >= static_cast<int32_t>(frame.height)) {
                scanlineValid = false;
                break;
            }
            luma[static_cast<size_t>(s + halfL)] = static_cast<float>(frame.GetLuma(
                static_cast<uint32_t>(px), static_cast<uint32_t>(py)));
        }

        if (!scanlineValid) {
            continue;
        }

        // 1D Central Difference Gradient: |luma[s+1] - luma[s-1]|
        float maxGrad = 0.0F;
        int32_t bestS = 0;
        for (int32_t s = -halfL + 1; s <= halfL - 1; ++s) {
            const size_t idx = static_cast<size_t>(s + halfL);
            const float g = std::abs(luma[idx + 1] - luma[idx - 1]);
            grad[idx] = g;
            if (g > maxGrad) {
                maxGrad = g;
                bestS = s;
            }
        }

        if (maxGrad < scanParams.minGradientMagnitude) {
            continue;
        }

        // Must be strict local maximum and not on boundaries
        if (bestS <= -halfL + 1 || bestS >= halfL - 1) {
            continue;
        }

        const size_t peakIdx = static_cast<size_t>(bestS + halfL);
        const float yPrev = grad[peakIdx - 1];
        const float yPeak = grad[peakIdx];
        const float yNext = grad[peakIdx + 1];

        if (yPeak <= yPrev || yPeak < yNext) {
            continue;
        }

        const float subpixelOffset = RefineSubpixelParabola(yPrev, yPeak, yNext);
        const float refinedS = static_cast<float>(bestS) + subpixelOffset;
        edgePoints.push_back(center + normal * refinedS);
    }

    return edgePoints;
}

std::vector<Vector2> LineFitterRansac::DetectEdgePoints(
    const CameraFrameView& frame,
    const Vector2& start,
    const Vector2& end) const {
    return DetectEdgePoints(frame, start, end, ScanlineParams{});
}

Line2D LineFitterRansac::DetectAndFit(
    const CameraFrameView& frame,
    const Vector2& start,
    const Vector2& end) const {
    return DetectAndFit(frame, start, end, ScanlineParams{});
}

Line2D LineFitterRansac::DetectAndFit(
    const CameraFrameView& frame,
    const Vector2& start,
    const Vector2& end,
    const ScanlineParams& scanParams) const {
    const std::vector<Vector2> edgePoints = DetectEdgePoints(frame, start, end, scanParams);
    return Fit(edgePoints);
}

} // namespace phonecast::core::tracking

