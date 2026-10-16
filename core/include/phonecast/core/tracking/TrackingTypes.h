#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace phonecast::core::tracking {

// ============================================================================
// 2D Vector
// ============================================================================
struct Vector2 {
    float x{0.0F};
    float y{0.0F};

    constexpr Vector2() noexcept = default;
    constexpr Vector2(float xVal, float yVal) noexcept : x(xVal), y(yVal) {}

    [[nodiscard]] constexpr Vector2 operator+(const Vector2& rhs) const noexcept {
        return {x + rhs.x, y + rhs.y};
    }
    [[nodiscard]] constexpr Vector2 operator-(const Vector2& rhs) const noexcept {
        return {x - rhs.x, y - rhs.y};
    }
    [[nodiscard]] constexpr Vector2 operator*(float scalar) const noexcept {
        return {x * scalar, y * scalar};
    }
    [[nodiscard]] constexpr Vector2 operator/(float scalar) const noexcept {
        return {x / scalar, y / scalar};
    }
    constexpr Vector2& operator+=(const Vector2& rhs) noexcept {
        x += rhs.x; y += rhs.y; return *this;
    }
    constexpr Vector2& operator-=(const Vector2& rhs) noexcept {
        x -= rhs.x; y -= rhs.y; return *this;
    }
    [[nodiscard]] constexpr bool operator==(const Vector2& rhs) const noexcept {
        return x == rhs.x && y == rhs.y;
    }
    [[nodiscard]] constexpr bool operator!=(const Vector2& rhs) const noexcept {
        return !(*this == rhs);
    }

    [[nodiscard]] constexpr float Dot(const Vector2& rhs) const noexcept {
        return x * rhs.x + y * rhs.y;
    }
    [[nodiscard]] constexpr float SquaredNorm() const noexcept {
        return x * x + y * y;
    }
    [[nodiscard]] float Norm() const noexcept {
        return std::sqrt(SquaredNorm());
    }
    [[nodiscard]] Vector2 Normalized() const noexcept {
        const float len = Norm();
        return len > 1e-7F ? (*this / len) : Vector2{0.0F, 0.0F};
    }
};

// ============================================================================
// 3D Vector
// ============================================================================
struct Vector3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};

    constexpr Vector3() noexcept = default;
    constexpr Vector3(float xVal, float yVal, float zVal) noexcept : x(xVal), y(yVal), z(zVal) {}

    [[nodiscard]] constexpr Vector3 operator+(const Vector3& rhs) const noexcept {
        return {x + rhs.x, y + rhs.y, z + rhs.z};
    }
    [[nodiscard]] constexpr Vector3 operator-(const Vector3& rhs) const noexcept {
        return {x - rhs.x, y - rhs.y, z - rhs.z};
    }
    [[nodiscard]] constexpr Vector3 operator*(float scalar) const noexcept {
        return {x * scalar, y * scalar, z * scalar};
    }
    [[nodiscard]] constexpr Vector3 operator/(float scalar) const noexcept {
        return {x / scalar, y / scalar, z / scalar};
    }
    constexpr Vector3& operator+=(const Vector3& rhs) noexcept {
        x += rhs.x; y += rhs.y; z += rhs.z; return *this;
    }
    constexpr Vector3& operator-=(const Vector3& rhs) noexcept {
        x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this;
    }
    [[nodiscard]] constexpr bool operator==(const Vector3& rhs) const noexcept {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }
    [[nodiscard]] constexpr bool operator!=(const Vector3& rhs) const noexcept {
        return !(*this == rhs);
    }

    [[nodiscard]] constexpr float Dot(const Vector3& rhs) const noexcept {
        return x * rhs.x + y * rhs.y + z * rhs.z;
    }
    [[nodiscard]] constexpr Vector3 Cross(const Vector3& rhs) const noexcept {
        return {
            y * rhs.z - z * rhs.y,
            z * rhs.x - x * rhs.z,
            x * rhs.y - y * rhs.x
        };
    }
    [[nodiscard]] constexpr float SquaredNorm() const noexcept {
        return x * x + y * y + z * z;
    }
    [[nodiscard]] float Norm() const noexcept {
        return std::sqrt(SquaredNorm());
    }
    [[nodiscard]] Vector3 Normalized() const noexcept {
        const float len = Norm();
        return len > 1e-7F ? (*this / len) : Vector3{0.0F, 0.0F, 0.0F};
    }
};

