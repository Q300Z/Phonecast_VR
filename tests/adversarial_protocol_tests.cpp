#include "phonecast/core/protocol/StreamProtocol.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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

// ---------------------------------------------------------------------------
// Suite 1: ParseHeader Adversarial Stress
// ---------------------------------------------------------------------------
void TestParseHeaderAdversarial() {
    using namespace phonecast::core::protocol;
    std::cout << "[Suite 1] Running ParseHeader Adversarial Tests...\n";

    Message msg;
    std::uint32_t payloadSize = 0;
    std::string error;

    // 1.1 Nullptr and small sizes
    AssertFalse(ParseHeader(nullptr, 0, msg, payloadSize, error), "Nullptr with size 0 rejected");
    AssertTrue(!error.empty(), "Error message set on nullptr");
    AssertFalse(ParseHeader(nullptr, kHeaderSize, msg, payloadSize, error), "Nullptr with 32 bytes rejected");
    AssertFalse(ParseHeader(nullptr, 1024, msg, payloadSize, error), "Nullptr with 1024 bytes rejected");

    std::vector<std::uint8_t> validHeader(kHeaderSize, 0);
    validHeader[0] = 'P'; validHeader[1] = 'C'; validHeader[2] = 'V'; validHeader[3] = 'R';
    validHeader[4] = 1; // version
    validHeader[5] = static_cast<std::uint8_t>(MessageType::Ping);
    // payload size = 0
    validHeader[8] = 0; validHeader[9] = 0; validHeader[10] = 0; validHeader[11] = 0;

    for (std::size_t sz = 0; sz < kHeaderSize; ++sz) {
        AssertFalse(ParseHeader(validHeader.data(), sz, msg, payloadSize, error),
                    "Truncated header size " + std::to_string(sz) + " rejected");
    }

    // 1.2 Magic byte corruption
    for (int i = 0; i < 4; ++i) {
        auto corrupt = validHeader;
        corrupt[i] = 'X';
        AssertFalse(ParseHeader(corrupt.data(), corrupt.size(), msg, payloadSize, error),
                    "Magic corrupted at byte " + std::to_string(i) + " rejected");
    }

    // 1.3 Unsupported version
    {
        auto v0 = validHeader; v0[4] = 0;
        AssertFalse(ParseHeader(v0.data(), v0.size(), msg, payloadSize, error), "Protocol version 0 rejected");
        auto v2 = validHeader; v2[4] = 2;
        AssertFalse(ParseHeader(v2.data(), v2.size(), msg, payloadSize, error), "Protocol version 2 rejected");
        auto v255 = validHeader; v255[4] = 255;
        AssertFalse(ParseHeader(v255.data(), v255.size(), msg, payloadSize, error), "Protocol version 255 rejected");
    }

    // 1.4 Unknown message types
    {
        auto t0 = validHeader; t0[5] = 0;
        AssertFalse(ParseHeader(t0.data(), t0.size(), msg, payloadSize, error), "Message type 0 rejected");
        auto t17 = validHeader; t17[5] = 17;
        AssertFalse(ParseHeader(t17.data(), t17.size(), msg, payloadSize, error), "Message type 17 rejected");
        auto t255 = validHeader; t255[5] = 255;
        AssertFalse(ParseHeader(t255.data(), t255.size(), msg, payloadSize, error), "Message type 255 rejected");
    }

    // 1.5 Payload size safety limit (> 4 MiB)
    {
        auto oversized = validHeader;
        // 4 MiB + 1 = 4194305 = 0x00400001
        oversized[8] = 0x00; oversized[9] = 0x40; oversized[10] = 0x00; oversized[11] = 0x01;
        AssertFalse(ParseHeader(oversized.data(), oversized.size(), msg, payloadSize, error),
                    "Payload 4MiB + 1 rejected");
        // 0xFFFFFFFF
        oversized[8] = 0xFF; oversized[9] = 0xFF; oversized[10] = 0xFF; oversized[11] = 0xFF;
        AssertFalse(ParseHeader(oversized.data(), oversized.size(), msg, payloadSize, error),
                    "Payload 4GiB rejected");
    }

    // 1.6 Bounded payload size enforcement for ScreenMetrics (exact 32 bytes)
    {
        auto metricsHdr = validHeader;
        metricsHdr[5] = static_cast<std::uint8_t>(MessageType::ScreenMetrics);

        const std::uint32_t testSizes[] = {0, 1, 16, 31, 33, 48, 64, 1024, 4000000};
        for (auto sz : testSizes) {
            metricsHdr[8] = static_cast<std::uint8_t>(sz >> 24);
            metricsHdr[9] = static_cast<std::uint8_t>(sz >> 16);
            metricsHdr[10] = static_cast<std::uint8_t>(sz >> 8);
            metricsHdr[11] = static_cast<std::uint8_t>(sz);
            AssertFalse(ParseHeader(metricsHdr.data(), metricsHdr.size(), msg, payloadSize, error),
                        "ScreenMetrics header with invalid payloadSize=" + std::to_string(sz) + " rejected");
            AssertTrue(error.find("screen metrics") != std::string::npos, "Descriptive error for ScreenMetrics");
        }

        // Exactly 32 bytes
        metricsHdr[8] = 0; metricsHdr[9] = 0; metricsHdr[10] = 0; metricsHdr[11] = 32;
        AssertTrue(ParseHeader(metricsHdr.data(), metricsHdr.size(), msg, payloadSize, error),
                   "ScreenMetrics header with exact 32 bytes accepted");
        AssertTrue(payloadSize == 32, "payloadSize matches 32");
        AssertTrue(msg.type == MessageType::ScreenMetrics, "Message type matches ScreenMetrics");
    }

    // 1.7 Bounded payload size enforcement for PhoneImu (exact 48 bytes)
    {
        auto imuHdr = validHeader;
        imuHdr[5] = static_cast<std::uint8_t>(MessageType::PhoneImu);

        const std::uint32_t testSizes[] = {0, 1, 16, 32, 47, 49, 64, 1024, 4000000};
        for (auto sz : testSizes) {
            imuHdr[8] = static_cast<std::uint8_t>(sz >> 24);
            imuHdr[9] = static_cast<std::uint8_t>(sz >> 16);
            imuHdr[10] = static_cast<std::uint8_t>(sz >> 8);
            imuHdr[11] = static_cast<std::uint8_t>(sz);
            AssertFalse(ParseHeader(imuHdr.data(), imuHdr.size(), msg, payloadSize, error),
                        "PhoneImu header with invalid payloadSize=" + std::to_string(sz) + " rejected");
            AssertTrue(error.find("phone IMU") != std::string::npos, "Descriptive error for PhoneImu");
        }

        // Exactly 48 bytes
        imuHdr[8] = 0; imuHdr[9] = 0; imuHdr[10] = 0; imuHdr[11] = 48;
        AssertTrue(ParseHeader(imuHdr.data(), imuHdr.size(), msg, payloadSize, error),
                   "PhoneImu header with exact 48 bytes accepted");
        AssertTrue(payloadSize == 48, "payloadSize matches 48");
        AssertTrue(msg.type == MessageType::PhoneImu, "Message type matches PhoneImu");
    }

    // 1.8 Audio bounded sizes check
    {
        auto audioHdr = validHeader;
        audioHdr[5] = static_cast<std::uint8_t>(MessageType::AudioConfig);
        audioHdr[11] = 15; // not 16
        AssertFalse(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                    "AudioConfig with payload 15 rejected");
        audioHdr[11] = 16;
        AssertTrue(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                   "AudioConfig with payload 16 accepted");

        audioHdr[5] = static_cast<std::uint8_t>(MessageType::AudioFrame);
        audioHdr[8] = 0; audioHdr[9] = 0; audioHdr[10] = 0; audioHdr[11] = 0;
        AssertFalse(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                    "AudioFrame with payload 0 rejected");
        // 1932 bytes = 0x078C
        audioHdr[10] = 0x07; audioHdr[11] = 0x8C;
        AssertTrue(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                   "AudioFrame with payload 1932 accepted");

        audioHdr[5] = static_cast<std::uint8_t>(MessageType::AudioStatus);
        audioHdr[10] = 0; audioHdr[11] = 11;
        AssertFalse(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                    "AudioStatus with payload 11 rejected");
        audioHdr[11] = 12;
        AssertTrue(ParseHeader(audioHdr.data(), audioHdr.size(), msg, payloadSize, error),
                   "AudioStatus with payload 12 accepted");
    }
}

