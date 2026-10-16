#include "phonecast/core/protocol/StreamProtocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace phonecast::core::protocol {
namespace {
constexpr std::uint8_t kMagic[] = {'P', 'C', 'V', 'R'};

void Write16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}
void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int index = 3; index >= 0; --index) {
        bytes[offset + static_cast<std::size_t>(3 - index)] =
            static_cast<std::uint8_t>(value >> (index * 8));
    }
}
void Write64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (int index = 7; index >= 0; --index) {
        bytes[offset + static_cast<std::size_t>(7 - index)] =
            static_cast<std::uint8_t>(value >> (index * 8));
    }
}
std::uint16_t Read16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}
std::uint32_t Read32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value = (value << 8U) | data[index];
    return value;
}
std::uint64_t Read64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8U) | data[index];
    return value;
}
bool IsKnownType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(MessageType::Hello) &&
           type <= static_cast<std::uint8_t>(MessageType::PhoneImu);
}

void WriteFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value) {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value), "32-bit float required");
    std::memcpy(&bits, &value, sizeof(bits));
    Write32(bytes, offset, bits);
}

float ReadFloat(const std::uint8_t* data) {
    const std::uint32_t bits = Read32(data);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool IsKnownPointerType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(phonecast::core::PointerEvent::Type::Down) &&
           type <= static_cast<std::uint8_t>(phonecast::core::PointerEvent::Type::Back);
}

constexpr std::size_t kNotificationV1HeaderSize = 18;
constexpr std::size_t kNotificationV2HeaderSize = 26;
constexpr std::size_t kMaximumNotificationFieldSize = 1024;

bool NotificationFieldValid(const std::string& field) {
    return field.size() <= kMaximumNotificationFieldSize && field.size() <= 0xffffU;
}
}  // namespace

std::vector<std::uint8_t> Serialize(const Message& message) {
    if (message.payload.size() > kMaximumPayloadSize) return {};
    std::vector<std::uint8_t> bytes(kHeaderSize + message.payload.size());
    std::copy(std::begin(kMagic), std::end(kMagic), bytes.begin());
    bytes[4] = kProtocolVersion;
    bytes[5] = static_cast<std::uint8_t>(message.type);
    Write16(bytes, 6, message.flags);
    Write32(bytes, 8, static_cast<std::uint32_t>(message.payload.size()));
    Write64(bytes, 12, message.sequence);
    Write64(bytes, 20, message.timestampMicros);
    Write16(bytes, 28, message.width);
    Write16(bytes, 30, message.height);
    std::copy(message.payload.begin(), message.payload.end(), bytes.begin() + kHeaderSize);
    return bytes;
}

bool ParseHeader(const std::uint8_t* data, std::size_t size, Message& message,
                 std::uint32_t& payloadSize, std::string& error) {
    if (data == nullptr || size < kHeaderSize) {
        error = "Incomplete stream protocol header.";
        return false;
    }
    if (!std::equal(std::begin(kMagic), std::end(kMagic), data)) {
        error = "Invalid stream protocol magic.";
        return false;
    }
    if (data[4] != kProtocolVersion) {
        error = "Unsupported stream protocol version: " + std::to_string(data[4]) + '.';
        return false;
    }
    if (!IsKnownType(data[5])) {
        error = "Unknown stream message type.";
        return false;
    }
    payloadSize = Read32(data + 8);
    if (payloadSize > kMaximumPayloadSize) {
        error = "Stream payload exceeds the 4 MiB safety limit.";
        return false;
    }
    const auto type = static_cast<MessageType>(data[5]);
    if ((type == MessageType::AudioConfig && payloadSize != 16) ||
        (type == MessageType::AudioFrame && payloadSize != 1932) ||
        (type == MessageType::AudioStatus && payloadSize != 12) ||
        (type == MessageType::ScreenMetrics && payloadSize != 32) ||
        (type == MessageType::PhoneImu && payloadSize != 48)) {
        if (type == MessageType::ScreenMetrics) {
            error = "Invalid bounded screen metrics payload size.";
        } else if (type == MessageType::PhoneImu) {
            error = "Invalid bounded phone IMU payload size.";
        } else {
            error = "Invalid bounded audio payload size.";
        }
        return false;
    }
    message.type = type;
    message.flags = Read16(data + 6);
    message.sequence = Read64(data + 12);
    message.timestampMicros = Read64(data + 20);
    message.width = Read16(data + 28);
    message.height = Read16(data + 30);
    message.payload.clear();
    error.clear();
    return true;
}