// ============================================================================
// Quaternion (Unit Hamiltonian Rotation)
// ============================================================================
struct Quaternion {
    float w{1.0F};
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};

    constexpr Quaternion() noexcept = default;
    constexpr Quaternion(float wVal, float xVal, float yVal, float zVal) noexcept
        : w(wVal), x(xVal), y(yVal), z(zVal) {}

    [[nodiscard]] static constexpr Quaternion Identity() noexcept {
        return {1.0F, 0.0F, 0.0F, 0.0F};
    }

    [[nodiscard]] constexpr Quaternion Conjugate() const noexcept {
        return {w, -x, -y, -z};
    }

    [[nodiscard]] constexpr float SquaredNorm() const noexcept {
        return w * w + x * x + y * y + z * z;
    }

    [[nodiscard]] float Norm() const noexcept {
        return std::sqrt(SquaredNorm());
    }

    [[nodiscard]] Quaternion Normalized() const noexcept {
        const float len = Norm();
        if (len < 1e-7F) {
            return Identity();
        }
        const float inv = 1.0F / len;
        return {w * inv, x * inv, y * inv, z * inv};
    }

    // Hamilton product: q_composite = q_lhs * q_rhs (applies rhs then lhs)
    [[nodiscard]] constexpr Quaternion operator*(const Quaternion& rhs) const noexcept {
        return {
            w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z,
            w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
            w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
            w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w
        };
    }

    // Rotates a 3D vector by this quaternion: v' = q * v * q^-1
    [[nodiscard]] Vector3 Rotate(const Vector3& v) const noexcept {
        const Vector3 qv{x, y, z};
        const Vector3 t = qv.Cross(v) * 2.0F;
        return v + (t * w) + qv.Cross(t);
    }
};

// ============================================================================
// Tracking State Enumeration
// ============================================================================
enum class TrackingState : uint8_t {
    Lost = 0,
    Tracking = 1,
    Occluded = 2
};

inline const char* TrackingStateToString(TrackingState state) noexcept {
    switch (state) {
        case TrackingState::Lost: return "Lost";
        case TrackingState::Tracking: return "Tracking";
        case TrackingState::Occluded: return "Occluded";
        default: return "Unknown";
    }
}

// ============================================================================
// 6-DoF Rigid Pose
// ============================================================================
struct Pose6D {
    Vector3 position{};       // Position in meters
    Quaternion orientation{};  // Rotation quaternion
    bool valid{false};
    float confidence{0.0F};   // Normalized tracking quality [0.0, 1.0]
    uint64_t timestampMicros{0};
    TrackingState state{TrackingState::Lost};

    [[nodiscard]] static Pose6D Invalid() noexcept {
        return {{0.0F, 0.0F, 0.0F}, Quaternion::Identity(), false, 0.0F, 0, TrackingState::Lost};
    }

    [[nodiscard]] Pose6D Inverse() const noexcept {
        const Quaternion invRot = orientation.Conjugate();
        const Vector3 invPos = invRot.Rotate(position * -1.0F);
        return {invPos, invRot, valid, confidence, timestampMicros, state};
    }

    [[nodiscard]] Vector3 TransformPoint(const Vector3& p) const noexcept {
        return orientation.Rotate(p) + position;
    }

    // Pose compounding: T_ac = T_ab * T_bc
    [[nodiscard]] Pose6D operator*(const Pose6D& rhs) const noexcept {
        return {
            position + orientation.Rotate(rhs.position),
            (orientation * rhs.orientation).Normalized(),
            valid && rhs.valid,
            std::min(confidence, rhs.confidence),
            std::max(timestampMicros, rhs.timestampMicros),
            (valid && rhs.valid) ? state : TrackingState::Lost
        };
    }
};

// ============================================================================
// Physical Display Metrics
// ============================================================================
struct ScreenMetrics {
    float widthMillimeters{0.0F};
    float heightMillimeters{0.0F};
    uint32_t pixelWidth{0};
    uint32_t pixelHeight{0};
    float cornerRadiusMillimeters{0.0F};

    [[nodiscard]] bool IsValid() const noexcept {
        return widthMillimeters >= 20.0F && widthMillimeters <= 400.0F &&
               heightMillimeters >= 20.0F && heightMillimeters <= 400.0F &&
               pixelWidth > 0 && pixelHeight > 0;
    }

    [[nodiscard]] constexpr float WidthMeters() const noexcept {
        return widthMillimeters * 0.001F;
    }

    [[nodiscard]] constexpr float HeightMeters() const noexcept {
        return heightMillimeters * 0.001F;
    }

    [[nodiscard]] constexpr float AspectRatio() const noexcept {
        return heightMillimeters > 0.0F ? (widthMillimeters / heightMillimeters) : 0.0F;
    }
};

