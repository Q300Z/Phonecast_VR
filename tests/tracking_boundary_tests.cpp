#include "phonecast/core/tracking/CornerIntersection.h"
#include "phonecast/core/tracking/EigenInterop.h"
#include "phonecast/core/tracking/InertialPoseFusion.h"
#include "phonecast/core/tracking/LineFitterRansac.h"
#include "phonecast/core/tracking/OneEuroFilter.h"
#include "phonecast/core/tracking/PlanarPnpSolver.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const std::string& name, const std::string& detail = "") {
    if (condition) {
        ++g_pass;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_fail;
        std::cerr << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << '\n';
}

void CheckNear(float actual, float expected, float tol, const std::string& name) {
    const float diff = std::abs(actual - expected);
    if (diff <= tol) {
        ++g_pass;
        std::cout << "  [PASS] " << name << '\n';
    } else {
        ++g_fail;
        std::cerr << "  [FAIL] " << name << " (actual: " << actual << ", expected: " << expected
                  << ", diff: " << diff << ", tol: " << tol << ")\n";
    }
}

float QuaternionAngleDegrees(const phonecast::core::tracking::Quaternion& q1,
                             const phonecast::core::tracking::Quaternion& q2) {
    const float dot = std::abs(q1.w * q2.w + q1.x * q2.x + q1.y * q2.y + q1.z * q2.z);
    const float clamped = std::clamp(dot, -1.0F, 1.0F);
    return 2.0F * std::acos(clamped) * (180.0F / 3.14159265358979323846F);
}

