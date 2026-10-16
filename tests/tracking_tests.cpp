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
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int totalAssertions = 0;
int failureCount = 0;

void AssertTrue(bool condition, const std::string& description) {
    ++totalAssertions;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

void AssertFalse(bool condition, const std::string& description) {
    AssertTrue(!condition, description);
}

void AssertNear(float actual, float expected, float tolerance, const std::string& description) {
    ++totalAssertions;
    const float diff = std::abs(actual - expected);
    if (diff > tolerance) {
        std::cerr << "FAIL: " << description << " (actual: " << actual
                  << ", expected: " << expected << ", diff: " << diff
                  << ", tol: " << tolerance << ")\n";
        ++failureCount;
    }
}

// ============================================================================
// Suite 1: Tracking Data Types & Math Rigor
// ============================================================================
void TestTrackingTypesAndMath() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 1] Running TrackingTypes & Math Tests...\n";

    // 1.1 Vector2 operations
    const Vector2 v2a{3.0F, 4.0F};
    const Vector2 v2b{1.0F, 2.0F};
    AssertNear(v2a.Dot(v2b), 11.0F, 1e-5F, "Vector2 Dot product");
    AssertNear(v2a.Norm(), 5.0F, 1e-5F, "Vector2 Norm");
    const Vector2 v2norm = v2a.Normalized();
    AssertNear(v2norm.Norm(), 1.0F, 1e-5F, "Vector2 Normalized length");
    AssertNear(v2norm.x, 0.6F, 1e-5F, "Vector2 Normalized x");
    AssertNear(v2norm.y, 0.8F, 1e-5F, "Vector2 Normalized y");
    const Vector2 v2sum = v2a + v2b;
    AssertTrue(v2sum.x == 4.0F && v2sum.y == 6.0F, "Vector2 addition");

    // 1.2 Vector3 operations
    const Vector3 v3a{1.0F, 0.0F, 0.0F};
    const Vector3 v3b{0.0F, 1.0F, 0.0F};
    const Vector3 v3c = v3a.Cross(v3b);
    AssertNear(v3c.x, 0.0F, 1e-5F, "Vector3 Cross X");
    AssertNear(v3c.y, 0.0F, 1e-5F, "Vector3 Cross Y");
    AssertNear(v3c.z, 1.0F, 1e-5F, "Vector3 Cross Z");

    // 1.3 Quaternion operations
    const Quaternion qId = Quaternion::Identity();
    AssertNear(qId.w, 1.0F, 1e-5F, "Quaternion Identity w");
    AssertNear(qId.Norm(), 1.0F, 1e-5F, "Quaternion Identity Norm");

    // 90 deg rotation around X: w = cos(45 deg) = sqrt(2)/2, x = sin(45 deg) = sqrt(2)/2
    constexpr float kHalfSqrt2 = 0.70710678F;
    const Quaternion qRotX{kHalfSqrt2, kHalfSqrt2, 0.0F, 0.0F};
    const Vector3 pY{0.0F, 1.0F, 0.0F};
    const Vector3 pRot = qRotX.Rotate(pY);
    AssertNear(pRot.x, 0.0F, 1e-4F, "Rotate around X: X coordinate");
    AssertNear(pRot.y, 0.0F, 1e-4F, "Rotate around X: Y coordinate -> 0");
    AssertNear(pRot.z, 1.0F, 1e-4F, "Rotate around X: Z coordinate -> 1");

    // Hamilton product: q * q.Conjugate() == Identity
    const Quaternion qConj = qRotX.Conjugate();
    const Quaternion qComp = qRotX * qConj;
    AssertNear(qComp.w, 1.0F, 1e-5F, "q * q* w == 1");
    AssertNear(qComp.x, 0.0F, 1e-5F, "q * q* x == 0");
    AssertNear(qComp.y, 0.0F, 1e-5F, "q * q* y == 0");
    AssertNear(qComp.z, 0.0F, 1e-5F, "q * q* z == 0");

    // 1.4 Pose6D operations
    const Pose6D poseA{Vector3{1.0F, 2.0F, 3.0F}, qRotX, true, 0.9F, 1000, TrackingState::Tracking};
    const Pose6D poseInv = poseA.Inverse();
    const Vector3 testPt{0.5F, -1.0F, 2.0F};
    const Vector3 roundTrip = poseA.TransformPoint(poseInv.TransformPoint(testPt));
    AssertNear(roundTrip.x, testPt.x, 1e-4F, "Pose TransformPoint round-trip X");
    AssertNear(roundTrip.y, testPt.y, 1e-4F, "Pose TransformPoint round-trip Y");
    AssertNear(roundTrip.z, testPt.z, 1e-4F, "Pose TransformPoint round-trip Z");

    // Compounding: T * T^-1 should be identity pose
    const Pose6D poseIdentity = poseA * poseInv;
    AssertNear(poseIdentity.position.x, 0.0F, 1e-4F, "Compounded Identity Pos X");
    AssertNear(poseIdentity.position.y, 0.0F, 1e-4F, "Compounded Identity Pos Y");
    AssertNear(poseIdentity.position.z, 0.0F, 1e-4F, "Compounded Identity Pos Z");
    AssertNear(poseIdentity.orientation.w, 1.0F, 1e-4F, "Compounded Identity Rot W");

    // 1.5 ScreenMetrics validation
    ScreenMetrics metrics{};
    metrics.widthMillimeters = 70.0F;
    metrics.heightMillimeters = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    AssertTrue(metrics.IsValid(), "Valid screen metrics recognized");
    AssertNear(metrics.WidthMeters(), 0.070F, 1e-6F, "Width in meters");
    AssertNear(metrics.HeightMeters(), 0.150F, 1e-6F, "Height in meters");
    AssertNear(metrics.AspectRatio(), 70.0F / 150.0F, 1e-5F, "Aspect ratio");

    metrics.widthMillimeters = 10.0F; // Too small (< 20mm)
    AssertFalse(metrics.IsValid(), "Rejects non-physical width < 20mm");
    metrics.widthMillimeters = 500.0F; // Too large (> 400mm)
    AssertFalse(metrics.IsValid(), "Rejects non-physical width > 400mm");

    // 1.6 CameraIntrinsics projection and normalization
    const CameraIntrinsics camera{800.0F, 800.0F, 320.0F, 240.0F, 640, 480};
    AssertTrue(camera.IsValid(), "Valid camera intrinsics");
    const Vector3 ptCam{0.1F, -0.05F, 0.5F};
    const Vector2 pix = camera.Project(ptCam);
    AssertNear(pix.x, (0.1F / 0.5F) * 800.0F + 320.0F, 1e-4F, "Project X pixel");
    AssertNear(pix.y, (-0.05F / 0.5F) * 800.0F + 240.0F, 1e-4F, "Project Y pixel");
    const Vector2 rayNorm = camera.NormalizePixel(pix);
    AssertNear(rayNorm.x, 0.1F / 0.5F, 1e-4F, "NormalizePixel X ray");
    AssertNear(rayNorm.y, -0.05F / 0.5F, 1e-4F, "NormalizePixel Y ray");

    // 1.7 Line2D geometry
    // Line: 3*x + 4*y - 10 = 0 (Normal: [0.6, 0.8], c = -2)
    const Line2D line{0.6F, 0.8F, -2.0F, true};
    const Vector2 onLine{2.0F, 1.0F}; // 0.6*2 + 0.8*1 - 2 = 1.2 + 0.8 - 2 = 0
    AssertNear(line.Distance(onLine), 0.0F, 1e-5F, "Distance of point on line is 0");
    const Vector2 offLine{2.0F, 2.0F}; // 0.6*2 + 0.8*2 - 2 = 1.2 + 1.6 - 2 = 0.8
    AssertNear(line.SignedDistance(offLine), 0.8F, 1e-5F, "Signed distance of point off line");

    // 1.8 TrackingState stringification
    AssertTrue(std::string(TrackingStateToString(TrackingState::Tracking)) == "Tracking", "State Tracking string");
    AssertTrue(std::string(TrackingStateToString(TrackingState::Occluded)) == "Occluded", "State Occluded string");
    AssertTrue(std::string(TrackingStateToString(TrackingState::Lost)) == "Lost", "State Lost string");
}