// ---------------------------------------------------------------------------
// Suite 2: ScreenMetrics Adversarial Stress
// ---------------------------------------------------------------------------
void TestScreenMetricsAdversarial() {
    using namespace phonecast::core::protocol;
    std::cout << "[Suite 2] Running ScreenMetrics Adversarial Tests...\n";

    StreamScreenMetrics valid;
    valid.version = 1;
    valid.flags = 0;
    valid.densityDpi = 440;
    valid.pixelWidth = 1080;
    valid.pixelHeight = 2400;
    valid.physicalWidthMm = 70.0F;
    valid.physicalHeightMm = 155.0F;
    valid.xdpi = 401.0F;
    valid.ydpi = 402.0F;

    const auto validBytes = SerializeScreenMetrics(valid);
    AssertTrue(validBytes.size() == 32, "Valid ScreenMetrics serializes to 32 bytes");

    StreamScreenMetrics parsed;
    std::string error;

    // 2.1 Null and length bounds
    AssertFalse(ParseScreenMetrics(nullptr, 0, parsed, error), "ScreenMetrics null rejected");
    AssertFalse(ParseScreenMetrics(nullptr, 32, parsed, error), "ScreenMetrics null 32 rejected");
    for (std::size_t sz = 0; sz < 32; ++sz) {
        AssertFalse(ParseScreenMetrics(validBytes.data(), sz, parsed, error),
                    "ScreenMetrics size " + std::to_string(sz) + " rejected");
    }
    AssertFalse(ParseScreenMetrics(validBytes.data(), 33, parsed, error), "ScreenMetrics size 33 rejected");
    AssertFalse(ParseScreenMetrics(validBytes.data(), 1024, parsed, error), "ScreenMetrics size 1024 rejected");

    // 2.2 Version enforcement
    {
        auto b = validBytes;
        b[0] = 0; AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Version 0 rejected");
        b[0] = 2; AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Version 2 rejected");
        b[0] = 255; AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Version 255 rejected");
    }

    // 2.3 Reserved flags
    {
        for (std::uint8_t flag = 2; flag != 0; flag = static_cast<std::uint8_t>(flag << 1)) {
            auto b = validBytes;
            b[1] = flag;
            AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error),
                        "Reserved flag 0x" + std::to_string(flag) + " rejected");
        }
        auto b1 = validBytes; b1[1] = 1; // measuredDpi bit
        AssertTrue(ParseScreenMetrics(b1.data(), b1.size(), parsed, error), "Flag 1 accepted");
    }

    // 2.4 Pixel dimensions (min 1, max 16384)
    {
        // pixelWidth = 0
        StreamScreenMetrics m = valid; m.pixelWidth = 0;
        AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects pixelWidth=0");
        auto b = validBytes;
        b[4] = 0; b[5] = 0; b[6] = 0; b[7] = 0;
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects pixelWidth=0");

        // pixelWidth = 16384 (valid)
        m.pixelWidth = 16384;
        AssertFalse(SerializeScreenMetrics(m).empty(), "Serialize accepts pixelWidth=16384");
        // pixelWidth = 16385 (invalid)
        m.pixelWidth = 16385;
        AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects pixelWidth=16385");
        b[4] = 0; b[5] = 0; b[6] = 0x40; b[7] = 0x01; // 16385
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects pixelWidth=16385");

        // pixelHeight = 0
        m = valid; m.pixelHeight = 0;
        AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects pixelHeight=0");
        b = validBytes;
        b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 0;
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects pixelHeight=0");

        // pixelHeight = 16384 (valid)
        m.pixelHeight = 16384;
        AssertFalse(SerializeScreenMetrics(m).empty(), "Serialize accepts pixelHeight=16384");
        // pixelHeight = 16385 (invalid)
        m.pixelHeight = 16385;
        AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects pixelHeight=16385");
        b[8] = 0; b[9] = 0; b[10] = 0x40; b[11] = 0x01;
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects pixelHeight=16385");
    }

    // 2.5 Physical dimensions (10.0 to 1000.0 mm)
    {
        const float extremeFloats[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::signaling_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            -10.0F, -0.001F, 0.0F, 9.999F, 1000.001F, 10000.0F
        };

        for (float val : extremeFloats) {
            StreamScreenMetrics m = valid; m.physicalWidthMm = val;
            AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects physicalWidthMm=" + std::to_string(val));
            m = valid; m.physicalHeightMm = val;
            AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects physicalHeightMm=" + std::to_string(val));
        }

        // Boundary tests
        StreamScreenMetrics m = valid;
        m.physicalWidthMm = 10.0F; m.physicalHeightMm = 1000.0F;
        auto bytes = SerializeScreenMetrics(m);
        AssertFalse(bytes.empty(), "Serialize accepts boundary physical dimensions (10.0 and 1000.0 mm)");
        AssertTrue(ParseScreenMetrics(bytes.data(), bytes.size(), parsed, error), "Parse accepts boundary physical mm");
        AssertTrue(std::abs(parsed.physicalWidthMm - 10.0F) < 0.001F, "Width parsed correctly");
        AssertTrue(std::abs(parsed.physicalHeightMm - 1000.0F) < 0.001F, "Height parsed correctly");
    }

    // 2.6 DPI values (10.0 to 2000.0)
    {
        const float badDpis[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -1.0F, 0.0F, 9.999F, 2000.001F, 5000.0F
        };
        for (float dpi : badDpis) {
            StreamScreenMetrics m = valid; m.xdpi = dpi;
            AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects xdpi=" + std::to_string(dpi));
            m = valid; m.ydpi = dpi;
            AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects ydpi=" + std::to_string(dpi));
        }

        StreamScreenMetrics m = valid;
        m.xdpi = 10.0F; m.ydpi = 2000.0F;
        auto bytes = SerializeScreenMetrics(m);
        AssertFalse(bytes.empty(), "Serialize accepts boundary DPIs (10.0 and 2000.0)");
        AssertTrue(ParseScreenMetrics(bytes.data(), bytes.size(), parsed, error), "Parse accepts boundary DPIs");
    }

    // 2.7 Density DPI (1 to 2000)
    {
        StreamScreenMetrics m = valid;
        m.densityDpi = 0; AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects densityDpi=0");
        m.densityDpi = 2001; AssertTrue(SerializeScreenMetrics(m).empty(), "Serialize rejects densityDpi=2001");
        m.densityDpi = 1; AssertFalse(SerializeScreenMetrics(m).empty(), "Serialize accepts densityDpi=1");
        m.densityDpi = 2000; AssertFalse(SerializeScreenMetrics(m).empty(), "Serialize accepts densityDpi=2000");

        auto b = validBytes;
        b[2] = 0; b[3] = 0;
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects densityDpi=0");
        b[2] = static_cast<std::uint8_t>(2001 >> 8);
        b[3] = static_cast<std::uint8_t>(2001 & 0xFF);
        AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error), "Parse rejects densityDpi=2001");
    }

    // 2.8 Reserved bytes must be zero
    {
        for (std::size_t offset = 28; offset < 32; ++offset) {
            auto b = validBytes;
            b[offset] = 1;
            AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error),
                        "Non-zero reserved byte at offset " + std::to_string(offset) + " rejected");
            b[offset] = 0xFF;
            AssertFalse(ParseScreenMetrics(b.data(), b.size(), parsed, error),
                        "0xFF reserved byte at offset " + std::to_string(offset) + " rejected");
        }
    }
}

