#include "phonecast/core/tracking/CornerIntersection.h"
#include "phonecast/core/tracking/LineFitterRansac.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void LogTest(const std::string& name, bool pass, const std::string& detail = "") {
    if (pass) {
        ++g_pass;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " -- " << detail;
    }
    std::cout << '\n';
}

// ============================================================================
// Section 1: LineFitterRansac Stress Tests
// ============================================================================

void TestLineFitterOcclusion() {
    std::cout << "\n=== [1.1] LineFitterRansac: Outlier & Occlusion Stress ===\n";
    using namespace phonecast::core::tracking;

    LineFitterRansac fitter;
    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0.0F, 0.3F);

    // True line: x = 100 -> 1*x + 0*y - 100 = 0
    // Generate scattered non-collinear outliers simulating realistic finger / background noise
    auto generatePoints = [&](size_t inlierCount, size_t outlierCount) {
        std::vector<Vector2> pts;
        pts.reserve(inlierCount + outlierCount);
        for (size_t i = 0; i < inlierCount; ++i) {
            float y = 10.0F + static_cast<float>(i) * (200.0F / static_cast<float>(inlierCount));
            pts.push_back({100.0F + noise(rng), y});
        }
        std::uniform_real_distribution<float> xDist(120.0F, 180.0F);
        std::uniform_real_distribution<float> yDist(10.0F, 210.0F);
        for (size_t i = 0; i < outlierCount; ++i) {
            pts.push_back({xDist(rng), yDist(rng)});
        }
        return pts;
    };

    // Test 30% scattered outliers (40 inliers, 17 outliers -> ~30%)
    {
        auto pts = generatePoints(40, 17);
        Line2D line = fitter.Fit(pts);
        bool ok = line.valid && std::abs(line.a) > 0.98F && std::abs(line.b) < 0.15F;
        LogTest("30% Scattered Outlier Occlusion", ok, "a=" + std::to_string(line.a) + ", b=" + std::to_string(line.b));
    }

    // Test 40% scattered outliers (40 inliers, 27 outliers -> ~40%)
    {
        auto pts = generatePoints(40, 27);
        Line2D line = fitter.Fit(pts);
        bool ok = line.valid && std::abs(line.a) > 0.98F && std::abs(line.b) < 0.15F;
        LogTest("40% Scattered Outlier Occlusion", ok, "a=" + std::to_string(line.a) + ", b=" + std::to_string(line.b));
    }

    // Test 50% scattered outliers (35 inliers, 35 outliers -> 50%)
    {
        auto pts = generatePoints(35, 35);
        Line2D line = fitter.Fit(pts);
        bool ok = line.valid && std::abs(line.a) > 0.98F && std::abs(line.b) < 0.15F;
        LogTest("50% Scattered Outlier Occlusion", ok, "a=" + std::to_string(line.a) + ", b=" + std::to_string(line.b));
    }

    // Test 60% scattered outliers (25 inliers, 38 outliers -> ~60% outliers, inlier ratio = 25/63 = 0.397 >= 0.35)
    {
        auto pts = generatePoints(25, 38);
        Line2D line = fitter.Fit(pts);
        bool ok = line.valid && std::abs(line.a) > 0.98F && std::abs(line.b) < 0.15F;
        LogTest("60% Scattered Outlier Occlusion (ratio ~0.40)", ok,
                "valid=" + std::to_string(line.valid) + ", a=" + std::to_string(line.a));
    }

    // Test 70% scattered outliers (20 inliers, 47 outliers -> inlier ratio = 20/67 = 0.298 < minInlierRatio 0.35)
    {
        auto pts = generatePoints(20, 47);
        Line2D line = fitter.Fit(pts);
        // Should gracefully reject because inlier ratio < 0.35
        LogTest("70% Scattered Outlier Occlusion (ratio < 0.35 rejected)", !line.valid,
                "valid=" + std::to_string(line.valid));
    }

    // Realistic curved thumb occlusion covering 40% of the screen border:
    // Screen border is pixels 0 to 400.
    // Thumb covers Y = 120 to 280 (40% of the span).
    // Border points in Y = [0, 120] and Y = [280, 400] are true inliers on x = 100.
    // Thumb points bulge outward into x = [115, 145] in a semicircular arc.
    {
        std::vector<Vector2> thumbPts;
        for (int i = 0; i < 12; ++i) {
            float y = 5.0F + static_cast<float>(i) * 9.0F; // [5, 104]
            thumbPts.push_back({100.0F + noise(rng), y});
        }
        for (int i = 0; i < 12; ++i) {
            float y = 290.0F + static_cast<float>(i) * 9.0F; // [290, 389]
            thumbPts.push_back({100.0F + noise(rng), y});
        }
        for (int i = 0; i < 16; ++i) {
            float theta = 3.14159265F * static_cast<float>(i + 1) / 18.0F;
            float y = 200.0F - 80.0F * std::cos(theta); // Y in [120, 280]
            float x = 100.0F + 35.0F * std::sin(theta); // X bulges to 135
            thumbPts.push_back({x + noise(rng), y});
        }
        Line2D line = fitter.Fit(thumbPts);
        bool ok = line.valid && std::abs(line.a) > 0.98F && std::abs(line.b) < 0.15F;
        LogTest("Realistic 40% Curved Thumb Arc Occlusion", ok,
                "valid=" + std::to_string(line.valid) + ", a=" + std::to_string(line.a) +
                ", b=" + std::to_string(line.b));
    }
}