bool IsValidPairCode(const std::string& code) noexcept {
    return code.size() == 6 &&
           std::all_of(code.begin(), code.end(), [](char value) { return value >= '0' && value <= '9'; });
}

std::vector<std::uint8_t> SerializePointerEvent(const phonecast::core::PointerEvent& event) {
    std::vector<std::uint8_t> bytes(16, 0);
    bytes[0] = static_cast<std::uint8_t>(event.type);
    WriteFloat(bytes, 4, event.normalizedX);
    WriteFloat(bytes, 8, event.normalizedY);
    WriteFloat(bytes, 12, event.scrollDelta);
    return bytes;
}

bool ParsePointerEvent(const std::uint8_t* data, std::size_t size,
                       phonecast::core::PointerEvent& event, std::string& error) {
    if (data == nullptr || size != 16) {
        error = "Remote-input payload must be exactly 16 bytes.";
        return false;
    }
    if (!IsKnownPointerType(data[0])) {
        error = "Unknown remote-input event type.";
        return false;
    }
    const float x = ReadFloat(data + 4);
    const float y = ReadFloat(data + 8);
    const float scroll = ReadFloat(data + 12);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scroll) ||
        x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F ||
        scroll < -1.0F || scroll > 1.0F) {
        error = "Remote-input coordinates are outside the normalized range.";
        return false;
    }
    event.type = static_cast<phonecast::core::PointerEvent::Type>(data[0]);
    event.normalizedX = x;
    event.normalizedY = y;
    event.scrollDelta = scroll;
    error.clear();
    return true;
}

std::vector<std::uint8_t> SerializeNotification(const NotificationEvent& notification) {
    if (!NotificationFieldValid(notification.applicationName) ||
        !NotificationFieldValid(notification.title) ||
        !NotificationFieldValid(notification.body) ||
        !NotificationFieldValid(notification.packageName)) return {};
    const std::size_t payloadSize = kNotificationV2HeaderSize +
        notification.applicationName.size() + notification.title.size() +
        notification.body.size() + notification.packageName.size();
    if (payloadSize > kMaximumPayloadSize) return {};
    std::vector<std::uint8_t> bytes(payloadSize, 0);
    bytes[0] = 2;
    bytes[1] = notification.contentRedacted ? 1U : 0U;
    Write16(bytes, 2, static_cast<std::uint16_t>(notification.applicationName.size()));
    Write16(bytes, 4, static_cast<std::uint16_t>(notification.title.size()));
    Write16(bytes, 6, static_cast<std::uint16_t>(notification.body.size()));
    Write16(bytes, 8, static_cast<std::uint16_t>(notification.packageName.size()));
    Write64(bytes, 10, notification.postedAtMillis);
    Write64(bytes, 18, notification.actionToken);
    auto output = bytes.begin() + static_cast<std::ptrdiff_t>(kNotificationV2HeaderSize);
    for (const auto* field : {&notification.applicationName, &notification.title,
                              &notification.body, &notification.packageName}) {
        output = std::copy(field->begin(), field->end(), output);
    }
    return bytes;
}

std::vector<std::uint8_t> SerializeRemoteControlStatus(const RemoteControlStatus& status) {
    return {1U, static_cast<std::uint8_t>((status.appEnabled ? 1U : 0U) |
                                         (status.accessibilityEnabled ? 2U : 0U))};
}