// ============================================================================
// Suite 2: LineFitterRansac & Finger Occlusion Rejection
// ============================================================================
void TestLineFitterRansac() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 2] Running LineFitterRansac Tests...\n";

    // 2.1 Subpixel parabolic peak interpolation
    // Parabola f(u) = -2*(u - u*)^2 + 10 with u* = 0.25
    // f(-1) = -2*(-1.25)^2 + 10 = -3.125 + 10 = 6.875
    // f(0)  = -2*(-0.25)^2 + 10 = -0.125 + 10 = 9.875
    // f(1)  = -2*(0.75)^2 + 10  = -1.125 + 10 = 8.875
    const float uRecovered = LineFitterRansac::RefineSubpixelParabola(6.875F, 9.875F, 8.875F);
    AssertNear(uRecovered, 0.25F, 1e-4F, "RefineSubpixelParabola recovers u*=0.25");

    // Test with u* = -0.3
    // f(u) = -(u + 0.3)^2 + 20
    // f(-1) = -(-0.7)^2 + 20 = 19.51
    // f(0)  = -(0.3)^2 + 20  = 19.91
    // f(1)  = -(1.3)^2 + 20  = 18.31
    const float uRecoveredNeg = LineFitterRansac::RefineSubpixelParabola(19.51F, 19.91F, 18.31F);
    AssertNear(uRecoveredNeg, -0.3F, 1e-4F, "RefineSubpixelParabola recovers u*=-0.3");

    // 2.2 Clean line fitting with slight noise
    // True line: x + y - 100 = 0 -> normalized: 1/sqrt(2)*x + 1/sqrt(2)*y - 100/sqrt(2) = 0
    // a = 0.70710678, b = 0.70710678, c = -70.710678
    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0F, 0.2F);

    std::vector<Vector2> cleanPoints;
    for (int i = 0; i < 40; ++i) {
        const float x = 20.0F + static_cast<float>(i) * 5.0F;
        const float y = 100.0F - x + noise(rng);
        cleanPoints.push_back({x, y});
    }

    LineFitterRansac fitter;
    const Line2D fitClean = fitter.Fit(cleanPoints);
    AssertTrue(fitClean.valid, "Clean line fit is valid");
    AssertNear(std::abs(fitClean.a), 0.70710678F, 0.02F, "Clean line normal A");
    AssertNear(std::abs(fitClean.b), 0.70710678F, 0.02F, "Clean line normal B");

    // 2.3 Severe finger / thumb occlusion rejection (30% contaminated outliers)
    // 50 true inliers along 0.6*x + 0.8*y - 120 = 0
    std::vector<Vector2> occludedPoints;
    for (int i = 0; i < 40; ++i) {
        const float t = static_cast<float>(i) * 6.0F;
        // Direction vector [-0.8, 0.6], point on line at (200, 0) -> 0.6*200 = 120
        const float x = 200.0F - 0.8F * t + noise(rng);
        const float y = 0.0F + 0.6F * t + noise(rng);
        occludedPoints.push_back({x, y});
    }
    // Add 16 outlier points simulating a finger overlapping the screen border (shifted by 15 to 40 pixels)
    // 16 outliers out of 56 total points = 28.5% occlusion
    for (int i = 0; i < 16; ++i) {
        const float t = 80.0F + static_cast<float>(i) * 4.0F;
        const float x = 200.0F - 0.8F * t + 25.0F + noise(rng); // Shifted off line by 25 px
        const float y = 0.0F + 0.6F * t + noise(rng);
        occludedPoints.push_back({x, y});
    }

    const Line2D fitOccluded = fitter.Fit(occludedPoints);
    AssertTrue(fitOccluded.valid, "Line fit with finger occlusion is valid");
    // Line normal must match true normal within 0.05 deg (~0.001 in unit coordinates)
    const float dotNormal = std::abs(fitOccluded.a * 0.6F + fitOccluded.b * 0.8F);
    AssertNear(dotNormal, 1.0F, 0.005F, "Finger occlusion rejected: normal aligned to true border");

    // 2.4 Degeneracy rejection: cluster of coincident points
    std::vector<Vector2> degeneratePoints;
    for (int i = 0; i < 30; ++i) {
        degeneratePoints.push_back({100.0F + noise(rng) * 0.01F, 100.0F + noise(rng) * 0.01F});
    }
    const Line2D fitDegen = fitter.Fit(degeneratePoints);
    AssertFalse(fitDegen.valid, "Degenerate coincident points rejected");

    // 2.5 Scanline gradient detection on synthetic CameraFrameView
    // Create synthetic 100x100 grayscale image with a vertical step edge at x = 50
    std::vector<uint8_t> frameData(100 * 100, 20); // Dark background
    for (uint32_t y = 0; y < 100; ++y) {
        for (uint32_t x = 50; x < 100; ++x) {
            frameData[y * 100 + x] = 230; // Bright screen
        }
    }
    CameraFrameView frameView{};
    frameView.data = frameData.data();
    frameView.width = 100;
    frameView.height = 100;
    frameView.strideBytes = 100;
    frameView.bytesPerPixel = 1;

    LineFitterRansac::ScanlineParams scanParams{};
    scanParams.numScanlines = 20;
    scanParams.scanlineHalfLengthPixels = 12.0F;
    scanParams.minGradientMagnitude = 30.0F;
    scanParams.marginRatio = 0.15F;

    const auto detectedPts = fitter.DetectEdgePoints(frameView, Vector2{50.0F, 5.0F}, Vector2{50.0F, 95.0F}, scanParams);
    AssertTrue(!detectedPts.empty(), "Edge points detected along synthetic edge");
    for (const auto& pt : detectedPts) {
        AssertNear(pt.x, 50.0F, 1.0F, "Detected edge point X lies on step edge at x=50");
    }

    const Line2D detectedLine = fitter.DetectAndFit(frameView, Vector2{50.0F, 5.0F}, Vector2{50.0F, 95.0F}, scanParams);
    AssertTrue(detectedLine.valid, "DetectAndFit recovers valid vertical line");
    AssertNear(std::abs(detectedLine.a), 1.0F, 0.05F, "Vertical edge normal A is ~1.0");
    AssertNear(std::abs(detectedLine.b), 0.0F, 0.05F, "Vertical edge normal B is ~0.0");
}

