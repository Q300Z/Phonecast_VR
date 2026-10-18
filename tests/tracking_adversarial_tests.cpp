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
#include <random>
#include <string>
#include <vector>

namespace {

int g_totalTests = 0;
int g_failedTests = 0;

void LogTest(bool passed, const std::string& name, const std::string& details = "") {
    ++g_totalTests;
    if (!passed) {
        ++g_failedTests;
        std::cerr << "  [FAIL] " << name;
        if (!details.empty()) {
            std::cerr << " (" << details << ")";
        }
        std::cerr << "\n";
    } else {
        std::cout << "  [PASS] " << name << "\n";
    }
}

float QuaternionAngleDegrees(const phonecast::core::tracking::Quaternion& q1,
                             const phonecast::core::tracking::Quaternion& q2) {
    const float dot = std::abs(q1.w * q2.w + q1.x * q2.x + q1.y * q2.y + q1.z * q2.z);
    const float clamped = std::clamp(dot, -1.0F, 1.0F);
    return 2.0F * std::acos(clamped) * (180.0F / 3.14159265358979323846F);
}

// ============================================================================
// PART 1: PlanarPnpSolver Adversarial Testing
// ============================================================================
void RunPlanarPnpAdversarialTests() {
    using namespace phonecast::core::tracking;
    std::cout << "\n========================================================\n";
    std::cout << "ADVERSARIAL SUITE 1: PlanarPnpSolver & Gravity Ambiguity\n";
    std::cout << "========================================================\n";

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

    const float w = metrics.WidthMeters();
    const float h = metrics.HeightMeters();
    const std::array<Vector3, 4> modelPts = {
        Vector3{-w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F,  h * 0.5F, 0.0F},
        Vector3{ w * 0.5F, -h * 0.5F, 0.0F},
        Vector3{-w * 0.5F, -h * 0.5F, 0.0F}
    };

    PlanarPnpSolver solver;

    // 1.1 Near-fronto-parallel configurations (micro-tilts: 0.0 deg to 5.0 deg)
    std::cout << "\n--- 1.1 Near-Fronto-Parallel Configurations ---\n";
    const std::vector<float> microTiltsDeg = {0.0F, 0.0001F, 0.01F, 0.1F, 0.5F, 1.0F, 2.5F, 5.0F};
    for (float tiltDeg : microTiltsDeg) {
        const float rad = tiltDeg * (3.1415926535F / 180.0F);
        // Tilt around diagonal axis [1, 1, 0]
        const float halfRad = rad * 0.5F;
        const float s = std::sin(halfRad) * 0.70710678F;
        const Quaternion qTilt{std::cos(halfRad), s, s, 0.0F};
        const Vector3 truePos{0.02F, -0.03F, 0.60F};

        ScreenCorners2D corners{};
        corners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = qTilt.Rotate(modelPts[i]) + truePos;
            corners.corners[i] = intrinsics.Project(pCam);
        }

        const auto res = solver.Solve(corners, metrics, intrinsics);
        const float posErr = (res.primaryPose.position - truePos).Norm() * 1000.0F; // mm
        const float rotErr = QuaternionAngleDegrees(res.primaryPose.orientation, qTilt);

        bool pass = res.success && (posErr < 1.0F) && (rotErr < 0.2F) &&
                    !std::isnan(res.secondaryPose.orientation.w) &&
                    !std::isnan(res.secondaryReprojectionRmsePixels);

        LogTest(pass, "Near-fronto-parallel tilt " + std::to_string(tiltDeg) + " deg",
                "posErr=" + std::to_string(posErr) + "mm, rotErr=" + std::to_string(rotErr) + "deg, rmse=" +
                std::to_string(res.primaryReprojectionRmsePixels));
    }

    // 1.2 Extreme Tilts (30 deg to 70 deg)
    std::cout << "\n--- 1.2 Extreme Tilts (30 deg to 70 deg) ---\n";
    const std::vector<float> extremeTiltsDeg = {30.0F, 40.0F, 50.0F, 60.0F, 70.0F};
    for (float tiltDeg : extremeTiltsDeg) {
        const float rad = tiltDeg * (3.1415926535F / 180.0F);
        // Tilt around X axis (pitch)
        const Quaternion qTilt{std::cos(rad * 0.5F), std::sin(rad * 0.5F), 0.0F, 0.0F};
        const Vector3 truePos{0.0F, 0.0F, 0.50F};

        ScreenCorners2D corners{};
        corners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = qTilt.Rotate(modelPts[i]) + truePos;
            corners.corners[i] = intrinsics.Project(pCam);
        }

