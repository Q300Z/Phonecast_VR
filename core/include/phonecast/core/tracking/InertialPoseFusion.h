#pragma once

#include "phonecast/core/tracking/OneEuroFilter.h"
#include "phonecast/core/tracking/TrackingTypes.h"
#include <cstdint>

namespace phonecast::core::tracking {

class InertialPoseFusion {
public:
    struct Params {
        float alphaBoresight{0.05F};             // Leaky integrator gain for yaw boresighting
        uint64_t maxHoldDurationMicros{1500000};  // 1.5 seconds stable hold on occlusion
        uint64_t opticalPeriodMicros{50000};      // 50 ms (20 Hz threshold) for tracking vs occluded state
        OneEuroFilter1D::Params positionFilterParams{1.0F, 15.0F, 1.0F};

        Params() = default;
    };


    InertialPoseFusion() noexcept;
    explicit InertialPoseFusion(Params params) noexcept;


    // Ingests optical pose expressed in Standing Space (meters and unit quaternion)
    void IngestOpticalPose(const Pose6D& standingPose) noexcept;

    // Ingests high-frequency phone IMU packet (Android sensor TYPE_GAME_ROTATION_VECTOR)
    void IngestImu(const PhoneImuPacket& imuPacket) noexcept;

    // Evaluates the fused 6-DoF pose at the given query timestamp
    [[nodiscard]] Pose6D EvaluatePose(uint64_t currentTimestampMicros) noexcept;

    // Returns current tracking state (Lost, Tracking, Occluded)
    [[nodiscard]] TrackingState GetState() const noexcept;

    // Returns the current calibrated yaw boresight angle in radians
    [[nodiscard]] float GetBoresightYawRadians() const noexcept;

    // Returns whether boresight has been initialized with at least one optical frame
    [[nodiscard]] bool IsBoresightCalibrated() const noexcept;

    // Resets filter state, clearing boresight and position filters
    void Reset() noexcept;

private:
    Params params_;
    OneEuroFilter3D positionFilter_;

    bool boresightCalibrated_{false};
    float psiBoresight_{0.0F};

    bool hasOptical_{false};
    uint64_t lastOpticalTimeMicros_{0};
    Vector3 lastFilteredPosition_{};
    Quaternion lastOpticalOrientation_{Quaternion::Identity()};
    float lastConfidence_{0.0F};

    bool hasImu_{false};
    PhoneImuPacket latestImu_{};

    TrackingState currentState_{TrackingState::Lost};
};

} // namespace phonecast::core::tracking