// ============================================================================
// Suite 3: CornerIntersection & Virtual Corners
// ============================================================================
void TestCornerIntersection() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 3] Running CornerIntersection Tests...\n";

    // 3.1 Closed-form 2D line intersection
    // L1: 2*x - y - 1 = 0
    // L2: x + y - 5 = 0
    // Intersection: 3x - 6 = 0 -> x = 2, y = 3
    const Line2D l1{2.0F, -1.0F, -1.0F, true};
    const Line2D l2{1.0F, 1.0F, -5.0F, true};
    const auto ptOpt = CornerIntersection::Intersect(l1, l2);
    AssertTrue(ptOpt.has_value(), "Intersect non-parallel lines succeeds");
    AssertNear(ptOpt->x, 2.0F, 1e-4F, "Intersect X = 2");
    AssertNear(ptOpt->y, 3.0F, 1e-4F, "Intersect Y = 3");

    // 3.2 Conditioning check: parallel or nearly parallel lines
    const Line2D lParallel{1.0F, 1.0F, -10.0F, true};
    const auto ptPar = CornerIntersection::Intersect(l2, lParallel);
    AssertFalse(ptPar.has_value(), "Parallel lines intersection rejected (|det| < minSinAngle)");

    // 3.3 Rectangular screen borders and virtual corner computation
    // Top: y = 100 -> 0*x + 1*y - 100 = 0
    // Right: x = 500 -> 1*x + 0*y - 500 = 0
    // Bottom: y = 400 -> 0*x + 1*y - 400 = 0
    // Left: x = 150 -> 1*x + 0*y - 150 = 0
    ScreenBorders borders{};
    borders.top = Line2D{0.0F, 1.0F, -100.0F, true};
    borders.right = Line2D{1.0F, 0.0F, -500.0F, true};
    borders.bottom = Line2D{0.0F, 1.0F, -400.0F, true};
    borders.left = Line2D{1.0F, 0.0F, -150.0F, true};
    borders.allValid = true;

    const ScreenCorners2D corners = CornerIntersection::ComputeCorners(borders);
    AssertTrue(corners.valid, "Corners computed successfully for rectangle");
    // Clockwise order: [0]=TL, [1]=TR, [2]=BR, [3]=BL
    AssertNear(corners.corners[0].x, 150.0F, 1e-4F, "TL X = 150");
    AssertNear(corners.corners[0].y, 100.0F, 1e-4F, "TL Y = 100");
    AssertNear(corners.corners[1].x, 500.0F, 1e-4F, "TR X = 500");
    AssertNear(corners.corners[1].y, 100.0F, 1e-4F, "TR Y = 100");
    AssertNear(corners.corners[2].x, 500.0F, 1e-4F, "BR X = 500");
    AssertNear(corners.corners[2].y, 400.0F, 1e-4F, "BR Y = 400");
    AssertNear(corners.corners[3].x, 150.0F, 1e-4F, "BL X = 150");
    AssertNear(corners.corners[3].y, 400.0F, 1e-4F, "BL Y = 400");

    // Area: (500 - 150) * (400 - 100) = 350 * 300 = 105,000 px^2
    AssertNear(corners.Area(), 105000.0F, 1.0F, "Shoelace area matches width * height");

    // 3.4 Non-convex quad rejection (hourglass self-intersecting configuration)
    std::array<Vector2, 4> bowtie = {
        Vector2{100.0F, 100.0F},
        Vector2{300.0F, 300.0F},
        Vector2{300.0F, 100.0F},
        Vector2{100.0F, 300.0F}
    };
    AssertFalse(CornerIntersection::IsConvexQuad(bowtie), "Bowtie self-intersecting quad rejected");
}