void TestLineFitterDegeneracy() {
    std::cout << "\n=== [1.2] LineFitterRansac: Degenerate Point Distributions & TLS Rejection ===\n";
    using namespace phonecast::core::tracking;

    LineFitterRansac fitter;
    std::mt19937 rng(42);

    // 1. All identical points
    {
        std::vector<Vector2> pts(30, {50.0F, 50.0F});
        Line2D line = fitter.Fit(pts);
        LogTest("All identical points rejected", !line.valid);
    }

    // 2. Collinear duplicates (15 points at (0, 0) and 15 points at (50, 0))
    {
        std::vector<Vector2> pts;
        for (int i = 0; i < 15; ++i) pts.push_back({0.0F, 0.0F});
        for (int i = 0; i < 15; ++i) pts.push_back({50.0F, 0.0F});
        Line2D line = fitter.Fit(pts);
        bool ok = line.valid && std::abs(line.b) > 0.99F;
        LogTest("Collinear two-cluster duplicates recovers line y=0", ok,
                "valid=" + std::to_string(line.valid) + ", b=" + std::to_string(line.b));
    }

    // 3. Circular cluster of points (uniform disk of radius 15)
    {
        std::vector<Vector2> pts;
        std::uniform_real_distribution<float> angleDist(0.0F, 6.283185F);
        std::uniform_real_distribution<float> radDist(0.0F, 15.0F);
        for (int i = 0; i < 50; ++i) {
            float ang = angleDist(rng);
            float r = std::sqrt(radDist(rng) / 15.0F) * 15.0F;
            pts.push_back({100.0F + r * std::cos(ang), 100.0F + r * std::sin(ang)});
        }
        Line2D line = fitter.Fit(pts);
        LogTest("Circular isotropic cluster rejected", !line.valid,
                "valid=" + std::to_string(line.valid));
    }

    // 4. Test TLS Eigenvalue ratio directly on controlled ellipses
    {
        std::vector<Vector2> ptsSub;
        for (int i = -20; i <= 20; ++i) {
            float u = static_cast<float>(i) * 1.0F;
            float v = (i % 2 == 0 ? 1.0F : -1.0F) * (20.0F / 3.0F);
            ptsSub.push_back({100.0F + u, 100.0F + v});
        }
        std::vector<size_t> inliers(ptsSub.size());
        std::iota(inliers.begin(), inliers.end(), 0);
        Line2D tlsLow = LineFitterRansac::FitTls(ptsSub, inliers, 15.0F);
        LogTest("TLS rejects low aspect ratio (ratio ~9 < 15)", !tlsLow.valid,
                "valid=" + std::to_string(tlsLow.valid));

        std::vector<Vector2> ptsHigh;
        for (int i = -20; i <= 20; ++i) {
            float u = static_cast<float>(i) * 1.0F;
            float v = (i % 2 == 0 ? 1.0F : -1.0F) * 1.5F;
            ptsHigh.push_back({100.0F + u, 100.0F + v});
        }
        std::vector<size_t> inliersHigh(ptsHigh.size());
        std::iota(inliersHigh.begin(), inliersHigh.end(), 0);
        Line2D tlsHigh = LineFitterRansac::FitTls(ptsHigh, inliersHigh, 15.0F);
        LogTest("TLS accepts high aspect ratio (ratio > 15)", tlsHigh.valid,
                "valid=" + std::to_string(tlsHigh.valid));
    }
}