// ---------------------------------------------------------------------------
// Suite 3: PhoneImu Adversarial Stress
// ---------------------------------------------------------------------------
void TestPhoneImuAdversarial() {
    using namespace phonecast::core::protocol;
    std::cout << "[Suite 3] Running PhoneImu Adversarial Tests...\n";

    StreamPhoneImu valid;
    valid.version = 1;
    valid.sensorType = 1;
    valid.accuracy = 3;
    valid.flags = 0;
    valid.qx = 0.0F;
    valid.qy = 0.0F;
    valid.qz = 0.0F;
    valid.qw = 1.0F;
    valid.gyroX = 0.0F;
    valid.gyroY = 0.0F;
    valid.gyroZ = 0.0F;
    valid.accelX = 0.0F;
    valid.accelY = 9.81F;
    valid.accelZ = 0.0F;

    const auto validBytes = SerializePhoneImu(valid);
    AssertTrue(validBytes.size() == 48, "Valid PhoneImu serializes to 48 bytes");

    StreamPhoneImu parsed;
    std::string error;

    // 3.1 Null and length bounds
    AssertFalse(ParsePhoneImu(nullptr, 0, parsed, error), "PhoneImu null 0 rejected");
    AssertFalse(ParsePhoneImu(nullptr, 48, parsed, error), "PhoneImu null 48 rejected");
    for (std::size_t sz = 0; sz < 48; ++sz) {
        AssertFalse(ParsePhoneImu(validBytes.data(), sz, parsed, error),
                    "PhoneImu size " + std::to_string(sz) + " rejected");
    }
    AssertFalse(ParsePhoneImu(validBytes.data(), 49, parsed, error), "PhoneImu size 49 rejected");
    AssertFalse(ParsePhoneImu(validBytes.data(), 1024, parsed, error), "PhoneImu size 1024 rejected");

    // 3.2 Version enforcement
    {
        auto b = validBytes;
        b[0] = 0; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Version 0 rejected");
        b[0] = 2; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Version 2 rejected");
        b[0] = 255; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Version 255 rejected");
    }

    // 3.3 Sensor type (1 or 2)
    {
        auto b = validBytes;
        b[1] = 0; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Sensor type 0 rejected");
        b[1] = 3; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Sensor type 3 rejected");
        b[1] = 255; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Sensor type 255 rejected");
        b[1] = 1; AssertTrue(ParsePhoneImu(b.data(), b.size(), parsed, error), "Sensor type 1 accepted");
        b[1] = 2; AssertTrue(ParsePhoneImu(b.data(), b.size(), parsed, error), "Sensor type 2 accepted");
    }

    // 3.4 Accuracy level (0 to 3)
    {
        auto b = validBytes;
        for (std::uint8_t acc = 0; acc <= 3; ++acc) {
            b[2] = acc;
            AssertTrue(ParsePhoneImu(b.data(), b.size(), parsed, error), "Accuracy " + std::to_string(acc) + " accepted");
        }
        b[2] = 4; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Accuracy 4 rejected");
        b[2] = 255; AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Accuracy 255 rejected");
    }

    // 3.5 Flags (reserved bits must be zero)
    {
        auto b = validBytes;
        for (std::uint8_t fl = 0; fl <= 3; ++fl) {
            b[3] = fl;
            AssertTrue(ParsePhoneImu(b.data(), b.size(), parsed, error), "Flags " + std::to_string(fl) + " accepted");
        }
        for (std::uint8_t fl = 4; fl != 0; fl = static_cast<std::uint8_t>(fl << 1)) {
            b[3] = fl;
            AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error), "Reserved flag 0x" + std::to_string(fl) + " rejected");
        }
    }

    // 3.6 Degenerate Quaternions & Non-Finite components
    {
        // All zeros
        StreamPhoneImu imu = valid;
        imu.qx = 0; imu.qy = 0; imu.qz = 0; imu.qw = 0;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects all-zero quaternion");

        const float nonFinites[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::signaling_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()
        };

        for (float val : nonFinites) {
            imu = valid; imu.qx = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite qx");
            imu = valid; imu.qy = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite qy");
            imu = valid; imu.qz = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite qz");
            imu = valid; imu.qw = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite qw");
        }

        // Quaternion normalization tolerance: |normSq - 1.0| <= 0.05
        // normSq = 1.050 -> valid, normSq = 1.051 -> rejected
        imu = valid; imu.qw = 1.024695F; // 1.024695^2 = 1.0499998 -> diff = 0.0499998 <= 0.05
        AssertFalse(SerializePhoneImu(imu).empty(), "Serialize accepts normSq within upper tolerance");
        imu.qw = std::sqrt(1.051F); // > 1.05
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects normSq=1.051");

        // normSq = 0.950 boundary -> qw = 0.97467947F has q*q = 0.95000005 -> diff = 0.04999995 <= 0.05
        imu.qw = 0.97467947F;
        AssertFalse(SerializePhoneImu(imu).empty(), "Serialize accepts normSq within lower tolerance (0.95)");
        imu.qw = 0.97467F; // 0.97467^2 = 0.9499816 -> diff = 0.050018 > 0.05
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects normSq below lower tolerance (< 0.95)");

        // Huge components that could cause float overflow during normSq calculation
        imu = valid; imu.qx = 1e20F; imu.qw = 1e20F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects overflow-inducing quaternion");
    }

    // 3.7 Gyroscope physical limits (|w| <= 50.0 rad/s)
    {
        const float nonFinites[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()
        };
        for (float val : nonFinites) {
            StreamPhoneImu imu = valid; imu.gyroX = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite gyroX");
            imu = valid; imu.gyroY = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite gyroY");
            imu = valid; imu.gyroZ = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite gyroZ");
        }

        StreamPhoneImu imu = valid;
        imu.gyroX = 50.0F; imu.gyroY = -50.0F; imu.gyroZ = 0.0F;
        AssertFalse(SerializePhoneImu(imu).empty(), "Serialize accepts boundary gyro (+-50.0 rad/s)");

        imu = valid; imu.gyroX = 50.001F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects gyroX=50.001");
        imu = valid; imu.gyroX = -50.001F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects gyroX=-50.001");
        imu = valid; imu.gyroY = 100.0F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects gyroY=100.0");
        imu = valid; imu.gyroZ = -100.0F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects gyroZ=-100.0");
    }

    // 3.8 Acceleration physical limits (|a| <= 200.0 m/s^2)
    {
        const float nonFinites[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()
        };
        for (float val : nonFinites) {
            StreamPhoneImu imu = valid; imu.accelX = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite accelX");
            imu = valid; imu.accelY = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite accelY");
            imu = valid; imu.accelZ = val;
            AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects non-finite accelZ");
        }

        StreamPhoneImu imu = valid;
        imu.accelX = 200.0F; imu.accelY = -200.0F; imu.accelZ = 0.0F;
        AssertFalse(SerializePhoneImu(imu).empty(), "Serialize accepts boundary accel (+-200.0 m/s^2)");

        imu = valid; imu.accelX = 200.001F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects accelX=200.001");
        imu = valid; imu.accelX = -200.001F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects accelX=-200.001");
        imu = valid; imu.accelY = 500.0F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects accelY=500.0");
        imu = valid; imu.accelZ = -500.0F;
        AssertTrue(SerializePhoneImu(imu).empty(), "Serialize rejects accelZ=-500.0");
    }

    // 3.9 Reserved bytes (bytes 44..47)
    {
        for (std::size_t offset = 44; offset < 48; ++offset) {
            auto b = validBytes;
            b[offset] = 1;
            AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error),
                        "Non-zero reserved byte at offset " + std::to_string(offset) + " rejected");
            b[offset] = 0xFF;
            AssertFalse(ParsePhoneImu(b.data(), b.size(), parsed, error),
                        "0xFF reserved byte at offset " + std::to_string(offset) + " rejected");
        }
    }
}