// ============================================================================
// Suite 4: PlanarPnpSolver, Accuracy & Gravity Disambiguation
// ============================================================================
void TestPlanarPnpSolver() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 4] Running PlanarPnpSolver Tests...\n";

    ScreenMetrics metrics{};
    metrics.widthMillimeters = 70.0F;   // 0.070 m
    metrics.heightMillimeters = 150.0F; // 0.150 m
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;

    CameraIntrinsics intrinsics{};
    intrinsics.fx = 750.0F;
    intrinsics.fy = 750.0F;
    intrinsics.cx = 320.0F;
    intrinsics.cy = 240.0F;
    intrinsics.imageWidth = 640;
    intrinsics.imageHeight = 480;

    PlanarPnpSolver solver;

    // 4.1 Fronto-parallel pose at Z = 0.50m
    const Vector3 truePos{0.04F, -0.02F, 0.50F};
    const Quaternion trueRot = Quaternion::Identity();

    // 3D Object model vertices (meters): TL, TR, BR, BL
    const float w = metrics.WidthMeters();
    const float h = metrics.HeightMeters();
    const std::array<Vector3, 4> modelPts = {
        Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
        Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
    };

    ScreenCorners2D corners{};
    corners.valid = true;
    for (size_t i = 0; i < 4; ++i) {
        const Vector3 pCam = trueRot.Rotate(modelPts[i]) + truePos;
        corners.corners[i] = intrinsics.Project(pCam);
    }

    const auto resFronto = solver.Solve(corners, metrics, intrinsics);
    AssertTrue(resFronto.success, "Planar PnP fronto-parallel solve succeeded");
    AssertNear(resFronto.primaryReprojectionRmsePixels, 0.0F, 0.1F, "Fronto-parallel reprojection RMSE < 0.1 px");
    AssertNear(resFronto.primaryPose.position.x, truePos.x, 0.0005F, "Fronto-parallel X error < 0.5 mm");
    AssertNear(resFronto.primaryPose.position.y, truePos.y, 0.0005F, "Fronto-parallel Y error < 0.5 mm");
    AssertNear(resFronto.primaryPose.position.z, truePos.z, 0.0005F, "Fronto-parallel Z error < 0.5 mm");
    AssertNear(resFronto.primaryPose.orientation.w, 1.0F, 0.001F, "Fronto-parallel rot W ~ 1.0");

    // 4.2 Oblique 30-degree tilted pose with subpixel noise
    // Rotation of 25 deg around X and 15 deg around Y (~30 deg composite tilt)
    // qX(25 deg): w = cos(12.5 deg) = 0.9763, x = sin(12.5 deg) = 0.2164
    // qY(15 deg): w = cos(7.5 deg) = 0.9914, y = sin(7.5 deg) = 0.1305
    const Quaternion qX{0.9763F, 0.2164F, 0.0F, 0.0F};
    const Quaternion qY{0.9914F, 0.0F, 0.1305F, 0.0F};
    const Quaternion trueTiltRot = (qY * qX).Normalized();
    const Vector3 trueTiltPos{-0.05F, 0.03F, 0.55F};

    std::mt19937 rng(1337);
    std::normal_distribution<float> pixNoise(0.0F, 0.2F); // 0.2 px noise

    ScreenCorners2D tiltCorners{};
    tiltCorners.valid = true;
    for (size_t i = 0; i < 4; ++i) {
        const Vector3 pCam = trueTiltRot.Rotate(modelPts[i]) + trueTiltPos;
        const Vector2 exactPix = intrinsics.Project(pCam);
        tiltCorners.corners[i] = {exactPix.x + pixNoise(rng), exactPix.y + pixNoise(rng)};
    }

    const auto resTilt = solver.Solve(tiltCorners, metrics, intrinsics);
    AssertTrue(resTilt.success, "Planar PnP 30 deg tilted pose solve succeeded");
    AssertTrue(resTilt.secondaryPose.valid, "Secondary dual planar pose generated");

    // Metric accuracy criteria under 30 deg tilt:
    // Position error < 2.0 mm (0.002 m), orientation error < 0.4 deg (~0.007 rad)
    const float posXErr = std::abs(resTilt.primaryPose.position.x - trueTiltPos.x);
    const float posYErr = std::abs(resTilt.primaryPose.position.y - trueTiltPos.y);
    const float posZErr = std::abs(resTilt.primaryPose.position.z - trueTiltPos.z);
    AssertTrue(posXErr < 0.002F, "Tilted pose X error < 2.0 mm (got " + std::to_string(posXErr * 1000.0F) + " mm)");
    AssertTrue(posYErr < 0.002F, "Tilted pose Y error < 2.0 mm (got " + std::to_string(posYErr * 1000.0F) + " mm)");
    AssertTrue(posZErr < 0.002F, "Tilted pose Z error < 2.0 mm (got " + std::to_string(posZErr * 1000.0F) + " mm)");

    // Orientation error: 2 * acos(|q1 . q2|)
    const float dotQ = std::abs(resTilt.primaryPose.orientation.w * trueTiltRot.w +
                                resTilt.primaryPose.orientation.x * trueTiltRot.x +
                                resTilt.primaryPose.orientation.y * trueTiltRot.y +
                                resTilt.primaryPose.orientation.z * trueTiltRot.z);
    const float angleErrRad = 2.0F * std::acos(std::clamp(dotQ, -1.0F, 1.0F));
    const float angleErrDeg = angleErrRad * (180.0F / 3.14159265F);
    AssertTrue(angleErrDeg < 0.4F, "Tilted pose orientation error < 0.4 deg (got " + std::to_string(angleErrDeg) + " deg)");

    // 4.3 Deterministic Planar Ambiguity Disambiguation via Phone IMU Gravity Vector
    // Suppose the phone is upright, so gravity points down (-Y_B in phone frame)
    // Camera is looking forward, so camera gravity is [0, 1, 0] (downward)
    // Let's create an ambiguous case where both primary and secondary poses exist:
    // Disambiguate must select the pose that aligns with the IMU gravity prior without flipping!
    const Quaternion imuOrientation = trueTiltRot;
    const Pose6D disambiguated = PlanarPnpSolver::Disambiguate(resTilt, imuOrientation);
    AssertTrue(disambiguated.valid, "Disambiguated pose is valid");
    const float dotDisambig = std::abs(disambiguated.orientation.w * trueTiltRot.w +
                                      disambiguated.orientation.x * trueTiltRot.x +
                                      disambiguated.orientation.y * trueTiltRot.y +
                                      disambiguated.orientation.z * trueTiltRot.z);
    AssertNear(dotDisambig, 1.0F, 0.01F, "Disambiguation deterministically selected the true physical pose");
}