void TestLineFitterScanlines() {
    std::cout << "\n=== [1.3] LineFitterRansac: Scanlines & Zero-Contrast / Noise ===\n";
    using namespace phonecast::core::tracking;

    LineFitterRansac fitter;

    // 1. Zero-contrast frame (uniform gray 128)
    {
        std::vector<uint8_t> flat(200 * 200, 128);
        CameraFrameView frame{flat.data(), 200, 200, 200, 1, 0, 0};
        auto pts = fitter.DetectEdgePoints(frame, {50.0F, 20.0F}, {50.0F, 180.0F});
        LogTest("Zero-contrast frame returns 0 edge points", pts.empty(),
                "points=" + std::to_string(pts.size()));
        Line2D line = fitter.DetectAndFit(frame, {50.0F, 20.0F}, {50.0F, 180.0F});
        LogTest("Zero-contrast DetectAndFit returns invalid line", !line.valid);
    }

    // 2. Pure noise frame (no structured edge)
    {
        std::vector<uint8_t> noiseImg(200 * 200);
        std::mt19937 rng(999);
        std::uniform_int_distribution<uint32_t> dist(0, 255);
        for (auto& p : noiseImg) {
            p = static_cast<uint8_t>(dist(rng));
        }
        CameraFrameView frame{noiseImg.data(), 200, 200, 200, 1, 0, 0};
        Line2D line = fitter.DetectAndFit(frame, {100.0F, 20.0F}, {100.0F, 180.0F});
        LogTest("Pure noise frame returns invalid line", !line.valid,
                "valid=" + std::to_string(line.valid));
    }

    // 3. Step edge with Gaussian noise
    {
        std::vector<uint8_t> noisyStep(200 * 200);
        std::mt19937 rng(123);
        std::normal_distribution<float> noise(0.0F, 10.0F);
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 200; ++x) {
                float base = (x < 100) ? 30.0F : 220.0F;
                float val = std::clamp(base + noise(rng), 0.0F, 255.0F);
                noisyStep[y * 200 + x] = static_cast<uint8_t>(val);
            }
        }
        CameraFrameView frame{noisyStep.data(), 200, 200, 200, 1, 0, 0};
        Line2D line = fitter.DetectAndFit(frame, {100.0F, 20.0F}, {100.0F, 180.0F});
        bool ok = line.valid && std::abs(line.a) > 0.98F;
        LogTest("Noisy step edge (sigma=10, Delta=190) detected correctly", ok,
                "valid=" + std::to_string(line.valid) + ", a=" + std::to_string(line.a));
    }

    // 4. Low-contrast step edge (Delta = 15, below default minGradientMagnitude = 20)
    {
        std::vector<uint8_t> lowContrast(200 * 200);
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 200; ++x) {
                lowContrast[y * 200 + x] = (x < 100) ? 100 : 115;
            }
        }
        CameraFrameView frame{lowContrast.data(), 200, 200, 200, 1, 0, 0};
        auto pts = fitter.DetectEdgePoints(frame, {100.0F, 20.0F}, {100.0F, 180.0F});
        LogTest("Sub-threshold contrast (Delta=15 < 20) rejected", pts.empty(),
                "points=" + std::to_string(pts.size()));
    }
}