bool ParseRemoteControlStatus(const std::uint8_t* data, std::size_t size,
                              RemoteControlStatus& status, std::string& error) {
    if (data == nullptr || size != 2U) {
        error = "Remote-control status payload must be exactly 2 bytes.";
        return false;
    }
    if (data[0] != 1U || (data[1] & 0xfcU) != 0U) {
        error = "Unsupported remote-control status version or flags.";
        return false;
    }
    status.appEnabled = (data[1] & 1U) != 0U;
    status.accessibilityEnabled = (data[1] & 2U) != 0U;
    error.clear();
    return true;
}

bool ParseNotification(const std::uint8_t* data, std::size_t size,
                       NotificationEvent& notification, std::string& error) {
    if (data == nullptr || size < kNotificationV1HeaderSize) {
        error = "Notification payload is incomplete.";
        return false;
    }
    if ((data[0] != 1U && data[0] != 2U) || (data[1] & 0xfeU) != 0U) {
        error = "Unsupported notification payload version or flags.";
        return false;
    }
    const std::size_t headerSize = data[0] == 2U
        ? kNotificationV2HeaderSize : kNotificationV1HeaderSize;
    if (size < headerSize) {
        error = "Notification payload is incomplete.";
        return false;
    }
    const std::array<std::size_t, 4> lengths{
        Read16(data + 2), Read16(data + 4), Read16(data + 6), Read16(data + 8)};
    std::size_t expected = headerSize;
    for (const auto length : lengths) {
        if (length > kMaximumNotificationFieldSize || expected > size || length > size - expected) {
            error = "Notification field length is invalid.";
            return false;
        }
        expected += length;
    }
    if (expected != size) {
        error = "Notification payload has trailing or missing data.";
        return false;
    }
    std::size_t offset = headerSize;
    auto readString = [&](std::size_t length) {
        std::string value(reinterpret_cast<const char*>(data + offset), length);
        offset += length;
        return value;
    };
    notification.applicationName = readString(lengths[0]);
    notification.title = readString(lengths[1]);
    notification.body = readString(lengths[2]);
    notification.packageName = readString(lengths[3]);
    notification.postedAtMillis = Read64(data + 10);
    notification.actionToken = data[0] == 2U ? Read64(data + 18) : 0U;
    notification.contentRedacted = (data[1] & 1U) != 0U;
    if (notification.applicationName.empty() || notification.packageName.empty()) {
        error = "Notification source is missing.";
        return false;
    }
    error.clear();
    return true;
}

std::vector<std::uint8_t> TrackingCapability() {
    return {'P', 'C', 'T', 'R', 1, 1, 0, 0};
}