// ============================================================================
// SUITE 1: LineFitterRansac Edge Cases & Branch Coverage
// ============================================================================
void TestLineFitterRansacCoverage() {
    std::cout << "\n========================================================\n";
    std::cout << "SUITE 1: LineFitterRansac Edge Cases & Branch Coverage\n";
    std::cout << "========================================================\n";
    using namespace phonecast::core::tracking;

    // 1.1 RefineSubpixelParabola edge conditions
    {
        // Flat input: yPrev = yPeak = yNext
        float uFlat = LineFitterRansac::RefineSubpixelParabola(10.0F, 10.0F, 10.0F);
        CheckNear(uFlat, 0.0F, 1e-6F, "RefineSubpixelParabola flat input returns 0.0");

        // Peak offset exactly at +0.5 boundary
        // f(u) = -a*u^2 + b*u + c
        // For u* = 0.5: yPrev = f(-1) = -a - b + c, yPeak = f(0) = c, yNext = f(1) = -a + b + c.
        // u* = -b / (-2a) = b / (2a) = 0.5 => b = a.
        // Let a = 1, b = 1, c = 10: yPrev = 8, yPeak = 10, yNext = 10.
        // denom = 2*(8 - 20 + 10) = -4. (yPrev - yNext) / denom = (8 - 10) / (-4) = -2 / -4 = 0.5.
        float uBoundaryPlus = LineFitterRansac::RefineSubpixelParabola(8.0F, 10.0F, 10.0F);
        CheckNear(uBoundaryPlus, 0.5F, 1e-5F, "RefineSubpixelParabola u* = +0.5 boundary");

        // u* = -0.5 boundary: a = 1, b = -1, c = 10: yPrev = 10, yPeak = 10, yNext = 8.
        // denom = 2*(10 - 20 + 8) = -4. (yPrev - yNext) / denom = (10 - 8) / (-4) = 2 / -4 = -0.5.
        float uBoundaryMinus = LineFitterRansac::RefineSubpixelParabola(10.0F, 10.0F, 8.0F);
        CheckNear(uBoundaryMinus, -0.5F, 1e-5F, "RefineSubpixelParabola u* = -0.5 boundary");

        // Exceeding +0.5 boundary: yPrev = 5, yPeak = 10, yNext = 12 (not a peak, next > peak)
        // denom = 2*(5 - 20 + 12) = -6. (yPrev - yNext) / denom = (5 - 12) / (-6) = 7/6 = 1.167 > 0.5
        float uExceeds = LineFitterRansac::RefineSubpixelParabola(5.0F, 10.0F, 12.0F);
        CheckNear(uExceeds, 0.0F, 1e-6F, "RefineSubpixelParabola |u| > 0.5 returns 0.0");

        // Inverted trough (minimum): yPrev = 15, yPeak = 5, yNext = 15
        // denom = 2*(15 - 10 + 15) = 40 > 0. (15 - 15) / 40 = 0.
        float uTrough = LineFitterRansac::RefineSubpixelParabola(15.0F, 5.0F, 15.0F);
        CheckNear(uTrough, 0.0F, 1e-6F, "RefineSubpixelParabola trough returns 0.0");

        // Extreme non-finite inputs
        float nanF = std::numeric_limits<float>::quiet_NaN();
        float infF = std::numeric_limits<float>::infinity();
        CheckNear(LineFitterRansac::RefineSubpixelParabola(nanF, 10.0F, 5.0F), 0.0F, 1e-6F, "Parabola yPrev=NaN returns 0.0");
        CheckNear(LineFitterRansac::RefineSubpixelParabola(10.0F, nanF, 5.0F), 0.0F, 1e-6F, "Parabola yPeak=NaN returns 0.0");
        CheckNear(LineFitterRansac::RefineSubpixelParabola(10.0F, 5.0F, nanF), 0.0F, 1e-6F, "Parabola yNext=NaN returns 0.0");
        CheckNear(LineFitterRansac::RefineSubpixelParabola(infF, 10.0F, 5.0F), 0.0F, 1e-6F, "Parabola yPrev=Inf returns 0.0");
        CheckNear(LineFitterRansac::RefineSubpixelParabola(10.0F, -infF, 5.0F), 0.0F, 1e-6F, "Parabola yPeak=-Inf returns 0.0");
    }

    // 1.2 FitTls edge conditions & branch coverage
    {
        // Out-of-bounds inlier index in inlierIndices
        std::vector<Vector2> pts = {{10.0F, 10.0F}, {20.0F, 20.0F}, {30.0F, 30.0F}};
        std::vector<size_t> badIndices = {0, 1, 99}; // 99 >= pts.size()
        Line2D tlsBadIdx = LineFitterRansac::FitTls(pts, badIndices, 15.0F);
        Check(!tlsBadIdx.valid, "FitTls with out-of-bounds inlier index returns invalid line");

        // Negative or NaN minEigenvalueRatio
        std::vector<size_t> validIndices = {0, 1, 2};
        Line2D tlsNegRatio = LineFitterRansac::FitTls(pts, validIndices, -5.0F);
        Check(!tlsNegRatio.valid, "FitTls with negative minEigenvalueRatio returns invalid line");
        float nanF = std::numeric_limits<float>::quiet_NaN();
        Line2D tlsNanRatio = LineFitterRansac::FitTls(pts, validIndices, nanF);
        Check(!tlsNanRatio.valid, "FitTls with NaN minEigenvalueRatio returns invalid line");

        // Single point (count < 2)
        std::vector<size_t> singleIdx = {0};
        Line2D tlsSingle = LineFitterRansac::FitTls(pts, singleIdx, 15.0F);
        Check(!tlsSingle.valid, "FitTls with count < 2 returns invalid line");

        // High coordinate offset: double-precision centering test
        // Points centered at (1,000,000, 2,000,000) along line y = 2,000,000 + 0.5 * (x - 1,000,000)
        std::vector<Vector2> largeCoords;
        for (int i = -20; i <= 20; ++i) {
            float dx = static_cast<float>(i) * 2.0F;
            float dy = 0.5F * dx;
            largeCoords.push_back({1000000.0F + dx, 2000000.0F + dy});
        }
        std::vector<size_t> allIndices(largeCoords.size());
        for (size_t i = 0; i < largeCoords.size(); ++i) allIndices[i] = i;

        Line2D tlsLarge = LineFitterRansac::FitTls(largeCoords, allIndices, 15.0F);
        Check(tlsLarge.valid, "FitTls with coordinates at 10^6 px solves valid line");
        // Line normal for y = 0.5x -> -0.5x + y = 0 -> normalized normal: [-0.5, 1.0] / sqrt(1.25)
        const float expectedA = -0.5F / std::sqrt(1.25F);
        const float expectedB = 1.0F / std::sqrt(1.25F);
        const float dotLarge = std::abs(tlsLarge.a * expectedA + tlsLarge.b * expectedB);
        CheckNear(dotLarge, 1.0F, 1e-4F, "FitTls at 10^6 px maintains subpixel precision without cancellation");
    }

    // 1.3 DetectEdgePoints parameter bounds & edge handling
    {
        std::vector<uint8_t> frameData(100 * 100, 128);
        CameraFrameView frame{frameData.data(), 100, 100, 100, 1, 0, 0};
        LineFitterRansac fitter;

        // Zero length segment (len < 10.0)
        auto ptsShort = fitter.DetectEdgePoints(frame, {50.0F, 50.0F}, {50.0F, 55.0F});
        Check(ptsShort.empty(), "DetectEdgePoints with segment length < 10px returns empty");

        // Negative marginRatio
        LineFitterRansac::ScanlineParams badMargin1{};
        badMargin1.marginRatio = -0.05F;
        auto ptsNegMargin = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, badMargin1);
        Check(ptsNegMargin.empty(), "DetectEdgePoints with marginRatio < 0 returns empty");

        // marginRatio >= 0.5
        LineFitterRansac::ScanlineParams badMargin2{};
        badMargin2.marginRatio = 0.5F;
        auto ptsHighMargin = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, badMargin2);
        Check(ptsHighMargin.empty(), "DetectEdgePoints with marginRatio >= 0.5 returns empty");

        // numScanlines == 0
        LineFitterRansac::ScanlineParams zeroScanlines{};
        zeroScanlines.numScanlines = 0;
        auto ptsZeroScan = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, zeroScanlines);
        Check(ptsZeroScan.empty(), "DetectEdgePoints with numScanlines == 0 returns empty");

        // scanlineHalfLengthPixels < 2.0
        LineFitterRansac::ScanlineParams shortSpan{};
        shortSpan.scanlineHalfLengthPixels = 1.5F;
        auto ptsShortSpan = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, shortSpan);
        Check(ptsShortSpan.empty(), "DetectEdgePoints with scanlineHalfLength < 2.0 returns empty");

        // scanlineHalfLengthPixels > 1024.0
        LineFitterRansac::ScanlineParams hugeSpan{};
        hugeSpan.scanlineHalfLengthPixels = 2048.0F;
        auto ptsHugeSpan = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, hugeSpan);
        Check(ptsHugeSpan.empty(), "DetectEdgePoints with scanlineHalfLength > 1024.0 returns empty");

        // minGradientMagnitude < 0
        LineFitterRansac::ScanlineParams negGrad{};
        negGrad.minGradientMagnitude = -1.0F;
        auto ptsNegGrad = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, negGrad);
        Check(ptsNegGrad.empty(), "DetectEdgePoints with minGradientMagnitude < 0 returns empty");
    }
}

