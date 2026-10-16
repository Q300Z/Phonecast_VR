#pragma once

#include "phonecast/core/input/IInputProvider.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phonecast::core::protocol {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint32_t kMaximumPayloadSize = 4U * 1024U * 1024U;

enum class MessageType : std::uint8_t {
    Hello = 1,
    VideoConfig = 2,
    VideoFrame = 3,
    Ping = 4,
    Pong = 5,
    EndStream = 6,
    RequestKeyFrame = 7,
    RemoteInput = 8,
    Notification = 9,
    RemoteControlStatus = 10,
    NotificationOpen = 11,
    AudioConfig = 12,
    AudioFrame = 13,
    AudioStatus = 14,
    ScreenMetrics = 15,
    PhoneImu = 16,
};

enum MessageFlags : std::uint16_t {
    None = 0,
    KeyFrame = 1U << 0U,
};

struct Message {
    MessageType type{MessageType::Hello};
    std::uint16_t flags{};
    std::uint64_t sequence{};
    std::uint64_t timestampMicros{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::vector<std::uint8_t> payload;
};

struct NotificationEvent {
    std::string applicationName;
    std::string title;
    std::string body;
    std::string packageName;
    std::uint64_t postedAtMillis{};
    // Opaque, session-scoped Android token. Zero means the source notification
    // has no launch action.
    std::uint64_t actionToken{};
    bool contentRedacted{true};
};

struct RemoteControlStatus {
    bool appEnabled{false};
    bool accessibilityEnabled{false};

    [[nodiscard]] bool Ready() const noexcept {
        return appEnabled && accessibilityEnabled;
    }
};

std::vector<std::uint8_t> Serialize(const Message& message);
bool ParseHeader(const std::uint8_t* data, std::size_t size, Message& message,
                 std::uint32_t& payloadSize, std::string& error);
bool IsValidPairCode(const std::string& code) noexcept;
std::vector<std::uint8_t> SerializePointerEvent(const phonecast::core::PointerEvent& event);
bool ParsePointerEvent(const std::uint8_t* data, std::size_t size,
                       phonecast::core::PointerEvent& event, std::string& error);
std::vector<std::uint8_t> SerializeNotification(const NotificationEvent& notification);
bool ParseNotification(const std::uint8_t* data, std::size_t size,
                       NotificationEvent& notification, std::string& error);
std::vector<std::uint8_t> SerializeRemoteControlStatus(const RemoteControlStatus& status);
bool ParseRemoteControlStatus(const std::uint8_t* data, std::size_t size,
                              RemoteControlStatus& status, std::string& error);

struct StreamScreenMetrics {
    std::uint8_t version{1};
    std::uint8_t flags{0};
    std::uint16_t densityDpi{0};
    std::uint32_t pixelWidth{0};
    std::uint32_t pixelHeight{0};
    float physicalWidthMm{0.0f};
    float physicalHeightMm{0.0f};
    float xdpi{0.0f};
    float ydpi{0.0f};
};

struct StreamPhoneImu {
    std::uint8_t version{1};
    std::uint8_t sensorType{1};
    std::uint8_t accuracy{3};
    std::uint8_t flags{0};
    float qx{0.0f};
    float qy{0.0f};
    float qz{0.0f};
    float qw{1.0f};
    float gyroX{0.0f};
    float gyroY{0.0f};
    float gyroZ{0.0f};
    float accelX{0.0f};
    float accelY{0.0f};
    float accelZ{0.0f};
};

std::vector<std::uint8_t> TrackingCapability();
bool HasTrackingCapability(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> SerializeScreenMetrics(const StreamScreenMetrics& metrics);
bool ParseScreenMetrics(const std::uint8_t* data, std::size_t size,
                        StreamScreenMetrics& metrics, std::string& error);

std::vector<std::uint8_t> SerializePhoneImu(const StreamPhoneImu& imu);
bool ParsePhoneImu(const std::uint8_t* data, std::size_t size,
                   StreamPhoneImu& imu, std::string& error);

}  // namespace phonecast::core::protocol