bool HasTrackingCapability(const std::vector<std::uint8_t>& payload) {
    const std::uint8_t token[8] = {'P', 'C', 'T', 'R', 1, 1, 0, 0};
    if (payload.size() < 8) return false;
    for (std::size_t i = 0; i + 8 <= payload.size(); i += 8) {
        if (std::equal(token, token + 8, payload.data() + i)) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> SerializeScreenMetrics(const StreamScreenMetrics& metrics) {
    if (metrics.version != 1 ||
        (metrics.flags & 0xfeU) != 0 ||
        metrics.pixelWidth == 0 || metrics.pixelWidth > 16384 ||
        metrics.pixelHeight == 0 || metrics.pixelHeight > 16384 ||
        !std::isfinite(metrics.physicalWidthMm) || metrics.physicalWidthMm < 10.0F || metrics.physicalWidthMm > 1000.0F ||
        !std::isfinite(metrics.physicalHeightMm) || metrics.physicalHeightMm < 10.0F || metrics.physicalHeightMm > 1000.0F ||
        !std::isfinite(metrics.xdpi) || metrics.xdpi < 10.0F || metrics.xdpi > 2000.0F ||
        !std::isfinite(metrics.ydpi) || metrics.ydpi < 10.0F || metrics.ydpi > 2000.0F ||
        metrics.densityDpi == 0 || metrics.densityDpi > 2000) {
        return {};
    }
    std::vector<std::uint8_t> bytes(32, 0);
    bytes[0] = metrics.version;
    bytes[1] = metrics.flags;
    Write16(bytes, 2, metrics.densityDpi);
    Write32(bytes, 4, metrics.pixelWidth);
    Write32(bytes, 8, metrics.pixelHeight);
    WriteFloat(bytes, 12, metrics.physicalWidthMm);
    WriteFloat(bytes, 16, metrics.physicalHeightMm);
    WriteFloat(bytes, 20, metrics.xdpi);
    WriteFloat(bytes, 24, metrics.ydpi);
    return bytes;
}

bool ParseScreenMetrics(const std::uint8_t* data, std::size_t size,
                        StreamScreenMetrics& metrics, std::string& error) {
    if (data == nullptr || size != 32) {
        error = "Screen metrics payload must be exactly 32 bytes.";
        return false;
    }
    if (data[0] != 1) {
        error = "Unsupported screen metrics payload version: " + std::to_string(data[0]) + '.';
        return false;
    }
    if ((data[1] & 0xfeU) != 0) {
        error = "Reserved screen metrics flags must be zero.";
        return false;
    }
    const auto densityDpi = Read16(data + 2);
    const auto pixelWidth = Read32(data + 4);
    const auto pixelHeight = Read32(data + 8);
    const auto physicalWidthMm = ReadFloat(data + 12);
    const auto physicalHeightMm = ReadFloat(data + 16);
    const auto xdpi = ReadFloat(data + 20);
    const auto ydpi = ReadFloat(data + 24);

    if (pixelWidth == 0 || pixelWidth > 16384 ||
        pixelHeight == 0 || pixelHeight > 16384) {
        error = "Screen metrics pixel dimensions out of range.";
        return false;
    }
    if (!std::isfinite(physicalWidthMm) || physicalWidthMm < 10.0F || physicalWidthMm > 1000.0F) {
        error = "Physical screen width mm out of range.";
        return false;
    }
    if (!std::isfinite(physicalHeightMm) || physicalHeightMm < 10.0F || physicalHeightMm > 1000.0F) {
        error = "Physical screen height mm out of range.";
        return false;
    }
    if (!std::isfinite(xdpi) || xdpi < 10.0F || xdpi > 2000.0F ||
        !std::isfinite(ydpi) || ydpi < 10.0F || ydpi > 2000.0F) {
        error = "Screen metrics DPI values out of range.";
        return false;
    }
    if (densityDpi == 0 || densityDpi > 2000) {
        error = "Screen metrics density DPI out of range.";
        return false;
    }
    if (data[28] != 0 || data[29] != 0 || data[30] != 0 || data[31] != 0) {
        error = "Reserved screen metrics bytes must be zero.";
        return false;
    }

    metrics.version = data[0];
    metrics.flags = data[1];
    metrics.densityDpi = densityDpi;
    metrics.pixelWidth = pixelWidth;
    metrics.pixelHeight = pixelHeight;
    metrics.physicalWidthMm = physicalWidthMm;
    metrics.physicalHeightMm = physicalHeightMm;
    metrics.xdpi = xdpi;
    metrics.ydpi = ydpi;
    error.clear();
    return true;
}

std::vector<std::uint8_t> SerializePhoneImu(const StreamPhoneImu& imu) {
    if (imu.version != 1) return {};
    if (imu.sensorType != 1 && imu.sensorType != 2) return {};
    if (imu.accuracy > 3) return {};
    if ((imu.flags & 0xfcU) != 0) return {};
    if (!std::isfinite(imu.qx) || !std::isfinite(imu.qy) ||
        !std::isfinite(imu.qz) || !std::isfinite(imu.qw)) return {};
    const float normSq = imu.qx * imu.qx + imu.qy * imu.qy + imu.qz * imu.qz + imu.qw * imu.qw;
    if (std::abs(normSq - 1.0F) > 0.05F) return {};
    if (!std::isfinite(imu.gyroX) || !std::isfinite(imu.gyroY) || !std::isfinite(imu.gyroZ) ||
        !std::isfinite(imu.accelX) || !std::isfinite(imu.accelY) || !std::isfinite(imu.accelZ)) return {};
    if (std::abs(imu.gyroX) > 50.0F || std::abs(imu.gyroY) > 50.0F || std::abs(imu.gyroZ) > 50.0F) return {};
    if (std::abs(imu.accelX) > 200.0F || std::abs(imu.accelY) > 200.0F || std::abs(imu.accelZ) > 200.0F) return {};

    std::vector<std::uint8_t> bytes(48, 0);
    bytes[0] = imu.version;
    bytes[1] = imu.sensorType;
    bytes[2] = imu.accuracy;
    bytes[3] = imu.flags;
    WriteFloat(bytes, 4, imu.qx);
    WriteFloat(bytes, 8, imu.qy);
    WriteFloat(bytes, 12, imu.qz);
    WriteFloat(bytes, 16, imu.qw);
    WriteFloat(bytes, 20, imu.gyroX);
    WriteFloat(bytes, 24, imu.gyroY);
    WriteFloat(bytes, 28, imu.gyroZ);
    WriteFloat(bytes, 32, imu.accelX);
    WriteFloat(bytes, 36, imu.accelY);
    WriteFloat(bytes, 40, imu.accelZ);
    return bytes;
}

bool ParsePhoneImu(const std::uint8_t* data, std::size_t size,
                   StreamPhoneImu& imu, std::string& error) {
    if (data == nullptr || size != 48) {
        error = "Phone IMU payload must be exactly 48 bytes.";
        return false;
    }
    if (data[0] != 1) {
        error = "Unsupported phone IMU payload version: " + std::to_string(data[0]) + '.';
        return false;
    }
    const auto sensorType = data[1];
    if (sensorType != 1 && sensorType != 2) {
        error = "Unsupported sensor type.";
        return false;
    }
    const auto accuracy = data[2];
    if (accuracy > 3) {
        error = "Invalid sensor accuracy level.";
        return false;
    }
    const auto flags = data[3];
    if ((flags & 0xfcU) != 0) {
        error = "Reserved phone IMU flags must be zero.";
        return false;
    }
    const auto qx = ReadFloat(data + 4);
    const auto qy = ReadFloat(data + 8);
    const auto qz = ReadFloat(data + 12);
    const auto qw = ReadFloat(data + 16);
    if (!std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw)) {
        error = "Phone IMU quaternion components must be finite.";
        return false;
    }
    const float normSq = qx * qx + qy * qy + qz * qz + qw * qw;
    if (std::abs(normSq - 1.0F) > 0.05F) {
        error = "Orientation quaternion is not normalized.";
        return false;
    }
    const auto gyroX = ReadFloat(data + 20);
    const auto gyroY = ReadFloat(data + 24);
    const auto gyroZ = ReadFloat(data + 28);
    const auto accelX = ReadFloat(data + 32);
    const auto accelY = ReadFloat(data + 36);
    const auto accelZ = ReadFloat(data + 40);
    if (!std::isfinite(gyroX) || !std::isfinite(gyroY) || !std::isfinite(gyroZ) ||
        !std::isfinite(accelX) || !std::isfinite(accelY) || !std::isfinite(accelZ)) {
        error = "Phone IMU sensor vectors must be finite.";
        return false;
    }
    if (std::abs(gyroX) > 50.0F || std::abs(gyroY) > 50.0F || std::abs(gyroZ) > 50.0F) {
        error = "Angular velocity exceeds physical limit.";
        return false;
    }
    if (std::abs(accelX) > 200.0F || std::abs(accelY) > 200.0F || std::abs(accelZ) > 200.0F) {
        error = "Acceleration exceeds physical limit.";
        return false;
    }
    if (data[44] != 0 || data[45] != 0 || data[46] != 0 || data[47] != 0) {
        error = "Reserved phone IMU bytes must be zero.";
        return false;
    }

    imu.version = data[0];
    imu.sensorType = sensorType;
    imu.accuracy = accuracy;
    imu.flags = flags;
    imu.qx = qx;
    imu.qy = qy;
    imu.qz = qz;
    imu.qw = qw;
    imu.gyroX = gyroX;
    imu.gyroY = gyroY;
    imu.gyroZ = gyroZ;
    imu.accelX = accelX;
    imu.accelY = accelY;
    imu.accelZ = accelZ;
    error.clear();
    return true;
}

}  // namespace phonecast::core::protocol