        const auto res = solver.Solve(corners, metrics, intrinsics);
        const float posErr = (res.primaryPose.position - truePos).Norm() * 1000.0F; // mm
        const float rotErr = QuaternionAngleDegrees(res.primaryPose.orientation, qTilt);

        // Verification of dual planar reflection
        // 1. Primary pose must accurately recover ground truth (< 5mm posErr, < 1.0 deg rotErr)
        // 2. Secondary pose must be a valid, distinct local minimum with reflection angle ~ 2 * theta
        const float expectedDualAngle = 2.0F * tiltDeg;
        const float dualAngleDist = QuaternionAngleDegrees(res.primaryPose.orientation, res.secondaryPose.orientation);
        const float dualAngleDiff = std::abs(dualAngleDist - expectedDualAngle);

        bool pass = res.success && (posErr < 5.0F) && (rotErr < 1.0F) &&
                    res.secondaryPose.valid && (dualAngleDiff < 5.0F) &&
                    !std::isnan(res.secondaryReprojectionRmsePixels);

        LogTest(pass, "Extreme tilt " + std::to_string(tiltDeg) + " deg",
                "posErr=" + std::to_string(posErr) + "mm, rotErr=" + std::to_string(rotErr) +
                "deg, dualAngleDist=" + std::to_string(dualAngleDist) + "deg (expected ~" +
                std::to_string(expectedDualAngle) + "deg), rmse2=" +
                std::to_string(res.secondaryReprojectionRmsePixels));
    }

    // 1.3 Grazing Views (75 deg to 85 deg)
    std::cout << "\n--- 1.3 Grazing Views (75 deg to 85 deg) ---\n";
    const std::vector<float> grazingTiltsDeg = {75.0F, 80.0F, 85.0F};
    for (float tiltDeg : grazingTiltsDeg) {
        const float rad = tiltDeg * (3.1415926535F / 180.0F);
        const Quaternion qTilt{std::cos(rad * 0.5F), std::sin(rad * 0.5F), 0.0F, 0.0F};
        const Vector3 truePos{0.0F, 0.0F, 0.50F};

        ScreenCorners2D corners{};
        corners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = qTilt.Rotate(modelPts[i]) + truePos;
            corners.corners[i] = intrinsics.Project(pCam);
        }

        const auto res = solver.Solve(corners, metrics, intrinsics);
        // Numerical stability check: NO NaNs or Infs even under severe perspective foreshortening
        bool noNan = !std::isnan(res.primaryPose.position.x) && !std::isnan(res.primaryPose.position.y) &&
                     !std::isnan(res.primaryPose.position.z) && !std::isnan(res.primaryPose.orientation.w) &&
                     !std::isnan(res.secondaryPose.position.x) && !std::isnan(res.secondaryPose.orientation.w);

        LogTest(noNan, "Grazing view " + std::to_string(tiltDeg) + " deg numerical stability",
                "success=" + std::to_string(res.success) + ", rmse1=" +
                std::to_string(res.primaryReprojectionRmsePixels) + ", area=" +
                std::to_string(corners.Area()) + "px^2");
    }

    // 1.4 Cheirality Violations (Target or Points Behind Camera)
    std::cout << "\n--- 1.4 Cheirality Violations (Target/Points Behind Camera) ---\n";
    {
        // Case A: Target behind camera at Z = -0.5m
        // Projecting points behind camera using intrinsics.Project returns (0, 0) because p.z <= 1e-4
        ScreenCorners2D behindCorners{};
        behindCorners.valid = true;
        const Vector3 behindPos{0.0F, 0.0F, -0.50F};
        for (size_t i = 0; i < 4; ++i) {
            behindCorners.corners[i] = intrinsics.Project(modelPts[i] + behindPos);
        }

        const auto resBehind = solver.Solve(behindCorners, metrics, intrinsics);
        // All corners collapsed to (0, 0)
        LogTest(!resBehind.success, "Target behind camera (z = -0.5m) safely rejected",
                "success=" + std::to_string(resBehind.success) + ", t.z=" +
                std::to_string(resBehind.primaryPose.position.z));
    }

    {
        // Case B: Tilt placing corners behind the camera plane
        // Target at z = 0.05m, tilted 70 deg around X.
        // Screen half height = 0.075m.
        // Top corners have z = 0.05 - 0.075 * sin(70 deg) = 0.05 - 0.0705 = -0.0205m (BEHIND CAMERA!)
        const float rad = 70.0F * (3.1415926535F / 180.0F);
        const Quaternion qTilt{std::cos(rad * 0.5F), std::sin(rad * 0.5F), 0.0F, 0.0F};
        const Vector3 nearPos{0.0F, 0.0F, 0.05F};

        ScreenCorners2D nearCorners{};
        nearCorners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = qTilt.Rotate(modelPts[i]) + nearPos;
            // If pCam.z <= 0, Project returns {0, 0}
            nearCorners.corners[i] = intrinsics.Project(pCam);
        }

        const auto resNear = solver.Solve(nearCorners, metrics, intrinsics);
        // Reprojection RMSE must flag error (e.g. 1e6 or very large)
        bool safeHandling = (resNear.primaryReprojectionRmsePixels > 10.0F || !resNear.success);
        LogTest(safeHandling, "Corner behind camera detected with high RMSE or rejected",
                "rmse=" + std::to_string(resNear.primaryReprojectionRmsePixels) +
                ", conf=" + std::to_string(resNear.primaryPose.confidence));
    }

    // 1.5 Phone IMU Gravity Scoring: 100% Ambiguity Resolution
    std::cout << "\n--- 1.5 Phone IMU Gravity Scoring Disambiguation (1000 Monte Carlo Trials) ---\n";
    std::mt19937 rng(424242);
    std::uniform_real_distribution<float> angleDist(-60.0F, 60.0F); // Pitch/roll between -60 and +60 deg
    std::uniform_real_distribution<float> yawDist(-180.0F, 180.0F);
    std::uniform_real_distribution<float> zDist(0.30F, 1.20F);
    std::uniform_real_distribution<float> xyDist(-0.15F, 0.15F);

    int disambiguationSuccess = 0;
    int totalTrials = 1000;
    int ambiguousCasesTested = 0;

    for (int trial = 0; trial < totalTrials; ++trial) {
        const float pitch = angleDist(rng) * (3.1415926535F / 180.0F);
        const float roll = angleDist(rng) * (3.1415926535F / 180.0F);
        const float yaw = yawDist(rng) * (3.1415926535F / 180.0F);

        const Quaternion qP{std::cos(pitch * 0.5F), std::sin(pitch * 0.5F), 0.0F, 0.0F};
        const Quaternion qR{std::cos(roll * 0.5F), 0.0F, 0.0F, std::sin(roll * 0.5F)};
        const Quaternion qY{std::cos(yaw * 0.5F), 0.0F, std::sin(yaw * 0.5F), 0.0F};
        const Quaternion trueRot = (qY * qP * qR).Normalized();

        const Vector3 truePos{xyDist(rng), xyDist(rng), zDist(rng)};

        // In Android Earth frame, gravity is [0, 0, -1].
        // In Camera frame, gravity is cameraGravity = [0, 1, 0].
        // Phone body gravity is g_B = trueRot.Conjugate().Rotate(cameraGravity).
        // imuOrientation must satisfy: imuOrientation.Conjugate().Rotate([0, 0, -1]) == g_B.
        // Base alignment: Rx(-90 deg) maps Earth (+Z up, -Z gravity) to Camera (+Y down, +Y gravity):
        // Rx(-90 deg) * [0, 0, -1] = [0, 1, 0].
        // Therefore: cameraGravity = Rx(-90 deg) * [0, 0, -1].
        // And trueRot * g_B = cameraGravity = Rx(-90 deg) * [0, 0, -1].
        // So g_B = (Rx(-90 deg)^T * trueRot)^T * [0, 0, -1]
        // Which means imuOrientation = (Rx(-90 deg)^T * trueRot).
        constexpr float kHalfSqrt2 = 0.70710678F;
        const Quaternion qRxPos90{kHalfSqrt2, kHalfSqrt2, 0.0F, 0.0F}; // Rx(+90 deg)
        const Quaternion imuOrientation = (qRxPos90.Conjugate() * trueRot).Normalized();

        ScreenCorners2D trialCorners{};
        trialCorners.valid = true;
        for (size_t i = 0; i < 4; ++i) {
            const Vector3 pCam = trueRot.Rotate(modelPts[i]) + truePos;
            trialCorners.corners[i] = intrinsics.Project(pCam);
        }

        const auto res = solver.Solve(trialCorners, metrics, intrinsics);
        if (!res.success) {
            continue;
        }

        const float dualSeparationDeg = QuaternionAngleDegrees(res.primaryPose.orientation, res.secondaryPose.orientation);
        if (dualSeparationDeg > 15.0F) {
            ++ambiguousCasesTested;

            // Test A: Normal solver result with IMU orientation = imuOrientation
            const Pose6D chosenA = PlanarPnpSolver::Disambiguate(res, imuOrientation);
            const float errA = QuaternionAngleDegrees(chosenA.orientation, trueRot);

            // Test B: Swap primary and secondary in solver result (simulating DLT picking the dual pose as primary)
            PlanarPnpSolver::Result swappedRes = res;
            std::swap(swappedRes.primaryPose, swappedRes.secondaryPose);
            std::swap(swappedRes.primaryReprojectionRmsePixels, swappedRes.secondaryReprojectionRmsePixels);

            const Pose6D chosenB = PlanarPnpSolver::Disambiguate(swappedRes, imuOrientation);
            const float errB = QuaternionAngleDegrees(chosenB.orientation, trueRot);

            if (errA < 3.0F && errB < 3.0F) {
                ++disambiguationSuccess;
            } else if (ambiguousCasesTested <= 10) {
                const Vector3 g_B = imuOrientation.Conjugate().Rotate(Vector3{0.0F, 0.0F, -1.0F});
                const Vector3 g1 = res.primaryPose.orientation.Rotate(g_B);
                const Vector3 g2 = res.secondaryPose.orientation.Rotate(g_B);
                std::cout << "    [DIAG trial " << trial << "] dualSep=" << dualSeparationDeg
                          << " errA=" << errA << " errB=" << errB
                          << " score1=" << g1.Dot(Vector3{0.0F, 1.0F, 0.0F})
                          << " score2=" << g2.Dot(Vector3{0.0F, 1.0F, 0.0F})
                          << " rmse1=" << res.primaryReprojectionRmsePixels
                          << " rmse2=" << res.secondaryReprojectionRmsePixels << "\n";
            }
        }
    }

    std::cout << "  Ambiguous cases (> 15 deg dual separation): " << ambiguousCasesTested << " / " << totalTrials << "\n";
    std::cout << "  Disambiguation resolution rate: " << disambiguationSuccess << " / " << ambiguousCasesTested << "\n";
    const float rate = static_cast<float>(disambiguationSuccess) / static_cast<float>(ambiguousCasesTested);
    LogTest(rate >= 0.999F, "Phone IMU Gravity Scoring 100% resolves planar ambiguity",
            "Rate=" + std::to_string(rate * 100.0F) + "%");
}