void TestLineFitterSafety() {
    std::cout << "\n=== [1.4] LineFitterRansac: Numerical Safety & Graceful Failure ===\n";
    using namespace phonecast::core::tracking;

    LineFitterRansac fitter;

    // 1. Empty points
    {
        std::vector<Vector2> empty;
        Line2D line = fitter.Fit(empty);
        LogTest("Fit on empty vector returns invalid line", !line.valid);
    }

    // 2. Count < minInlierCount (e.g. 5 points)
    {
        std::vector<Vector2> pts = {{0,0}, {10,10}, {20,20}, {30,30}, {40,40}};
        Line2D line = fitter.Fit(pts);
        LogTest("Fit on 5 points (< minInlierCount=10) returns invalid line", !line.valid);
    }

    // 3. Null camera frame
    {
        CameraFrameView nullFrame{};
        auto pts = fitter.DetectEdgePoints(nullFrame, {0,0}, {100,100});
        LogTest("DetectEdgePoints on null frame returns empty", pts.empty());
    }

    // 4. Segment outside frame boundaries
    {
        std::vector<uint8_t> dummy(100 * 100, 50);
        CameraFrameView frame{dummy.data(), 100, 100, 100, 1, 0, 0};
        auto pts = fitter.DetectEdgePoints(frame, {-50.0F, -50.0F}, {-10.0F, -10.0F});
        LogTest("Scanline completely outside frame returns empty", pts.empty());
    }

    // 5. Points with NaN in candidate list
    {
        float nanVal = std::numeric_limits<float>::quiet_NaN();
        std::vector<Vector2> pts;
        for (int i = 0; i < 20; ++i) {
            pts.push_back({10.0F + static_cast<float>(i), 20.0F});
        }
        pts[5] = {nanVal, nanVal};
        Line2D line = fitter.Fit(pts);
        bool safe = !line.valid || (!std::isnan(line.a) && !std::isnan(line.b) && !std::isnan(line.c));
        LogTest("NaN in candidate points handled without NaN propagation", safe,
                "valid=" + std::to_string(line.valid) + ", a=" + std::to_string(line.a));
    }

    // 6. Direct FitTls with NaN points (BUG 1)
    {
        float nanVal = std::numeric_limits<float>::quiet_NaN();
        std::vector<Vector2> nanPts = {
            {nanVal, nanVal}, {1.0F, 2.0F}, {3.0F, 4.0F}, {5.0F, 6.0F}
        };
        std::vector<size_t> inliers = {0, 1, 2, 3};
        Line2D line = LineFitterRansac::FitTls(nanPts, inliers);
        bool pass = !line.valid;
        LogTest("BUG 1 VERIFICATION: Direct FitTls with NaN returns valid=false", pass,
                "valid=" + std::to_string(line.valid) + ", a=" + std::to_string(line.a));
    }

    // 7. RefineSubpixelParabola with NaN and Inf
    {
        float nanVal = std::numeric_limits<float>::quiet_NaN();
        float infVal = std::numeric_limits<float>::infinity();
        float u1 = LineFitterRansac::RefineSubpixelParabola(nanVal, 10.0F, 5.0F);
        float u2 = LineFitterRansac::RefineSubpixelParabola(5.0F, infVal, 5.0F);
        float u3 = LineFitterRansac::RefineSubpixelParabola(10.0F, 10.0F, 10.0F);
        bool safe = !std::isnan(u1) && !std::isnan(u2) && !std::isnan(u3) &&
                    u1 >= -0.5F && u1 <= 0.5F && u2 >= -0.5F && u2 <= 0.5F;
        LogTest("RefineSubpixelParabola NaN/Inf/Flat input safety", safe,
                "u1=" + std::to_string(u1) + ", u2=" + std::to_string(u2) + ", u3=" + std::to_string(u3));
    }

    // 8. DetectEdgePoints with negative scanlineHalfLengthPixels (BUG 3)
    {
        std::vector<uint8_t> dummy(100 * 100, 120);
        CameraFrameView frame{dummy.data(), 100, 100, 100, 1, 0, 0};
        LineFitterRansac::ScanlineParams badParams{};
        badParams.scanlineHalfLengthPixels = -1.0F;
        bool caughtCrash = false;
        try {
            auto pts = fitter.DetectEdgePoints(frame, {10.0F, 10.0F}, {90.0F, 90.0F}, badParams);
            caughtCrash = false; // Returned without exception
        } catch (const std::exception& e) {
            caughtCrash = true;
        }
        LogTest("BUG 3 VERIFICATION: DetectEdgePoints with negative scanline length does NOT crash", !caughtCrash,
                caughtCrash ? "THREW std::exception (unvalidated allocation crash)" : "handled gracefully");
    }
}