// ---------------------------------------------------------------------------
// Suite 4: Capability Negotiation Edge Cases
// ---------------------------------------------------------------------------
void TestCapabilityAdversarial() {
    using namespace phonecast::core::protocol;
    std::cout << "[Suite 4] Running Capability Negotiation Tests...\n";

    const auto cap = TrackingCapability();
    AssertTrue(cap.size() == 8, "TrackingCapability size is 8 bytes");
    AssertTrue(HasTrackingCapability(cap), "HasTrackingCapability recognizes itself");

    // Sub-size payloads
    for (std::size_t sz = 0; sz < 8; ++sz) {
        std::vector<std::uint8_t> shortBuf(cap.begin(), cap.begin() + sz);
        AssertFalse(HasTrackingCapability(shortBuf), "Sub-8-byte payload returns false");
    }

    // Corruptions in 8-byte token
    for (std::size_t i = 0; i < 8; ++i) {
        auto corrupt = cap;
        corrupt[i] ^= 0x5A;
        AssertFalse(HasTrackingCapability(corrupt), "Corrupted capability token rejected");
    }

    // Compound payloads
    std::vector<std::uint8_t> audioCap = {'P', 'C', 'A', 'C', 1, 1, 0, 0};
    std::vector<std::uint8_t> compound1 = audioCap;
    compound1.insert(compound1.end(), cap.begin(), cap.end());
    AssertTrue(HasTrackingCapability(compound1), "Capability found at offset 8");

    std::vector<std::uint8_t> compound2 = cap;
    compound2.insert(compound2.end(), audioCap.begin(), audioCap.end());
    AssertTrue(HasTrackingCapability(compound2), "Capability found at offset 0");

    std::vector<std::uint8_t> compound3 = audioCap;
    compound3.insert(compound3.end(), audioCap.begin(), audioCap.end());
    AssertFalse(HasTrackingCapability(compound3), "Capability not found in 2 audio tokens");

    // Trailing non-multiple of 8 bytes
    std::vector<std::uint8_t> oddCompound = compound1;
    oddCompound.push_back(0x42);
    oddCompound.push_back(0x43);
    AssertTrue(HasTrackingCapability(oddCompound), "Odd trailing bytes do not prevent detection");
}