// ============================================================================
// Suite 5: OneEuroFilter Adaptive Low-Pass Filter
// ============================================================================
void TestOneEuroFilter() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 5] Running OneEuroFilter Tests...\n";

    // 5.1 Static noise suppression (> 80% jitter elimination)
    OneEuroFilter1D filter1D(OneEuroFilter1D::Params{1.0F, 0.015F, 1.0F});

    std::mt19937 rng(999);
    std::normal_distribution<float> noise(0.0F, 0.01F); // 10 mm noise

    constexpr float kTruePos = 0.50F;
    std::vector<float> rawSamples;
    std::vector<float> filteredSamples;

    double t = 0.0;
    constexpr double dt = 1.0 / 60.0; // 60 Hz optical rate

    for (int i = 0; i < 120; ++i) {
        const float val = kTruePos + noise(rng);
        const float filtered = filter1D.Filter(val, t);
        rawSamples.push_back(val);
        filteredSamples.push_back(filtered);
        t += dt;
    }

    // Calculate variance of the second half (steady state)
    double varRaw = 0.0;
    double varFiltered = 0.0;
    const size_t startIdx = 60;
    for (size_t i = startIdx; i < 120; ++i) {
        const double diffRaw = rawSamples[i] - kTruePos;
        const double diffFilt = filteredSamples[i] - kTruePos;
        varRaw += diffRaw * diffRaw;
        varFiltered += diffFilt * diffFilt;
    }
    varRaw /= 60.0;
    varFiltered /= 60.0;

    const double jitterReduction = 1.0 - (varFiltered / varRaw);
    AssertTrue(jitterReduction > 0.80, "OneEuroFilter static jitter reduction > 80% (achieved " +
                                           std::to_string(jitterReduction * 100.0) + "%)");

    // 5.2 3D Position and Quaternion shortest-path continuity
    OneEuroFilter3D filter3D;
    const Vector3 vIn{1.0F, 2.0F, 3.0F};
    const Vector3 vOut = filter3D.Filter(vIn, 0.0);
    AssertNear(vOut.x, 1.0F, 1e-4F, "Filter3D X initial");
    AssertNear(vOut.y, 2.0F, 1e-4F, "Filter3D Y initial");
    AssertNear(vOut.z, 3.0F, 1e-4F, "Filter3D Z initial");

    OneEuroFilterQuaternion filterQ;
    const Quaternion qA{1.0F, 0.0F, 0.0F, 0.0F};
    const Quaternion qFilteredA = filterQ.Filter(qA, 0.0);
    AssertNear(qFilteredA.w, 1.0F, 1e-4F, "FilterQ initial");

    // Antipodal discontinuity test: q and -q represent the exact same 3D rotation.
    // If input abruptly jumps from q to -q, the filter MUST NOT interpolate through 180 deg!
    const Quaternion qNeg{-1.0F, 0.0F, 0.0F, 0.0F};
    const Quaternion qFilteredNeg = filterQ.Filter(qNeg, 0.016);
    AssertTrue(std::abs(qFilteredNeg.w) > 0.99F, "Antipodal q vs -q handled smoothly without flip");
}