// ============================================================================
// Section 2: CornerIntersection Stress Tests
// ============================================================================

void TestCornerParallelLines() {
    std::cout << "\n=== [2.1] CornerIntersection: Parallel & Near-Parallel Conditioning ===\n";
    using namespace phonecast::core::tracking;

    // Line 1: x = 0 -> 1*x + 0*y + 0 = 0
    Line2D l1{1.0F, 0.0F, 0.0F, true};

    // Exactly parallel: x - 10 = 0
    Line2D lPar{1.0F, 0.0F, -10.0F, true};
    auto intPar = CornerIntersection::Intersect(l1, lPar);
    LogTest("Exactly parallel lines rejected (|D| = 0 < 0.15)", !intPar.has_value());

    // Near parallel at small angles:
    // Angle theta such that sin(theta) < 0.15:
    // arcsin(0.15) = 0.150568 rad = 8.6269 deg
    // At 5 deg (sin 5 deg = 0.08715 < 0.15): should be rejected!
    float rad5 = 5.0F * 3.14159265F / 180.0F;
    Line2D l5deg{std::cos(rad5), std::sin(rad5), -10.0F, true};
    auto int5deg = CornerIntersection::Intersect(l1, l5deg);
    LogTest("Near-parallel 5 deg (|D| = 0.087 < 0.15) rejected", !int5deg.has_value());

    // At 8 deg (sin 8 deg = 0.13917 < 0.15): should be rejected!
    float rad8 = 8.0F * 3.14159265F / 180.0F;
    Line2D l8deg{std::cos(rad8), std::sin(rad8), -10.0F, true};
    auto int8deg = CornerIntersection::Intersect(l1, l8deg);
    LogTest("Near-parallel 8 deg (|D| = 0.139 < 0.15) rejected", !int8deg.has_value());

    // At 10 deg (sin 10 deg = 0.1736 > 0.15): should succeed!
    float rad10 = 10.0F * 3.14159265F / 180.0F;
    Line2D l10deg{std::cos(rad10), std::sin(rad10), -10.0F, true};
    auto int10deg = CornerIntersection::Intersect(l1, l10deg);
    LogTest("Conditioned 10 deg (|D| = 0.174 >= 0.15) succeeds", int10deg.has_value(),
            int10deg.has_value() ? ("x=" + std::to_string(int10deg->x) + ", y=" + std::to_string(int10deg->y)) : "none");

    // Perpendicular lines (90 deg, |D| = 1.0)
    Line2D lPerp{0.0F, 1.0F, -50.0F, true};
    auto intPerp = CornerIntersection::Intersect(l1, lPerp);
    bool okPerp = intPerp.has_value() && std::abs(intPerp->x - 0.0F) < 1e-4F && std::abs(intPerp->y - 50.0F) < 1e-4F;
    LogTest("Perpendicular lines (90 deg, |D| = 1.0) exact intersection (0, 50)", okPerp);
}

