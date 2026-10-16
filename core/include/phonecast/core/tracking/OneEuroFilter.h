#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"
#include <algorithm>
#include <cmath>

namespace phonecast::core::tracking {

class OneEuroFilter1D {
public:
    struct Params {
        float fcMin{1.0F};   // Minimum cutoff frequency in Hz
        float beta{0.015F};  // Cutoff slope parameter
        float dFc{1.0F};     // Cutoff frequency for derivative in Hz

        constexpr Params() noexcept = default;
        constexpr Params(float fcMinVal, float betaVal, float dFcVal = 1.0F) noexcept
            : fcMin(fcMinVal), beta(betaVal), dFc(dFcVal) {}
    };



    OneEuroFilter1D() noexcept : params_() {}
    explicit OneEuroFilter1D(Params params) noexcept : params_(params) {}


    float Filter(float value, double timestampSeconds) noexcept {
        if (!initialized_) {
            lastValue_ = value;
            lastDerivative_ = 0.0F;
            lastTimestamp_ = timestampSeconds;
            initialized_ = true;
            return value;
        }

        const float dt = std::clamp(static_cast<float>(timestampSeconds - lastTimestamp_), 1e-4F, 1.0F);

        // 1. Raw derivative
        const float rawDerivative = (value - lastValue_) / dt;

        // 2. Filter derivative
        const float alphaD = ComputeAlpha(params_.dFc, dt);
        const float derivative = alphaD * rawDerivative + (1.0F - alphaD) * lastDerivative_;

        // 3. Dynamic cutoff frequency
        const float fc = params_.fcMin + params_.beta * std::abs(derivative);

        // 4. Filter value
        const float alpha = ComputeAlpha(fc, dt);
        const float filteredValue = alpha * value + (1.0F - alpha) * lastValue_;

        // 5. Update state
        lastValue_ = filteredValue;
        lastDerivative_ = derivative;
        lastTimestamp_ = timestampSeconds;

        return filteredValue;
    }

    void Reset() noexcept {
        initialized_ = false;
        lastValue_ = 0.0F;
        lastDerivative_ = 0.0F;
        lastTimestamp_ = 0.0;
    }

    [[nodiscard]] bool IsInitialized() const noexcept {
        return initialized_;
    }

    [[nodiscard]] const Params& GetParams() const noexcept {
        return params_;
    }

    void SetParams(const Params& params) noexcept {
        params_ = params;
    }

private:
    [[nodiscard]] static float ComputeAlpha(float fc, float dt) noexcept {
        constexpr float kPi = 3.14159265358979323846F;
        const float tau = 1.0F / (2.0F * kPi * fc);
        return 1.0F / (1.0F + tau / dt);
    }

    Params params_;
    bool initialized_{false};
    float lastValue_{0.0F};
    float lastDerivative_{0.0F};
    double lastTimestamp_{0.0};
};

class OneEuroFilter3D {
public:
    OneEuroFilter3D() noexcept : xFilter_(), yFilter_(), zFilter_() {}
    explicit OneEuroFilter3D(OneEuroFilter1D::Params params) noexcept
        : xFilter_(params), yFilter_(params), zFilter_(params) {}

    Vector3 Filter(const Vector3& val, double timestampSeconds) noexcept {
        return {
            xFilter_.Filter(val.x, timestampSeconds),
            yFilter_.Filter(val.y, timestampSeconds),
            zFilter_.Filter(val.z, timestampSeconds)
        };
    }

    void Reset() noexcept {
        xFilter_.Reset();
        yFilter_.Reset();
        zFilter_.Reset();
    }

    [[nodiscard]] bool IsInitialized() const noexcept {
        return xFilter_.IsInitialized();
    }

    void SetParams(const OneEuroFilter1D::Params& params) noexcept {
        xFilter_.SetParams(params);
        yFilter_.SetParams(params);
        zFilter_.SetParams(params);
    }

private:
    OneEuroFilter1D xFilter_;
    OneEuroFilter1D yFilter_;
    OneEuroFilter1D zFilter_;
};

class OneEuroFilterQuaternion {
public:
    OneEuroFilterQuaternion() noexcept : wFilter_(), xFilter_(), yFilter_(), zFilter_() {}
    explicit OneEuroFilterQuaternion(OneEuroFilter1D::Params params) noexcept
        : wFilter_(params), xFilter_(params), yFilter_(params), zFilter_(params) {}


    Quaternion Filter(const Quaternion& q, double timestampSeconds) noexcept {
        Quaternion target = q;
        if (initialized_) {
            // Enforce shortest path (antipodal continuity: q and -q represent the same rotation)
            const float dot = target.w * lastQ_.w + target.x * lastQ_.x +
                              target.y * lastQ_.y + target.z * lastQ_.z;
            if (dot < 0.0F) {
                target.w = -target.w;
                target.x = -target.x;
                target.y = -target.y;
                target.z = -target.z;
            }
        }

        const float fw = wFilter_.Filter(target.w, timestampSeconds);
        const float fx = xFilter_.Filter(target.x, timestampSeconds);
        const float fy = yFilter_.Filter(target.y, timestampSeconds);
        const float fz = zFilter_.Filter(target.z, timestampSeconds);

        const Quaternion result = Quaternion{fw, fx, fy, fz}.Normalized();
        lastQ_ = result;
        initialized_ = true;
        return result;
    }

    void Reset() noexcept {
        wFilter_.Reset();
        xFilter_.Reset();
        yFilter_.Reset();
        zFilter_.Reset();
        initialized_ = false;
        lastQ_ = Quaternion::Identity();
    }

    [[nodiscard]] bool IsInitialized() const noexcept {
        return initialized_;
    }

    void SetParams(const OneEuroFilter1D::Params& params) noexcept {
        wFilter_.SetParams(params);
        xFilter_.SetParams(params);
        yFilter_.SetParams(params);
        zFilter_.SetParams(params);
    }

private:
    OneEuroFilter1D wFilter_;
    OneEuroFilter1D xFilter_;
    OneEuroFilter1D yFilter_;
    OneEuroFilter1D zFilter_;
    bool initialized_{false};
    Quaternion lastQ_{Quaternion::Identity()};
};

} // namespace phonecast::core::tracking