// ============================================================================
// PART 2: InertialPoseFusion Adversarial Testing
// ============================================================================
void RunInertialPoseFusionAdversarialTests() {
    using namespace phonecast::core::tracking;
    std::cout << "\n========================================================\n";
    std::cout << "ADVERSARIAL SUITE 2: InertialPoseFusion Occlusion Lifecycle\n";
    std::cout << "========================================================\n";

    InertialPoseFusion fusion;

    constexpr float kHalfSqrt2 = 0.70710678F;
    const Quaternion qBase{kHalfSqrt2, -kHalfSqrt2, 0.0F, 0.0F};
    const Vector3 startPos{0.10F, 1.20F, -0.60F};

    // 2.1 Optical Occlusion Lifecycle (< 1.5s hold + extrapolation -> recovery vs > 1.5s -> Lost)
    std::cout << "\n--- 2.1 Optical Occlusion Lifecycle & State Machine ---\n";

    // Phase 1: 1.0s of continuous tracking at 60 Hz (dt = 16,666 micros)
    uint64_t t = 1000000; // 1.00s
    for (int frame = 0; frame < 60; ++frame) {
        Pose6D opt{};
        opt.position = startPos;
        opt.orientation = qBase;
        opt.valid = true;
        opt.confidence = 0.95F;
        opt.timestampMicros = t;
        fusion.IngestOpticalPose(opt);

        PhoneImuPacket imu{};
        imu.sequence = static_cast<uint64_t>(frame);
        imu.timestampMicros = t;
        imu.orientation = Quaternion::Identity();
        fusion.IngestImu(imu);

        t += 16666;
    }

    const Pose6D evalActive = fusion.EvaluatePose(t);
    LogTest(evalActive.valid && evalActive.state == TrackingState::Tracking,
            "Active tracking phase: valid=true, state=Tracking",
            "confidence=" + std::to_string(evalActive.confidence));

    // Phase 2: Occlusion begins. Optical frames cease. High-frequency IMU continues at 100 Hz.
    std::cout << "\n--- 2.2 Short Occlusion (< 1.5s): Position Hold, IMU Extrapolation, Monotonic Confidence Decay ---\n";
    const uint64_t occlusionStartMicros = t;
    float lastConfidence = evalActive.confidence;
    bool confidenceStrictlyDecreasing = true;
    bool positionHeldConstant = true;

    // Occlude for 1.2 seconds (72 steps of 16.6ms)
    for (int step = 1; step <= 72; ++step) {
        t += 16666;

        // Feed IMU with slight rotating pitch (0.1 rad / s)
        const float pitch = static_cast<float>(step) * 0.002F;
        PhoneImuPacket imu{};
        imu.timestampMicros = t;
        imu.orientation = Quaternion{std::cos(pitch * 0.5F), std::sin(pitch * 0.5F), 0.0F, 0.0F};
        fusion.IngestImu(imu);

        const Pose6D eval = fusion.EvaluatePose(t);
        const uint64_t dtOccl = t - occlusionStartMicros;

        if (dtOccl > 50000) { // After 50 ms
            if (eval.state != TrackingState::Occluded || !eval.valid) {
                std::cerr << "State mismatch at dt=" << dtOccl << "us: state="
                          << TrackingStateToString(eval.state) << ", valid=" << eval.valid << "\n";
            }
        }

        // Check position hold: position must be exactly equal to last filtered position
        const float posShift = (eval.position - startPos).Norm();
        if (posShift > 1e-4F) {
            positionHeldConstant = false;
        }

        // Check monotonic confidence decay
        if (dtOccl > 50000) {
            if (eval.confidence >= lastConfidence + 1e-6F) {
                confidenceStrictlyDecreasing = false;
            }
            lastConfidence = eval.confidence;
        }
    }

    LogTest(positionHeldConstant, "Position held strictly constant during occlusion (zero positional drift)",
            "startPos=(" + std::to_string(startPos.x) + ", " + std::to_string(startPos.y) + ", " + std::to_string(startPos.z) + ")");
    LogTest(confidenceStrictlyDecreasing, "Confidence decays monotonically during occlusion hold",
            "finalConf=" + std::to_string(lastConfidence));

    // Check IMU extrapolation during occlusion
    const Pose6D evalExtrap = fusion.EvaluatePose(t);
    const float rotExtrapDist = QuaternionAngleDegrees(evalExtrap.orientation, qBase);
    LogTest(rotExtrapDist > 1.0F, "Orientation continues extrapolating with phone IMU during occlusion",
            "rotDelta=" + std::to_string(rotExtrapDist) + "deg");

    // Phase 3: Optical Recovery after 1.2s occlusion (no positional jump)
    std::cout << "\n--- 2.3 Recovery from Occlusion (< 1.5s) ---\n";
    t += 16666;
    Pose6D optRecovered{};
    optRecovered.position = startPos; // Phone remained in same position
    optRecovered.orientation = qBase;
    optRecovered.valid = true;
    optRecovered.confidence = 0.95F;
    optRecovered.timestampMicros = t;
    fusion.IngestOpticalPose(optRecovered);

    const Pose6D evalRecovered = fusion.EvaluatePose(t);
    const float jumpNorm = (evalRecovered.position - evalExtrap.position).Norm() * 1000.0F; // mm
    LogTest(evalRecovered.valid && evalRecovered.state == TrackingState::Tracking,
            "Recovery from occlusion: valid=true, state=Tracking");
    LogTest(jumpNorm < 1e-3F, "Zero positional jump on recovery from stationary occlusion",
            "jump=" + std::to_string(jumpNorm) + "mm");

    // Phase 4: Long Occlusion (> 1.5s) -> Transition to TrackingState::Lost
    std::cout << "\n--- 2.4 Prolonged Occlusion (> 1.5s) Graceful Transition to Lost ---\n";
    // Advance time by 1.6s without optical frames
    t += 1600000;
    PhoneImuPacket imuLost{};
    imuLost.timestampMicros = t;
    imuLost.orientation = Quaternion::Identity();
    fusion.IngestImu(imuLost);

    const Pose6D evalLost = fusion.EvaluatePose(t);
    LogTest(!evalLost.valid && evalLost.state == TrackingState::Lost && evalLost.confidence == 0.0F,
            "Prolonged occlusion (>1.5s) transitions to TrackingState::Lost with valid=false, conf=0",
            "state=" + std::string(TrackingStateToString(evalLost.state)) + ", valid=" +
            std::to_string(evalLost.valid) + ", conf=" + std::to_string(evalLost.confidence));

    // Phase 5: Recovery from TrackingState::Lost
    std::cout << "\n--- 2.5 Optical Re-acquisition from TrackingState::Lost ---\n";
    t += 16666;
    optRecovered.timestampMicros = t;
    fusion.IngestOpticalPose(optRecovered);
    const Pose6D evalRecoveredFromLost = fusion.EvaluatePose(t);
    LogTest(evalRecoveredFromLost.valid && evalRecoveredFromLost.state == TrackingState::Tracking,
            "Re-acquisition from Lost: valid=true, state=Tracking",
            "conf=" + std::to_string(evalRecoveredFromLost.confidence));
}