void TestCornerBowtieAndDegeneracy() {
    std::cout << "\n=== [2.2] CornerIntersection: Bowtie & Degenerate Quad Geometry ===\n";
    using namespace phonecast::core::tracking;

    // 1. Classical Bowtie (Hourglass) quad
    std::array<Vector2, 4> bowtie1 = {
        Vector2{0.0F, 0.0F},
        Vector2{100.0F, 100.0F},
        Vector2{100.0F, 0.0F},
        Vector2{0.0F, 100.0F}
    };
    LogTest("Hourglass bowtie quad rejected", !CornerIntersection::IsConvexQuad(bowtie1));

    // 2. Reflex / Dart quad (non-convex with one interior vertex)
    std::array<Vector2, 4> dart = {
        Vector2{0.0F, 0.0F},
        Vector2{100.0F, 0.0F},
        Vector2{50.0F, 40.0F},
        Vector2{0.0F, 100.0F}
    };
    LogTest("Dart (reflex angle) non-convex quad rejected", !CornerIntersection::IsConvexQuad(dart));

    // 3. Triangle degenerate quad (3 collinear vertices)
    std::array<Vector2, 4> triangle = {
        Vector2{0.0F, 0.0F},
        Vector2{50.0F, 0.0F},
        Vector2{100.0F, 0.0F},
        Vector2{50.0F, 100.0F}
    };
    LogTest("Collinear edge (triangle) degenerate quad rejected", !CornerIntersection::IsConvexQuad(triangle));

    // 4. Degenerate zero-area quad (all 4 vertices identical)
    std::array<Vector2, 4> pointQuad = {
        Vector2{50.0F, 50.0F}, Vector2{50.0F, 50.0F},
        Vector2{50.0F, 50.0F}, Vector2{50.0F, 50.0F}
    };
    LogTest("Zero-area point quad rejected", !CornerIntersection::IsConvexQuad(pointQuad));

    // 5. Quad with area below minAreaPixels (e.g. area = 300 < 400)
    std::array<Vector2, 4> smallQuad = {
        Vector2{0.0F, 0.0F}, Vector2{10.0F, 0.0F},
        Vector2{10.0F, 30.0F}, Vector2{0.0F, 30.0F}
    };
    LogTest("Small area quad (300 px^2 < 400 px^2) rejected", !CornerIntersection::IsConvexQuad(smallQuad, 400.0F));

    // 6. Valid convex quad with area > 400 px^2
    std::array<Vector2, 4> validQuad = {
        Vector2{0.0F, 0.0F}, Vector2{25.0F, 0.0F},
        Vector2{25.0F, 25.0F}, Vector2{0.0F, 25.0F}
    };
    LogTest("Valid convex quad (625 px^2 >= 400 px^2) accepted", CornerIntersection::IsConvexQuad(validQuad, 400.0F));

    // 7. Counter-clockwise inverted borders quad (BUG 4)
    ScreenBorders invertedBorders{};
    invertedBorders.top = Line2D{0.0F, 1.0F, -400.0F, true};     // Actually bottom line
    invertedBorders.bottom = Line2D{0.0F, 1.0F, -100.0F, true};  // Actually top line
    invertedBorders.right = Line2D{1.0F, 0.0F, -500.0F, true};
    invertedBorders.left = Line2D{1.0F, 0.0F, -150.0F, true};
    invertedBorders.allValid = true;
    ScreenCorners2D invertedCorners = CornerIntersection::ComputeCorners(invertedBorders);
    // In screen coordinates (Y down), valid corners MUST be in clockwise order (TL, TR, BR, BL)
    // Inverted borders produce CCW winding which should be rejected
    bool passWinding = !invertedCorners.valid;
    LogTest("BUG 4 VERIFICATION: Inverted/CCW screen quad rejected", passWinding,
            "valid=" + std::to_string(invertedCorners.valid) + " (accepted flipped borders!)");
}