// ============================================================================
// SUITE 2: CornerIntersection Edge Cases & Branch Coverage
// ============================================================================
void TestCornerIntersectionCoverage() {
    std::cout << "\n========================================================\n";
    std::cout << "SUITE 2: CornerIntersection Edge Cases & Branch Coverage\n";
    std::cout << "========================================================\n";
    using namespace phonecast::core::tracking;

    // 2.1 Intersect parameter conditioning & validation
    {
        Line2D l1{1.0F, 0.0F, -50.0F, true}; // x = 50
        Line2D l2{0.0F, 1.0F, -50.0F, true}; // y = 50

        // Negative minSinAngle
        auto ptNeg = CornerIntersection::Intersect(l1, l2, -0.1F);
        Check(!ptNeg.has_value(), "Intersect with negative minSinAngle returns nullopt");

        // NaN minSinAngle
        float nanF = std::numeric_limits<float>::quiet_NaN();
        auto ptNan = CornerIntersection::Intersect(l1, l2, nanF);
        Check(!ptNan.has_value(), "Intersect with NaN minSinAngle returns nullopt");

        // Extremely large line constant c = 10^7 (testing numerical conditioning)
        Line2D lLarge1{1.0F, 0.0F, -10000000.0F, true};
        Line2D lLarge2{0.0F, 1.0F, -20000000.0F, true};
        auto ptLarge = CornerIntersection::Intersect(lLarge1, lLarge2);
        Check(ptLarge.has_value(), "Intersect with c = 10^7 succeeds");
        if (ptLarge.has_value()) {
            CheckNear(ptLarge->x, 10000000.0F, 1.0F, "Large c intersection X = 10^7");
            CheckNear(ptLarge->y, 20000000.0F, 1.0F, "Large c intersection Y = 2*10^7");
        }

        // Exact angular threshold boundary test for minSinAngle = 0.15
        // arcsin(0.15) = 8.6269265 deg
        // At 8.62 deg: sin(8.62 deg) = 0.14988 < 0.15 => nullopt
        // At 8.64 deg: sin(8.64 deg) = 0.15023 >= 0.15 => has_value
        const float radSub = 8.62F * (3.14159265F / 180.0F);
        Line2D lSub{std::cos(radSub), std::sin(radSub), -10.0F, true};
        Check(!CornerIntersection::Intersect(l1, lSub, 0.15F).has_value(),
              "Intersect rejects angle with sin(theta) = 0.14988 < 0.15000");

        const float radSuper = 8.64F * (3.14159265F / 180.0F);
        Line2D lSuper{std::cos(radSuper), std::sin(radSuper), -10.0F, true};
        Check(CornerIntersection::Intersect(l1, lSuper, 0.15F).has_value(),
              "Intersect accepts angle with sin(theta) = 0.15023 >= 0.15000");
    }

    // 2.2 IsConvexQuad boundary and non-convex shapes
    {
        // Negative minAreaPixels
        std::array<Vector2, 4> validBox = {
            Vector2{0.0F, 0.0F}, Vector2{100.0F, 0.0F},
            Vector2{100.0F, 100.0F}, Vector2{0.0F, 100.0F}
        };
        Check(!CornerIntersection::IsConvexQuad(validBox, -10.0F),
              "IsConvexQuad with negative minAreaPixels returns false");

        // NaN minAreaPixels
        float nanF = std::numeric_limits<float>::quiet_NaN();
        Check(!CornerIntersection::IsConvexQuad(validBox, nanF),
              "IsConvexQuad with NaN minAreaPixels returns false");

        // Exactly 3 collinear vertices (degenerate triangle)
        // (0,0) -> (50,0) -> (100,0) are on line y=0. Cross product is exactly 0.
        std::array<Vector2, 4> triangle = {
            Vector2{0.0F, 0.0F}, Vector2{50.0F, 0.0F},
            Vector2{100.0F, 0.0F}, Vector2{50.0F, 80.0F}
        };
        Check(!CornerIntersection::IsConvexQuad(triangle),
              "IsConvexQuad rejects collinear edge (|cross| <= 1e-5)");

        // Area threshold boundary test (minAreaPixels = 400.0)
        // 20x20 box has area = 400.0 px^2 => accepted
        std::array<Vector2, 4> box400 = {
            Vector2{0.0F, 0.0F}, Vector2{20.0F, 0.0F},
            Vector2{20.0F, 20.0F}, Vector2{0.0F, 20.0F}
        };
        Check(CornerIntersection::IsConvexQuad(box400, 400.0F),
              "IsConvexQuad accepts area exactly equal to minAreaPixels (400 px^2)");

        // 19.95 x 20 box has area = 399.0 px^2 => rejected
        std::array<Vector2, 4> box399 = {
            Vector2{0.0F, 0.0F}, Vector2{19.95F, 0.0F},
            Vector2{19.95F, 20.0F}, Vector2{0.0F, 20.0F}
        };
        Check(!CornerIntersection::IsConvexQuad(box399, 400.0F),
              "IsConvexQuad rejects area below minAreaPixels (399 px^2 < 400 px^2)");
    }
}