// ============================================================================
// PART 3: OneEuroFilter Adversarial Testing
// ============================================================================
void RunOneEuroFilterAdversarialTests() {
    using namespace phonecast::core::tracking;
    std::cout << "\n========================================================\n";
    std::cout << "ADVERSARIAL SUITE 3: OneEuroFilter Numerical Stability\n";
    std::cout << "========================================================\n";

    // 3.1 Irregular Timestamps (dt -> 0, dt negative, dt huge)
    std::cout << "\n--- 3.1 Irregular Timestamps (dt -> 0, dt < 0, large dt) ---\n";
    OneEuroFilter1D filter1D;
    double t = 100.0;
    (void)filter1D.Filter(1.0F, t);

    // Call with dt = 0
    float vZeroDt = filter1D.Filter(2.0F, t);
    bool passZeroDt = !std::isnan(vZeroDt) && !std::isinf(vZeroDt);
    LogTest(passZeroDt, "dt = 0 (identical timestamp) does not produce NaN or Inf",
            "val=" + std::to_string(vZeroDt));

    // Call with sub-nanosecond dt = 1e-12
    float vSubNano = filter1D.Filter(2.5F, t + 1e-12);
    bool passSubNano = !std::isnan(vSubNano) && !std::isinf(vSubNano);
    LogTest(passSubNano, "dt = 1e-12s (sub-nanosecond) does not explode",
            "val=" + std::to_string(vSubNano));

    // Call with negative dt (out-of-order timestamps)
    float vNegDt = filter1D.Filter(3.0F, t - 5.0);
    bool passNegDt = !std::isnan(vNegDt) && !std::isinf(vNegDt);
    LogTest(passNegDt, "dt < 0 (out-of-order timestamp) clamped safely",
            "val=" + std::to_string(vNegDt));

    // Call with huge dt = 100,000s
    // Filter clamps dt to 1.0s, yielding a stable asymptotic update towards target without overshoot
    float vHugeDt = filter1D.Filter(50.0F, t + 100000.0);
    bool passHugeDt = !std::isnan(vHugeDt) && !std::isinf(vHugeDt) && (vHugeDt > 3.0F) && (vHugeDt <= 50.0F);
    LogTest(passHugeDt, "dt = 100,000s clamps to max dt safely without overshoot",
            "val=" + std::to_string(vHugeDt));

    // 3.2 Rapid Velocity Reversals (High-Frequency Oscillation & Impulse Shock)
    std::cout << "\n--- 3.2 Rapid Velocity Reversals (Stress Oscillation) ---\n";
    OneEuroFilter1D oscFilter;
    t = 0.0;
    bool oscStable = true;
    for (int i = 0; i < 1000; ++i) {
        const float target = (i % 2 == 0) ? 10.0F : -10.0F;
        const float out = oscFilter.Filter(target, t);
        if (std::isnan(out) || std::isinf(out) || std::abs(out) > 20.0F) {
            oscStable = false;
            break;
        }
        t += 0.01; // 100 Hz
    }
    LogTest(oscStable, "1000 cycles of +/- 10.0 velocity reversal remains bounded and stable");

    // 3.3 Antipodal Quaternion Flips (-q == q)
    std::cout << "\n--- 3.3 Antipodal Quaternion Flips (-q == q) ---\n";
    OneEuroFilterQuaternion filterQ;

    // Test with a 45 degree rotation around axis [1, 2, 3]
    const float axisNorm = std::sqrt(1.0F + 4.0F + 9.0F);
    const float halfTheta = 45.0F * 0.5F * (3.1415926535F / 180.0F);
    const float sinHalf = std::sin(halfTheta);
    const Quaternion qBaseTest{
        std::cos(halfTheta),
        (1.0F / axisNorm) * sinHalf,
        (2.0F / axisNorm) * sinHalf,
        (3.0F / axisNorm) * sinHalf
    };
    const Quaternion qNegTest{-qBaseTest.w, -qBaseTest.x, -qBaseTest.y, -qBaseTest.z};

    // Initialize filter with +q
    t = 0.0;
    filterQ.Filter(qBaseTest, t);

    // Rapidly alternate +q, -q, -q, +q, -q, +q for 100 steps
    bool antipodalStable = true;
    float maxAngularDiscrepancyDeg = 0.0F;

    for (int i = 0; i < 100; ++i) {
        t += 0.016;
        const bool flip = (i % 3 != 0); // Irregular flip pattern
        const Quaternion inQ = flip ? qNegTest : qBaseTest;

        const Quaternion outQ = filterQ.Filter(inQ, t);
        if (std::isnan(outQ.w) || std::isnan(outQ.x) || std::isnan(outQ.y) || std::isnan(outQ.z)) {
            antipodalStable = false;
            break;
        }

        // Physical rotation angle between filtered output and true physical rotation
        const float angDiff = QuaternionAngleDegrees(outQ, qBaseTest);
        maxAngularDiscrepancyDeg = std::max(maxAngularDiscrepancyDeg, angDiff);
        if (angDiff > 1.0F) {
            antipodalStable = false;
        }
    }

    LogTest(antipodalStable, "Antipodal quaternion flip sequence (+q / -q) handled with zero physical perturbation",
            "maxAngleDiff=" + std::to_string(maxAngularDiscrepancyDeg) + "deg");
}

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << "PhoneCast VR Milestone 2: Adversarial Challenge Suite\n";
    std::cout << "========================================================\n";

    RunPlanarPnpAdversarialTests();
    RunInertialPoseFusionAdversarialTests();
    RunOneEuroFilterAdversarialTests();

    std::cout << "\n========================================================\n";
    std::cout << "Adversarial Test Summary:\n";
    std::cout << "Total Checks: " << g_totalTests << "\n";
    std::cout << "Total Failed: " << g_failedTests << "\n";
    std::cout << "Verdict:      " << (g_failedTests == 0 ? "PASSED (APPROVE)" : "FAILED (REQUEST_CHANGES)") << "\n";
    std::cout << "========================================================\n";

    return (g_failedTests == 0) ? 0 : 1;
}