void TestCornerSubpixelAccuracy() {
    std::cout << "\n=== [2.3] CornerIntersection: Subpixel Accuracy Under Line Noise ===\n";
    using namespace phonecast::core::tracking;

    // 1. Algebraic residual precision of Cramer's rule on 5000 line pairs
    std::mt19937 rng(54321);
    std::uniform_real_distribution<float> angleDist(-3.14159F, 3.14159F);
    std::uniform_real_distribution<float> offsetDist(-800.0F, 800.0F);

    double maxResidual = 0.0;
    int validCount = 0;
    for (int i = 0; i < 5000; ++i) {
        float t1 = angleDist(rng);
        float t2 = angleDist(rng);
        Line2D l1{std::cos(t1), std::sin(t1), offsetDist(rng), true};
        Line2D l2{std::cos(t2), std::sin(t2), offsetDist(rng), true};

        auto pt = CornerIntersection::Intersect(l1, l2);
        if (pt.has_value()) {
            ++validCount;
            double r1 = std::abs(l1.a * pt->x + l1.b * pt->y + l1.c);
            double r2 = std::abs(l2.a * pt->x + l2.b * pt->y + l2.c);
            maxResidual = std::max(maxResidual, std::max(r1, r2));
        }
    }
    bool okResidual = maxResidual < 1e-3;
    LogTest("Cramer's rule algebraic precision (|L(P)| < 0.001 px)", okResidual,
            "maxResidual=" + std::to_string(maxResidual) + " px, validPairs=" + std::to_string(validCount));

    // 2. Geometric corner stability under realistic sensor line noise (0.10 px offset, 0.001 rad angle)
    std::normal_distribution<float> angleNoise(0.0F, 0.001F);
    std::normal_distribution<float> normalOffset(0.0F, 0.10F);
    float maxCornerShift = 0.0F;

    for (int trial = 0; trial < 100; ++trial) {
        auto makeNoisyLine = [&](Vector2 mid, Vector2 nomNormal) -> Line2D {
            float ang = angleNoise(rng);
            float ca = std::cos(ang);
            float sa = std::sin(ang);
            float nx = nomNormal.x * ca - nomNormal.y * sa;
            float ny = nomNormal.x * sa + nomNormal.y * ca;
            Vector2 perturbedMid = mid + Vector2{nx, ny} * normalOffset(rng);
            float c = -(nx * perturbedMid.x + ny * perturbedMid.y);
            return Line2D{nx, ny, c, true};
        };

        ScreenBorders borders{};
        borders.top = makeNoisyLine({300.0F, 100.0F}, {0.0F, 1.0F});
        borders.right = makeNoisyLine({500.0F, 500.0F}, {1.0F, 0.0F});
        borders.bottom = makeNoisyLine({300.0F, 900.0F}, {0.0F, 1.0F});
        borders.left = makeNoisyLine({100.0F, 500.0F}, {1.0F, 0.0F});
        borders.allValid = true;

        ScreenCorners2D corners = CornerIntersection::ComputeCorners(borders);
        if (!corners.valid) continue;

        std::array<Vector2, 4> trueCorners = {
            Vector2{100.0F, 100.0F}, Vector2{500.0F, 100.0F},
            Vector2{500.0F, 900.0F}, Vector2{100.0F, 900.0F}
        };

        for (size_t i = 0; i < 4; ++i) {
            float err = (corners.corners[i] - trueCorners[i]).Norm();
            maxCornerShift = std::max(maxCornerShift, err);
        }
    }
    bool okShift = maxCornerShift < 2.0F;
    LogTest("Geometric corner stability under realistic noise (< 2.0 px)", okShift,
            "maxCornerShift=" + std::to_string(maxCornerShift) + " px");
}