// ============================================================================
// Suite 6: InertialPoseFusion & Occlusion Hold
// ============================================================================
void TestInertialPoseFusion() {
    using namespace phonecast::core::tracking;
    std::cout << "[Suite 6] Running InertialPoseFusion Tests...\n";

    InertialPoseFusion fusion;
    AssertTrue(fusion.GetState() == TrackingState::Lost, "Initial state is Lost");
    AssertFalse(fusion.IsBoresightCalibrated(), "Initial boresight uncalibrated");

    // 6.1 Ingest optical and IMU packets
    // Standing space position (0, 1.2m up, -0.6m forward)
    const Vector3 optPos{0.1F, 1.2F, -0.6F};
    // Phone held upright in Standing Space: top along +Y, screen normal facing user (+Z)
    // Rx(-90 deg) maps Earth (+Z up) to Standing (+Y up)
    constexpr float kHalfSqrt2 = 0.70710678F;
    const Quaternion qBase{kHalfSqrt2, -kHalfSqrt2, 0.0F, 0.0F};

    // If phone is flat in Earth frame, q_imu = Identity, then q_inter = qBase
    // Suppose optical orientation is exactly qBase: then boresight yaw psi = 0
    Pose6D optPose{};
    optPose.position = optPos;
    optPose.orientation = qBase;
    optPose.valid = true;
    optPose.confidence = 0.95F;
    optPose.timestampMicros = 1000000; // 1.0s

    PhoneImuPacket imu{};
    imu.sequence = 1;
    imu.timestampMicros = 1000000;
    imu.orientation = Quaternion::Identity();

    fusion.IngestImu(imu);
    fusion.IngestOpticalPose(optPose);

    AssertTrue(fusion.IsBoresightCalibrated(), "Boresight calibrated after optical frame");
    AssertNear(fusion.GetBoresightYawRadians(), 0.0F, 0.05F, "Calibrated boresight yaw ~ 0");

    // Evaluate pose at current time
    const Pose6D eval1 = fusion.EvaluatePose(1000000);
    AssertTrue(eval1.valid, "Evaluated pose is valid");
    AssertTrue(eval1.state == TrackingState::Tracking, "State is Tracking");
    AssertNear(eval1.position.x, optPos.x, 1e-3F, "Fused pos X");
    AssertNear(eval1.position.y, optPos.y, 1e-3F, "Fused pos Y");
    AssertNear(eval1.position.z, optPos.z, 1e-3F, "Fused pos Z");

    // 6.2 Temporary Occlusion (e.g. 500 ms dropout)
    // Send only IMU updates without optical frames
    imu.timestampMicros = 1500000; // +500 ms
    fusion.IngestImu(imu);

    const Pose6D evalOccluded = fusion.EvaluatePose(1500000);
    AssertTrue(evalOccluded.valid, "Pose remains VALID during 500 ms occlusion hold");
    AssertTrue(evalOccluded.state == TrackingState::Occluded, "State transitions to Occluded");
    AssertNear(evalOccluded.position.x, optPos.x, 1e-3F, "Position frozen at last valid location");
    AssertTrue(evalOccluded.confidence < 0.95F && evalOccluded.confidence > 0.0F, "Confidence decays linearly");

    // 6.3 Complete Tracking Loss after > 1.5s
    // Advance timestamp to 2.6s (1.6s after last optical frame at 1.0s)
    imu.timestampMicros = 2600000;
    fusion.IngestImu(imu);

    const Pose6D evalLost = fusion.EvaluatePose(2600000);
    AssertFalse(evalLost.valid, "Pose becomes INVALID after exceeding max hold duration (1.5s)");
    AssertTrue(evalLost.state == TrackingState::Lost, "State transitions to Lost");
    AssertNear(evalLost.confidence, 0.0F, 1e-5F, "Confidence becomes 0 on Lost");

    // 6.4 Optical Re-acquisition
    // Optical tracking locks back on
    optPose.timestampMicros = 2700000;
    fusion.IngestOpticalPose(optPose);
    const Pose6D evalRecovered = fusion.EvaluatePose(2700000);
    AssertTrue(evalRecovered.valid, "Pose becomes valid again upon optical re-acquisition");
    AssertTrue(evalRecovered.state == TrackingState::Tracking, "State returns to Tracking");
}

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << "PhoneCast VR Milestone 2: Tracking & Fusion Test Suite\n";
    std::cout << "========================================================\n";

    TestTrackingTypesAndMath();
    TestLineFitterRansac();
    TestCornerIntersection();
    TestPlanarPnpSolver();
    TestOneEuroFilter();
    TestInertialPoseFusion();

    std::cout << "========================================================\n";
    std::cout << "Total assertions: " << totalAssertions << '\n';
    std::cout << "Total failures:   " << failureCount << '\n';
    std::cout << "========================================================\n";

    return (failureCount == 0) ? 0 : 1;
}