// ============================================================================
// Phone IMU Packet (Cadenced from NetworkStreamer)
// ============================================================================
struct PhoneImuPacket {
    uint64_t sequence{0};
    uint64_t timestampMicros{0};
    Quaternion orientation{};       // Sensor.TYPE_GAME_ROTATION_VECTOR (gravity-aligned)
    Vector3 angularVelocity{};      // rad/s
    Vector3 linearAcceleration{};   // m/s^2
};

// ============================================================================
// Pinhole Camera Intrinsics
// ============================================================================
struct CameraIntrinsics {
    float fx{0.0F};
    float fy{0.0F};
    float cx{0.0F};
    float cy{0.0F};
    uint32_t imageWidth{0};
    uint32_t imageHeight{0};

    [[nodiscard]] bool IsValid() const noexcept {
        return fx > 1.0F && fy > 1.0F && cx > 0.0F && cy > 0.0F &&
               imageWidth > 0 && imageHeight > 0;
    }

    // Projects 3D camera-space point (meters) to 2D pixel coordinates
    [[nodiscard]] Vector2 Project(const Vector3& p) const noexcept {
        if (p.z <= 1e-4F) {
            return {0.0F, 0.0F};
        }
        const float invZ = 1.0F / p.z;
        return {fx * (p.x * invZ) + cx, fy * (p.y * invZ) + cy};
    }

    // Normalizes pixel coordinate to camera ray: (u - cx)/fx, (v - cy)/fy
    [[nodiscard]] Vector2 NormalizePixel(const Vector2& pixel) const noexcept {
        return {(pixel.x - cx) / fx, (pixel.y - cy) / fy};
    }
};

// ============================================================================
// 2D Normalized Line (a*x + b*y + c = 0, a^2 + b^2 = 1)
// ============================================================================
struct Line2D {
    float a{0.0F};
    float b{0.0F};
    float c{0.0F};
    bool valid{false};

    [[nodiscard]] constexpr float SignedDistance(const Vector2& p) const noexcept {
        return a * p.x + b * p.y + c;
    }

    [[nodiscard]] constexpr float Distance(const Vector2& p) const noexcept {
        const float d = SignedDistance(p);
        return d >= 0.0F ? d : -d;
    }

    [[nodiscard]] constexpr Vector2 Normal() const noexcept {
        return {a, b};
    }

    [[nodiscard]] constexpr Vector2 Direction() const noexcept {
        return {-b, a};
    }
};

// ============================================================================
// Screen Borders (4 Fitted Lines)
// ============================================================================
struct ScreenBorders {
    Line2D top{};
    Line2D right{};
    Line2D bottom{};
    Line2D left{};
    bool allValid{false};

    [[nodiscard]] bool IsValid() const noexcept {
        return allValid && top.valid && right.valid && bottom.valid && left.valid;
    }
};

// ============================================================================
// Virtual 2D Corners (Intersections of Fitted Lines)
// ============================================================================
struct ScreenCorners2D {
    // Clockwise order:
    // [0] = Top-Left, [1] = Top-Right, [2] = Bottom-Right, [3] = Bottom-Left
    std::array<Vector2, 4> corners{};
    bool valid{false};

    [[nodiscard]] float Area() const noexcept {
        if (!valid) {
            return 0.0F;
        }
        // Shoelace formula
        float sum = 0.0F;
        for (size_t i = 0; i < 4; ++i) {
            const size_t next = (i + 1) % 4;
            sum += corners[i].x * corners[next].y - corners[next].x * corners[i].y;
        }
        return 0.5F * std::abs(sum);
    }
};

// ============================================================================
// Lightweight Non-Owning View of Camera Frame
// ============================================================================
struct CameraFrameView {
    const uint8_t* data{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t strideBytes{0};
    uint32_t bytesPerPixel{4}; // 4 = RGBA (OpenVR undistorted), 1 = grayscale
    uint64_t timestampMicros{0};
    uint32_t sequence{0};

    [[nodiscard]] uint8_t GetLuma(uint32_t x, uint32_t y) const noexcept {
        if (data == nullptr || x >= width || y >= height) {
            return 0;
        }
        const uint8_t* row = data + (y * strideBytes);
        if (bytesPerPixel == 1) {
            return row[x];
        }
        const uint8_t* pixel = row + (x * bytesPerPixel);
        // Fast integer luma approximation: (77*R + 150*G + 29*B) >> 8
        return static_cast<uint8_t>((77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8U);
    }
};

} // namespace phonecast::core::tracking