void TestCornerSafety() {
    std::cout << "\n=== [2.4] CornerIntersection: Numerical Safety & Graceful Failure ===\n";
    using namespace phonecast::core::tracking;

    // 1. Line marked invalid
    Line2D lValid{1.0F, 0.0F, 0.0F, true};
    Line2D lInvalid{0.0F, 1.0F, 0.0F, false};
    auto intInv = CornerIntersection::Intersect(lValid, lInvalid);
    LogTest("Intersect with invalid line returns nullopt", !intInv.has_value());

    // 2. Zero normal line (a=0, b=0) marked valid
    Line2D lZero{0.0F, 0.0F, 5.0F, true};
    auto intZero = CornerIntersection::Intersect(lValid, lZero);
    LogTest("Intersect with zero-norm line returns nullopt", !intZero.has_value());

    // 3. Lines containing NaN (BUG 2)
    float nanVal = std::numeric_limits<float>::quiet_NaN();
    Line2D lNan{nanVal, 1.0F, 0.0F, true};
    auto intNan = CornerIntersection::Intersect(lValid, lNan);
    bool nanSafe = !intNan.has_value();
    LogTest("BUG 2 VERIFICATION: Intersect with NaN returns nullopt", nanSafe,
            intNan.has_value() ? ("propagated x=" + std::to_string(intNan->x) + ", y=" + std::to_string(intNan->y)) : "nullopt");

    // 4. ComputeCorners with one missing/invalid border
    ScreenBorders bInvalid{};
    bInvalid.top = Line2D{0, 1, -100, true};
    bInvalid.right = Line2D{1, 0, -500, true};
    bInvalid.bottom = Line2D{0, 1, -900, true};
    bInvalid.left = Line2D{1, 0, -100, false};
    bInvalid.allValid = true;
    ScreenCorners2D cInv = CornerIntersection::ComputeCorners(bInvalid);
    LogTest("ComputeCorners with invalid left border returns valid=false", !cInv.valid);

    // 5. ComputeCorners where borders.allValid is false
    ScreenBorders bAllInvalid{};
    bAllInvalid.top = Line2D{0, 1, -100, true};
    bAllInvalid.right = Line2D{1, 0, -500, true};
    bAllInvalid.bottom = Line2D{0, 1, -900, true};
    bAllInvalid.left = Line2D{1, 0, -100, true};
    bAllInvalid.allValid = false;
    ScreenCorners2D cAllInv = CornerIntersection::ComputeCorners(bAllInvalid);
    LogTest("ComputeCorners with allValid=false returns valid=false", !cAllInv.valid);

    // 6. ComputeCorners with NaN line border
    ScreenBorders bNan{};
    bNan.top = Line2D{nanVal, 1, -100, true};
    bNan.right = Line2D{1, 0, -500, true};
    bNan.bottom = Line2D{0, 1, -900, true};
    bNan.left = Line2D{1, 0, -100, true};
    bNan.allValid = true;
    ScreenCorners2D cNan = CornerIntersection::ComputeCorners(bNan);
    LogTest("ComputeCorners with NaN line border returns valid=false", !cNan.valid,
            "valid=" + std::to_string(cNan.valid));
}

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << "PhoneCast VR Milestone 2: Challenger 1 Adversarial Suite\n";
    std::cout << "========================================================\n";

    TestLineFitterOcclusion();
    TestLineFitterDegeneracy();
    TestLineFitterScanlines();
    TestLineFitterSafety();

    TestCornerParallelLines();
    TestCornerBowtieAndDegeneracy();
    TestCornerSubpixelAccuracy();
    TestCornerSafety();

    std::cout << "\n========================================================\n";
    std::cout << "Total Passed: " << g_pass << '\n';
    std::cout << "Total Failed: " << g_fail << '\n';
    std::cout << "========================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