// ============================================================================
// SUITE 3: PlanarPnpSolver Edge Cases & Branch Coverage
// ============================================================================
void TestPlanarPnpSolverCoverage() {
    std::cout << "\n========================================================\n";
    std::cout << "SUITE 3: PlanarPnpSolver Edge Cases & Branch Coverage\n";
    std::cout << "========================================================\n";
    using namespace phonecast::core::tracking;

    ScreenMetrics metrics{70.0F, 150.0F, 1080, 2400};
    CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};

    const float w = metrics.WidthMeters();
    const float h = metrics.HeightMeters();
    const std::array<Vector3, 4> modelPts = {
        Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
        Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
    };

    PlanarPnpSolver solver;

    // 3.1 Distance boundaries: minPlaneDistanceMeters (0.05m) and maxPlaneDistanceMeters (3.0m)
    {
        // Target at z = 0.04m (below min 0.05m)
        ScreenCorners2D cTooClose{};
        cTooClose.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cTooClose.corners[i] = intrinsics.Project(modelPts[i] + Vector3{0.0F, 0.0F, 0.04F});
        }
        auto resTooClose = solver.Solve(cTooClose, metrics, intrinsics);
        Check(!resTooClose.success, "Solve rejects target at z = 0.04m (< minPlaneDistanceMeters 0.05m)");

        // Target at z = 0.06m (within valid range)
        ScreenCorners2D cValidNear{};
        cValidNear.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cValidNear.corners[i] = intrinsics.Project(modelPts[i] + Vector3{0.0F, 0.0F, 0.06F});
        }
        auto resValidNear = solver.Solve(cValidNear, metrics, intrinsics);
        Check(resValidNear.success, "Solve accepts target at z = 0.06m");

        // Target at z = 3.05m (above max 3.0m)
        ScreenCorners2D cTooFar{};
        cTooFar.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cTooFar.corners[i] = intrinsics.Project(modelPts[i] + Vector3{0.0F, 0.0F, 3.05F});
        }
        auto resTooFar = solver.Solve(cTooFar, metrics, intrinsics);
        Check(!resTooFar.success, "Solve rejects target at z = 3.05m (> maxPlaneDistanceMeters 3.0m)");

        // Target at z = 2.95m (within valid range)
        ScreenCorners2D cValidFar{};
        cValidFar.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cValidFar.corners[i] = intrinsics.Project(modelPts[i] + Vector3{0.0F, 0.0F, 2.95F});
        }
        auto resValidFar = solver.Solve(cValidFar, metrics, intrinsics);
        Check(resValidFar.success, "Solve accepts target at z = 2.95m");
    }

    // 3.2 Degenerate point configurations bypassing CornerIntersection (corners.valid forced true)
    {
        // 4 identical points: (200, 200)
        ScreenCorners2D cIdentical{};
        cIdentical.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            cIdentical.corners[i] = {200.0F, 200.0F};
        }
        auto resIdentical = solver.Solve(cIdentical, metrics, intrinsics);
        // Must handle gracefully without crashing or throwing
        bool safeIdentical = !resIdentical.success || resIdentical.primaryReprojectionRmsePixels > 10.0F;
        Check(safeIdentical, "Solve with 4 identical points handled gracefully without crashing");

        // 4 collinear points along x = 300
        ScreenCorners2D cCollinear{};
        cCollinear.valid = true;
        cCollinear.corners[0] = {300.0F, 100.0F};
        cCollinear.corners[1] = {300.0F, 200.0F};
        cCollinear.corners[2] = {300.0F, 300.0F};
        cCollinear.corners[3] = {300.0F, 400.0F};
        auto resCollinear = solver.Solve(cCollinear, metrics, intrinsics);
        bool safeCollinear = !resCollinear.success || resCollinear.primaryReprojectionRmsePixels > 10.0F;
        Check(safeCollinear, "Solve with 4 collinear points handled gracefully without crashing");
    }

    // 3.3 Disambiguate branches and fallback handling
    {
        // Setup ambiguous case with dual pose
        constexpr float kTiltRad = 35.0F * (3.14159265F / 180.0F);
        const Quaternion qTrue{std::cos(kTiltRad * 0.5F), std::sin(kTiltRad * 0.5F), 0.0F, 0.0F};
        ScreenCorners2D tiltCorners{};
        tiltCorners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            tiltCorners.corners[i] = intrinsics.Project(qTrue.Rotate(modelPts[i]) + Vector3{0.0F, 0.0F, 0.50F});
        }
        auto res = solver.Solve(tiltCorners, metrics, intrinsics);
        Check(res.success && res.secondaryPose.valid, "Oblique tilted solve generated valid dual solutions");

        // Case A: Zero norm camera gravity vector {0, 0, 0}
        const Pose6D pZeroGrav = PlanarPnpSolver::Disambiguate(res, qTrue, Vector3{0.0F, 0.0F, 0.0F});
        Check(pZeroGrav.valid, "Disambiguate with zero camera gravity returns valid pose (falls back to RMSE tie-break)");

        // Case B: Zero quaternion IMU {0, 0, 0, 0}
        const Pose6D pZeroImu = PlanarPnpSolver::Disambiguate(res, Quaternion{0.0F, 0.0F, 0.0F, 0.0F});
        Check(pZeroImu.valid, "Disambiguate with zero IMU quaternion returns valid pose without NaN");

        // Case C: Secondary RMSE artifact rejection (secondaryReprojectionRmsePixels > 2.5 * primary + 2.0)
        PlanarPnpSolver::Result resBadSecondary = res;
        resBadSecondary.primaryReprojectionRmsePixels = 0.2F;
        resBadSecondary.secondaryReprojectionRmsePixels = 5.0F; // 5.0 > 2.5 * 0.2 + 2.0 = 2.5
        // Even if IMU alignment would prefer secondary, algebraic artifact check keeps primary!
        const Quaternion qDualAligned = res.secondaryPose.orientation;
        const Pose6D pKeepPrimary = PlanarPnpSolver::Disambiguate(resBadSecondary, qDualAligned);
        const float dotPrimary = std::abs(pKeepPrimary.orientation.w * res.primaryPose.orientation.w +
                                         pKeepPrimary.orientation.x * res.primaryPose.orientation.x +
                                         pKeepPrimary.orientation.y * res.primaryPose.orientation.y +
                                         pKeepPrimary.orientation.z * res.primaryPose.orientation.z);
        CheckNear(dotPrimary, 1.0F, 1e-4F, "Disambiguate rejects high-RMSE secondary artifact regardless of IMU score");
    }
}