// ---------------------------------------------------------------------------
// Suite 5: Fuzzing & Mutation Stress Harness
// ---------------------------------------------------------------------------
void TestFuzzingStressHarness() {
    using namespace phonecast::core::protocol;
    std::cout << "[Suite 5] Running Fuzzing & Mutation Stress Harness (20,000 iterations)...\n";

    std::mt19937 rng(42); // Deterministic seed for reproducible testing
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> bitDist(0, 7);

    // 5.1 Mutate valid ScreenMetrics
    StreamScreenMetrics baseMetrics;
    baseMetrics.version = 1;
    baseMetrics.flags = 0;
    baseMetrics.densityDpi = 420;
    baseMetrics.pixelWidth = 1080;
    baseMetrics.pixelHeight = 2400;
    baseMetrics.physicalWidthMm = 70.0F;
    baseMetrics.physicalHeightMm = 155.0F;
    baseMetrics.xdpi = 400.0F;
    baseMetrics.ydpi = 400.0F;
    const auto validMetricsBytes = SerializeScreenMetrics(baseMetrics);

    StreamScreenMetrics parsedMetrics;
    std::string error;
    int metricsAccepted = 0;
    int metricsRejected = 0;

    for (int iter = 0; iter < 10000; ++iter) {
        auto mutated = validMetricsBytes;
        // Flip 1 to 4 random bits
        int flips = 1 + (byteDist(rng) % 4);
        for (int f = 0; f < flips; ++f) {
            std::size_t offset = byteDist(rng) % mutated.size();
            mutated[offset] ^= static_cast<std::uint8_t>(1 << bitDist(rng));
        }

        bool result = ParseScreenMetrics(mutated.data(), mutated.size(), parsedMetrics, error);
        if (result) {
            ++metricsAccepted;
            // Verify that accepted mutated packet respects all invariants
            AssertTrue(parsedMetrics.version == 1, "Invariant: version 1");
            AssertTrue((parsedMetrics.flags & 0xFE) == 0, "Invariant: flags reserved 0");
            AssertTrue(parsedMetrics.densityDpi >= 1 && parsedMetrics.densityDpi <= 2000, "Invariant: density");
            AssertTrue(parsedMetrics.pixelWidth >= 1 && parsedMetrics.pixelWidth <= 16384, "Invariant: width");
            AssertTrue(parsedMetrics.pixelHeight >= 1 && parsedMetrics.pixelHeight <= 16384, "Invariant: height");
            AssertTrue(std::isfinite(parsedMetrics.physicalWidthMm) &&
                       parsedMetrics.physicalWidthMm >= 10.0F && parsedMetrics.physicalWidthMm <= 1000.0F, "Invariant: mm width");
            AssertTrue(std::isfinite(parsedMetrics.physicalHeightMm) &&
                       parsedMetrics.physicalHeightMm >= 10.0F && parsedMetrics.physicalHeightMm <= 1000.0F, "Invariant: mm height");
            AssertTrue(std::isfinite(parsedMetrics.xdpi) &&
                       parsedMetrics.xdpi >= 10.0F && parsedMetrics.xdpi <= 2000.0F, "Invariant: xdpi");
            AssertTrue(std::isfinite(parsedMetrics.ydpi) &&
                       parsedMetrics.ydpi >= 10.0F && parsedMetrics.ydpi <= 2000.0F, "Invariant: ydpi");
        } else {
            ++metricsRejected;
            AssertTrue(!error.empty(), "Error message set on failure");
        }
    }

    // 5.2 Mutate valid PhoneImu
    StreamPhoneImu baseImu;
    baseImu.version = 1;
    baseImu.sensorType = 1;
    baseImu.accuracy = 3;
    baseImu.flags = 0;
    baseImu.qx = 0.0F; baseImu.qy = 0.0F; baseImu.qz = 0.0F; baseImu.qw = 1.0F;
    baseImu.gyroX = 0.0F; baseImu.gyroY = 0.0F; baseImu.gyroZ = 0.0F;
    baseImu.accelX = 0.0F; baseImu.accelY = 9.81F; baseImu.accelZ = 0.0F;
    const auto validImuBytes = SerializePhoneImu(baseImu);

    StreamPhoneImu parsedImu;
    int imuAccepted = 0;
    int imuRejected = 0;

    for (int iter = 0; iter < 10000; ++iter) {
        auto mutated = validImuBytes;
        int flips = 1 + (byteDist(rng) % 4);
        for (int f = 0; f < flips; ++f) {
            std::size_t offset = byteDist(rng) % mutated.size();
            mutated[offset] ^= static_cast<std::uint8_t>(1 << bitDist(rng));
        }

        bool result = ParsePhoneImu(mutated.data(), mutated.size(), parsedImu, error);
        if (result) {
            ++imuAccepted;
            // Verify all invariants
            AssertTrue(parsedImu.version == 1, "Invariant: version 1");
            AssertTrue(parsedImu.sensorType == 1 || parsedImu.sensorType == 2, "Invariant: sensorType");
            AssertTrue(parsedImu.accuracy <= 3, "Invariant: accuracy");
            AssertTrue((parsedImu.flags & 0xFC) == 0, "Invariant: flags");
            AssertTrue(std::isfinite(parsedImu.qx) && std::isfinite(parsedImu.qy) &&
                       std::isfinite(parsedImu.qz) && std::isfinite(parsedImu.qw), "Invariant: quat finite");
            float normSq = parsedImu.qx * parsedImu.qx + parsedImu.qy * parsedImu.qy +
                           parsedImu.qz * parsedImu.qz + parsedImu.qw * parsedImu.qw;
            AssertTrue(std::abs(normSq - 1.0F) <= 0.05F, "Invariant: quat normSq");
            AssertTrue(std::isfinite(parsedImu.gyroX) && std::abs(parsedImu.gyroX) <= 50.0F, "Invariant: gyroX");
            AssertTrue(std::isfinite(parsedImu.gyroY) && std::abs(parsedImu.gyroY) <= 50.0F, "Invariant: gyroY");
            AssertTrue(std::isfinite(parsedImu.gyroZ) && std::abs(parsedImu.gyroZ) <= 50.0F, "Invariant: gyroZ");
            AssertTrue(std::isfinite(parsedImu.accelX) && std::abs(parsedImu.accelX) <= 200.0F, "Invariant: accelX");
            AssertTrue(std::isfinite(parsedImu.accelY) && std::abs(parsedImu.accelY) <= 200.0F, "Invariant: accelY");
            AssertTrue(std::isfinite(parsedImu.accelZ) && std::abs(parsedImu.accelZ) <= 200.0F, "Invariant: accelZ");
        } else {
            ++imuRejected;
            AssertTrue(!error.empty(), "Error message set on failure");
        }
    }

    // 5.3 Pure Garbage fuzzer across header, metrics, and IMU
    std::uniform_int_distribution<std::size_t> lenDist(0, 128);
    Message garbageMsg;
    std::uint32_t garbagePayloadSize = 0;
    for (int iter = 0; iter < 5000; ++iter) {
        std::size_t len = lenDist(rng);
        std::vector<std::uint8_t> garbage(len);
        for (std::size_t i = 0; i < len; ++i) {
            garbage[i] = static_cast<std::uint8_t>(byteDist(rng));
        }

        // None of these should crash, throw, or cause memory corruptions
        ParseHeader(garbage.data(), garbage.size(), garbageMsg, garbagePayloadSize, error);
        ParseScreenMetrics(garbage.data(), garbage.size(), parsedMetrics, error);
        ParsePhoneImu(garbage.data(), garbage.size(), parsedImu, error);
    }

    std::cout << "  Fuzzing summary:\n"
              << "    ScreenMetrics: " << metricsAccepted << " accepted, " << metricsRejected << " rejected\n"
              << "    PhoneImu:      " << imuAccepted << " accepted, " << imuRejected << " rejected\n"
              << "    Garbage cycles: 5,000 completed with 0 crashes\n";
}

} // namespace

int main() {
    std::cout << "=== Adversarial Protocol Stress Harness ===\n";
    TestParseHeaderAdversarial();
    TestScreenMetricsAdversarial();
    TestPhoneImuAdversarial();
    TestCapabilityAdversarial();
    TestFuzzingStressHarness();

    std::cout << "\n============================================\n";
    std::cout << "Total Assertions: " << totalAssertions << '\n';
    std::cout << "Total Failures:   " << failureCount << '\n';
    std::cout << "============================================\n";

    if (failureCount == 0) {
        std::cout << "ALL ADVERSARIAL PROTOCOL TESTS PASSED!\n";
        return 0;
    } else {
        std::cerr << "FAILURES DETECTED IN ADVERSARIAL PROTOCOL SUITE!\n";
        return 1;
    }
}
