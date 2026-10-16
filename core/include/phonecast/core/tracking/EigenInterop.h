#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"
#include <Eigen/Dense>
#include <Eigen/Geometry>

namespace phonecast::core::tracking::interop {

inline Eigen::Vector2f ToEigen(const Vector2& v) noexcept {
    return {v.x, v.y};
}

inline Vector2 FromEigen(const Eigen::Vector2f& v) noexcept {
    return {v.x(), v.y()};
}

inline Eigen::Vector3f ToEigen(const Vector3& v) noexcept {
    return {v.x, v.y, v.z};
}

inline Vector3 FromEigen(const Eigen::Vector3f& v) noexcept {
    return {v.x(), v.y(), v.z()};
}

inline Eigen::Quaternionf ToEigen(const Quaternion& q) noexcept {
    return {q.w, q.x, q.y, q.z};
}

inline Quaternion FromEigen(const Eigen::Quaternionf& q) noexcept {
    return {q.w(), q.x(), q.y(), q.z()};
}

inline Eigen::Vector2d ToEigenDouble(const Vector2& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y)};
}

inline Vector2 FromEigenDouble(const Eigen::Vector2d& v) noexcept {
    return {static_cast<float>(v.x()), static_cast<float>(v.y())};
}

inline Eigen::Vector3d ToEigenDouble(const Vector3& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

inline Vector3 FromEigenDouble(const Eigen::Vector3d& v) noexcept {
    return {static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z())};
}

inline Eigen::Quaterniond ToEigenDouble(const Quaternion& q) noexcept {
    return {static_cast<double>(q.w), static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z)};
}

inline Quaternion FromEigenDouble(const Eigen::Quaterniond& q) noexcept {
    return {static_cast<float>(q.w()), static_cast<float>(q.x()), static_cast<float>(q.y()), static_cast<float>(q.z())};
}

} // namespace phonecast::core::tracking::interop