// ============================================================================
// SUITE 4: InertialPoseFusion Edge Cases & Branch Coverage
// ============================================================================
void TestInertialPoseFusionCoverage() {
    std::cout << "\n========================================================\n";
    std::cout << "SUITE 4: InertialPoseFusion Edge Cases & Branch Coverage\n";
    std::cout << "========================================================\n";
    using namespace phonecast::core::tracking;

    InertialPoseFusion fusion;

    // 4.1 Reset() complete state clearing
    {
        Pose6D optPose{Vector3{0.2F, 1.3F, -0.6F}, Quaternion::Identity(), true, 0.9F, 1000000, TrackingState::Tracking};
        PhoneImuPacket imu{0, 1000000, Quaternion::Identity(), {}, {}};
        fusion.IngestImu(imu);
        fusion.IngestOpticalPose(optPose);

        Check(fusion.IsBoresightCalibrated(), "Pre-reset: boresight calibrated");
        const Pose6D preEval = fusion.EvaluatePose(1000000);
        Check(preEval.valid, "Pre-reset: evaluated pose valid");
        Check(fusion.GetState() == TrackingState::Tracking, "Pre-reset: state Tracking");

        // Call Reset()
        fusion.Reset();
        Check(!fusion.IsBoresightCalibrated(), "Post-reset: IsBoresightCalibrated() is false");
        CheckNear(fusion.GetBoresightYawRadians(), 0.0F, 1e-6F, "Post-reset: GetBoresightYawRadians() is 0.0");
        Check(fusion.GetState() == TrackingState::Lost, "Post-reset: GetState() is Lost");
        Check(!fusion.EvaluatePose(1000000).valid, "Post-reset: EvaluatePose() returns invalid pose");

        // Re-ingest optical pose: verify boresight initializes directly without leaky lag
        constexpr float kTargetBoresight = 0.50F;
        const float halfB = kTargetBoresight * 0.5F;
        const Quaternion qYawOffset{std::cos(halfB), 0.0F, std::sin(halfB), 0.0F};
        // Base mapping Rx(-90 deg)
        constexpr float kHalfSqrt2 = 0.70710678F;
        const Quaternion qBase{kHalfSqrt2, -kHalfSqrt2, 0.0F, 0.0F};

        optPose.orientation = qYawOffset * qBase;
        optPose.timestampMicros = 2000000;
        imu.timestampMicros = 2000000;
        imu.orientation = Quaternion::Identity();

        fusion.IngestImu(imu);
        fusion.IngestOpticalPose(optPose);
        Check(fusion.IsBoresightCalibrated(), "Post-reset re-initialization: boresight calibrated");
        CheckNear(fusion.GetBoresightYawRadians(), kTargetBoresight, 0.02F,
                  "Post-reset re-initialization: boresight initialized directly to target yaw without lag");
    }

    // 4.2 Non-monotonic timestamp (timestamp jumps into the past)
    {
        fusion.Reset();
        Pose6D optPose{Vector3{0.0F, 1.0F, -0.5F}, Quaternion::Identity(), true, 1.0F, 1000000, TrackingState::Tracking};
        fusion.IngestOpticalPose(optPose);

        // Evaluate at a timestamp earlier than optical frame (e.g. 500,000 µs < 1,000,000 µs)
        const Pose6D evalPast = fusion.EvaluatePose(500000);
        Check(evalPast.valid, "EvaluatePose with past timestamp handled without underflow");
        Check(evalPast.state == TrackingState::Tracking, "State remains Tracking for dt < 0");
    }

    // 4.3 Exact boundary transitions: 50ms and 1500ms
    {
        fusion.Reset();
        Pose6D optPose{Vector3{0.0F, 1.0F, -0.5F}, Quaternion::Identity(), true, 1.0F, 1000000, TrackingState::Tracking};
        fusion.IngestOpticalPose(optPose);

        // dt = 50,000 µs (exact optical period threshold)
        const Pose6D eval50ms = fusion.EvaluatePose(1050000);
        Check(eval50ms.valid && eval50ms.state == TrackingState::Tracking,
              "dt = 50,000 µs exactly is TrackingState::Tracking");

        // dt = 50,001 µs (exceeds optical period by 1 µs)
        const Pose6D eval50msPlus = fusion.EvaluatePose(1050001);
        Check(eval50msPlus.valid && eval50msPlus.state == TrackingState::Occluded,
              "dt = 50,001 µs transitions to TrackingState::Occluded");

        // dt = 1,500,000 µs (exact max hold duration)
        const Pose6D eval1500ms = fusion.EvaluatePose(2500000);
        Check(eval1500ms.valid && eval1500ms.state == TrackingState::Occluded,
              "dt = 1,500,000 µs exactly is still valid TrackingState::Occluded");
        Check(eval1500ms.confidence >= 0.0F, "Confidence is non-negative at max hold duration");

        // dt = 1,500,001 µs (exceeds max hold duration by 1 µs)
        const Pose6D eval1500msPlus = fusion.EvaluatePose(2500001);
        Check(!eval1500msPlus.valid && eval1500msPlus.state == TrackingState::Lost,
              "dt = 1,500,001 µs transitions to TrackingState::Lost with valid=false");
        CheckNear(eval1500msPlus.confidence, 0.0F, 1e-6F, "Confidence is 0.0 on transition to Lost");
    }

    // 4.4 Full 360-degree rotation handling across multiple turns
    {
        fusion.Reset();
        constexpr float kHalfSqrt2 = 0.70710678F;
        const Quaternion qBase{kHalfSqrt2, -kHalfSqrt2, 0.0F, 0.0F};
        Pose6D optPose{Vector3{0.0F, 1.0F, -0.5F}, qBase, true, 1.0F, 1000000, TrackingState::Tracking};

        PhoneImuPacket imu{0, 1000000, Quaternion::Identity(), {}, {}};
        fusion.IngestImu(imu);
        fusion.IngestOpticalPose(optPose);

        // Rotate IMU smoothly for 3 full rotations (1080 degrees)
        bool allFinite = true;
        uint64_t t = 1000000;
        for (int deg = 0; deg <= 1080; deg += 10) {
            t += 10000; // 10ms
            const float rad = static_cast<float>(deg) * (3.14159265F / 180.0F);
            // Phone rotates around its vertical Y axis in phone frame
            const float halfR = rad * 0.5F;
            imu.orientation = Quaternion{std::cos(halfR), 0.0F, std::sin(halfR), 0.0F};
            imu.timestampMicros = t;
            fusion.IngestImu(imu);

            const Pose6D eval = fusion.EvaluatePose(t);
            if (std::isnan(eval.orientation.w) || std::isnan(eval.orientation.y) ||
                std::isinf(eval.orientation.w) || std::isinf(eval.orientation.y)) {
                allFinite = false;
                break;
            }
        }
        Check(allFinite, "3 full 360-deg rotations (1080 deg) handled continuously without NaN or singularity");
    }
}

