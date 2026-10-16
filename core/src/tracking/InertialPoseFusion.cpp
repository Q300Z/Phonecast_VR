#include "phonecast/core/tracking/InertialPoseFusion.h"

#include <algorithm>
#include <cmath>

namespace phonecast::core::tracking {

namespace {

// Base rotation: Rx(-90 deg) maps Earth frame (+Z up) to OpenVR Standing space (+Y up)
// Quaternion(w = cos(-pi/4), x = sin(-pi/4), y = 0, z = 0)
constexpr Quaternion kQBase{0.70710678F, -0.70710678F, 0.0F, 0.0F};

} // namespace

InertialPoseFusion::InertialPoseFusion() noexcept
    : params_(), positionFilter_() {}

InertialPoseFusion::InertialPoseFusion(Params params) noexcept
    : params_(params), positionFilter_(params.positionFilterParams) {}


void InertialPoseFusion::IngestOpticalPose(const Pose6D& standingPose) noexcept {
    if (!standingPose.valid) {
        return;
    }

    const double timestampSec = static_cast<double>(standingPose.timestampMicros) * 1e-6;
    lastFilteredPosition_ = positionFilter_.Filter(standingPose.position, timestampSec);
    lastOpticalOrientation_ = standingPose.orientation;
    lastOpticalTimeMicros_ = standingPose.timestampMicros;
    lastConfidence_ = standingPose.confidence;
    hasOptical_ = true;

    // Update yaw boresighting if IMU is available
    if (hasImu_) {
        // Intermediate aligned orientation: R_inter = R_base * R(q_imu)
        const Quaternion qInter = (kQBase * latestImu_.orientation).Normalized();

        // M = R_opt * R_inter^T
        // In quaternion representation: qDiff = q_opt * qInter.Conjugate()
        const Quaternion qDiff = (standingPose.orientation * qInter.Conjugate()).Normalized();

        // Extract yaw (rotation around vertical Y axis):
        // M_02 = 2*(x*z + w*y), M_00 = 1 - 2*(y^2 + z^2)
        const float sinYaw = 2.0F * (qDiff.w * qDiff.y + qDiff.x * qDiff.z);
        const float cosYaw = 1.0F - 2.0F * (qDiff.y * qDiff.y + qDiff.z * qDiff.z);
        const float psiMeas = std::atan2(sinYaw, cosYaw);

        if (!boresightCalibrated_) {
            psiBoresight_ = psiMeas;
            boresightCalibrated_ = true;
        } else {
            // Shortest angular difference
            const float dPsi = std::atan2(std::sin(psiMeas - psiBoresight_), std::cos(psiMeas - psiBoresight_));
            psiBoresight_ += params_.alphaBoresight * dPsi;
        }
    }
}

void InertialPoseFusion::IngestImu(const PhoneImuPacket& imuPacket) noexcept {
    latestImu_ = imuPacket;
    hasImu_ = true;
}

Pose6D InertialPoseFusion::EvaluatePose(uint64_t currentTimestampMicros) noexcept {
    if (!hasOptical_) {
        currentState_ = TrackingState::Lost;
        return Pose6D::Invalid();
    }

    const uint64_t dtMicros = (currentTimestampMicros >= lastOpticalTimeMicros_)
        ? (currentTimestampMicros - lastOpticalTimeMicros_)
        : 0;

    if (dtMicros > params_.maxHoldDurationMicros) {
        currentState_ = TrackingState::Lost;
        Pose6D lostPose = Pose6D::Invalid();
        lostPose.timestampMicros = currentTimestampMicros;
        return lostPose;
    }

    float confidence = lastConfidence_;
    if (dtMicros <= params_.opticalPeriodMicros) {
        currentState_ = TrackingState::Tracking;
    } else {
        currentState_ = TrackingState::Occluded;
        // Linearly decay confidence during occlusion hold
        const float fraction = static_cast<float>(dtMicros) / static_cast<float>(params_.maxHoldDurationMicros);
        confidence = lastConfidence_ * std::max(0.0F, 1.0F - fraction);
    }

    // Orientation: if IMU is available and boresight calibrated, extrapolate with full IMU cadence
    Quaternion fusedRot = lastOpticalOrientation_;
    if (hasImu_ && boresightCalibrated_) {
        const Quaternion qInter = (kQBase * latestImu_.orientation).Normalized();
        const float halfPsi = psiBoresight_ * 0.5F;
        const Quaternion qYaw{std::cos(halfPsi), 0.0F, std::sin(halfPsi), 0.0F};
        fusedRot = (qYaw * qInter).Normalized();
    }

    Pose6D result{};
    result.position = lastFilteredPosition_;
    result.orientation = fusedRot;
    result.valid = true;
    result.confidence = confidence;
    result.timestampMicros = currentTimestampMicros;
    result.state = currentState_;

    return result;
}

TrackingState InertialPoseFusion::GetState() const noexcept {
    return currentState_;
}

float InertialPoseFusion::GetBoresightYawRadians() const noexcept {
    return psiBoresight_;
}

bool InertialPoseFusion::IsBoresightCalibrated() const noexcept {
    return boresightCalibrated_;
}

void InertialPoseFusion::Reset() noexcept {
    positionFilter_.Reset();
    boresightCalibrated_ = false;
    psiBoresight_ = 0.0F;
    hasOptical_ = false;
    lastOpticalTimeMicros_ = 0;
    lastFilteredPosition_ = {};
    lastOpticalOrientation_ = Quaternion::Identity();
    lastConfidence_ = 0.0F;
    hasImu_ = false;
    latestImu_ = {};
    currentState_ = TrackingState::Lost;
}

} // namespace phonecast::core::tracking