// ============================================================================
// SUITE 5: OneEuroFilter Edge Cases & Branch Coverage
// ============================================================================
void TestOneEuroFilterCoverage() {
    std::cout << "\n========================================================\n";
    std::cout << "SUITE 5: OneEuroFilter Edge Cases & Branch Coverage\n";
    std::cout << "========================================================\n";
    using namespace phonecast::core::tracking;

    // 5.1 OneEuroFilter1D Reset & parameter updates
    {
        OneEuroFilter1D filter(OneEuroFilter1D::Params{1.0F, 0.015F, 1.0F});
        Check(!filter.IsInitialized(), "Filter initially uninitialized");

        float v1 = filter.Filter(10.0F, 1.0);
        Check(filter.IsInitialized(), "Filter initialized after first sample");
        CheckNear(v1, 10.0F, 1e-5F, "Initial sample returned unchanged");

        filter.Reset();
        Check(!filter.IsInitialized(), "Filter uninitialized after Reset()");
        float v2 = filter.Filter(50.0F, 5.0);
        Check(filter.IsInitialized(), "Filter re-initialized after reset");
        CheckNear(v2, 50.0F, 1e-5F, "Re-initialized sample returned unchanged");

        // Parameter update
        filter.SetParams(OneEuroFilter1D::Params{2.0F, 0.05F, 1.5F});
        CheckNear(filter.GetParams().fcMin, 2.0F, 1e-5F, "SetParams fcMin updated");
        CheckNear(filter.GetParams().beta, 0.05F, 1e-5F, "SetParams beta updated");
    }

    // 5.2 dt clamping boundaries: [1e-4, 1.0]
    {
        OneEuroFilter1D filter;
        filter.Filter(10.0F, 10.0);

        // dt < 1e-4 (e.g. dt = 1e-6)
        float vSub = filter.Filter(12.0F, 10.000001);
        Check(!std::isnan(vSub) && !std::isinf(vSub), "dt = 1e-6 clamped to 1e-4 safely");

        // dt = 1.0 (exact max boundary)
        float vOneSec = filter.Filter(15.0F, 11.000001);
        Check(!std::isnan(vOneSec) && !std::isinf(vOneSec), "dt = 1.0s processed stably");

        // dt > 1.0 (e.g. dt = 5.0)
        float vFiveSec = filter.Filter(20.0F, 16.000001);
        Check(!std::isnan(vFiveSec) && !std::isinf(vFiveSec), "dt = 5.0s clamped to 1.0s safely");
    }

    // 5.3 OneEuroFilter3D Reset
    {
        OneEuroFilter3D filter3D;
        Check(!filter3D.IsInitialized(), "Filter3D initially uninitialized");
        filter3D.Filter(Vector3{1.0F, 2.0F, 3.0F}, 1.0);
        Check(filter3D.IsInitialized(), "Filter3D initialized after first sample");
        filter3D.Reset();
        Check(!filter3D.IsInitialized(), "Filter3D uninitialized after Reset()");
    }

    // 5.4 OneEuroFilterQuaternion Reset, zero norm and antipodal transitions
    {
        OneEuroFilterQuaternion filterQ;
        Check(!filterQ.IsInitialized(), "FilterQ initially uninitialized");
        filterQ.Filter(Quaternion::Identity(), 1.0);
        Check(filterQ.IsInitialized(), "FilterQ initialized after first sample");
        filterQ.Reset();
        Check(!filterQ.IsInitialized(), "FilterQ uninitialized after Reset()");

        // Zero quaternion input {0, 0, 0, 0}
        Quaternion qZero = filterQ.Filter(Quaternion{0.0F, 0.0F, 0.0F, 0.0F}, 1.0);
        Check(!std::isnan(qZero.w) && !std::isnan(qZero.x), "Zero quaternion input returns valid normalized Identity");
        CheckNear(qZero.w, 1.0F, 1e-5F, "Zero quaternion input normalized to Identity w=1.0");

        // Fast 90-degree step change: verify stable convergence without overshoot
        constexpr float kHalfSqrt2 = 0.70710678F;
        const Quaternion qStep{kHalfSqrt2, kHalfSqrt2, 0.0F, 0.0F};
        double t = 2.0;
        Quaternion lastQ = qZero;
        bool allUnitNorm = true;
        for (int i = 0; i < 30; ++i) {
            t += 0.016;
            lastQ = filterQ.Filter(qStep, t);
            const float n = lastQ.Norm();
            if (std::abs(n - 1.0F) > 1e-4F) {
                allUnitNorm = false;
            }
        }
        Check(allUnitNorm, "All filtered quaternions maintain strict unit norm");
        const float finalDist = QuaternionAngleDegrees(lastQ, qStep);
        Check(finalDist < 1.0F, "Filter smoothly converged to 90-deg step within 30 frames (< 1.0 deg residual)");
    }
}

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "PhoneCast VR - Milestone 4 Challenger 3 Math & Vision Coverage Tests\n";
    std::cout << "Target: phonecast-tracking-coverage-hardening-tests\n";
    std::cout << "======================================================================\n";

    TestLineFitterRansacCoverage();
    TestCornerIntersectionCoverage();
    TestPlanarPnpSolverCoverage();
    TestInertialPoseFusionCoverage();
    TestOneEuroFilterCoverage();

    std::cout << "\n======================================================================\n";
    std::cout << "Coverage Hardening Test Summary:\n";
    std::cout << "Total Passed: " << g_pass << '\n';
    std::cout << "Total Failed: " << g_fail << '\n';
    std::cout << "======================================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
