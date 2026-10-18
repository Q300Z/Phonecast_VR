#include "ITrackedCameraSource.h"
#include "TrackedCameraWorker.h"
#include "phonecast/core/logging/ILogger.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/tracking/TrackingTypes.h"

#include <openvr.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const std::string& name, const std::string& detail = "") {
    if (condition) {
        ++g_pass;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << '\n';
}

class TestLogger : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel level, std::string_view category,
             std::string_view message) override {
        (void)level;
        (void)category;
        (void)message;
    }
};

class AdversarialMockCameraSource : public phonecast::platform::openvr::ITrackedCameraSource {
public:
    std::atomic<bool> hasCamera{true};
    uint32_t width{640};
    uint32_t height{480};
    uint32_t bytesPerPixel{4};
    phonecast::core::tracking::CameraIntrinsics intrinsics{750.0F, 750.0F, 320.0F, 240.0F, 640, 480};
    ::vr::HmdMatrix34_t cameraToHead{{{1.0F, 0.0F, 0.0F, 0.0F},
                                     {0.0F, 1.0F, 0.0F, 0.0F},
                                     {0.0F, 0.0F, 1.0F, 0.0F}}};

    std::atomic<bool> streamingActive{false};
    std::atomic<int> acquireCount{0};
    std::atomic<int> releaseCount{0};
    std::atomic<int> getFrameCount{0};

    std::vector<uint8_t> frameData;
    ::vr::CameraVideoStreamFrameHeader_t nextHeader{};
    std::atomic<::vr::EVRTrackedCameraError> nextError{::vr::VRTrackedCameraError_None};
    std::mutex sourceMutex;

    AdversarialMockCameraSource() {
        frameData.resize(width * height * bytesPerPixel, 10);
        nextHeader.nWidth = width;
        nextHeader.nHeight = height;
        nextHeader.nBytesPerPixel = bytesPerPixel;
        nextHeader.nFrameSequence = 1;
        nextHeader.ulFrameExposureTime = 1000000;
        nextHeader.trackedDevicePose.bPoseIsValid = true;
        nextHeader.trackedDevicePose.bDeviceIsConnected = true;
        nextHeader.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                                   {0.0F, 1.0F, 0.0F, 1.7F},
                                                                   {0.0F, 0.0F, 1.0F, 0.0F}}};
    }

    bool HasCamera() override {
        return hasCamera.load();
    }

    bool GetFrameSize(uint32_t& outWidth, uint32_t& outHeight, uint32_t& outBufferSize) override {
        outWidth = width;
        outHeight = height;
        outBufferSize = width * height * bytesPerPixel;
        return true;
    }

    bool GetIntrinsics(phonecast::core::tracking::CameraIntrinsics& outIntrinsics) override {
        outIntrinsics = intrinsics;
        return true;
    }

    bool GetCameraToHeadTransform(::vr::HmdMatrix34_t& outTransform) override {
        outTransform = cameraToHead;
        return true;
    }

    bool AcquireStreaming() override {
        streamingActive.store(true);
        acquireCount.fetch_add(1);
        return true;
    }

    void ReleaseStreaming() override {
        streamingActive.store(false);
        releaseCount.fetch_add(1);
    }

    ::vr::EVRTrackedCameraError GetFrame(
        void* buffer, uint32_t bufferSize,
        ::vr::CameraVideoStreamFrameHeader_t& header) override {
        getFrameCount.fetch_add(1);
        const auto err = nextError.load();
        if (err != ::vr::VRTrackedCameraError_None) {
            return err;
        }
        std::lock_guard<std::mutex> lock(sourceMutex);
        if (buffer && bufferSize >= frameData.size()) {
            std::memcpy(buffer, frameData.data(), frameData.size());
        }
        header = nextHeader;
        nextHeader.nFrameSequence++;
        return ::vr::VRTrackedCameraError_None;
    }

    void RenderSyntheticRect(uint32_t xMin, uint32_t yMin, uint32_t xMax, uint32_t yMax) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        std::fill(frameData.begin(), frameData.end(), 10);
        for (uint32_t y = yMin; y <= yMax && y < height; ++y) {
            for (uint32_t x = xMin; x <= xMax && x < width; ++x) {
                const uint32_t idx = (y * width + x) * bytesPerPixel;
                frameData[idx + 0] = 240;
                frameData[idx + 1] = 240;
                frameData[idx + 2] = 240;
                frameData[idx + 3] = 255;
            }
        }
    }
};

// ============================================================================
// Suite 1: Rapid Lifecycle Stress Transitions (Cycles, Idempotence, Concurrency)
// ============================================================================
void TestLifecycleRapidCycles() {
    std::cout << "\n=== [Suite 1.1] Rapid Lifecycle Cycles (Start -> SetActive -> Stop) ===\n";
    TestLogger logger;

    constexpr int kCycles = 200;
    bool allClean = true;
    auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < kCycles; ++i) {
        auto mock = std::make_unique<AdversarialMockCameraSource>();
        phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mock));

        if (!worker.Start()) {
            allClean = false;
            break;
        }
        worker.SetActive(true);
        // Short pause in some iterations, 0 pause in others
        if (i % 4 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        worker.SetActive(false);
        worker.Stop();

        if (worker.IsActive()) {
            allClean = false;
            break;
        }
    }
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    Check(allClean, "200 rapid sequential lifecycle cycles completed cleanly",
          std::to_string(elapsedMs) + " ms total, avg " + std::to_string(elapsedMs / (float)kCycles) + " ms/cycle");
}

void TestLifecycleIdempotenceAndImmediateStop() {
    std::cout << "\n=== [Suite 1.2] Lifecycle Idempotence & Immediate Stop Stress ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    // Multiple Start calls
    Check(worker.Start(), "first Start() returns true");
    Check(worker.Start(), "second Start() is idempotent and returns true");
    Check(worker.Start(), "third Start() is idempotent and returns true");

    // Multiple SetActive calls
    worker.SetActive(true);
    Check(worker.IsActive(), "SetActive(true) sets IsActive() == true");
    worker.SetActive(true);
    Check(worker.IsActive(), "repeated SetActive(true) remains active");

    worker.SetActive(false);
    Check(!worker.IsActive(), "SetActive(false) sets IsActive() == false");
    worker.SetActive(false);
    Check(!worker.IsActive(), "repeated SetActive(false) remains inactive");

    // Multiple Stop calls
    worker.Stop();
    Check(!worker.IsActive(), "worker inactive after Stop()");
    worker.Stop();
    worker.Stop();
    Check(!worker.IsActive(), "repeated Stop() is idempotent and does not crash");

    // 100 immediate Start -> Stop cycles (zero delay)
    bool immediateClean = true;
    for (int i = 0; i < 100; ++i) {
        auto m = std::make_unique<AdversarialMockCameraSource>();
        phonecast::platform::openvr::TrackedCameraWorker w(logger, std::move(m));
        w.Start();
        w.Stop();
        if (w.IsActive()) {
            immediateClean = false;
            break;
        }
    }
    Check(immediateClean, "100 immediate Start()->Stop() calls with zero delay succeeded without hang or leak");
}

void TestLifecycleConcurrentHammer() {
    std::cout << "\n=== [Suite 1.3] Multi-Threaded Concurrent Lifecycle Hammer ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    mockPtr->RenderSyntheticRect(268, 128, 372, 352);
    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    worker.Start();

    std::atomic<bool> running{true};
    std::atomic<uint64_t> activeToggles{0};
    std::atomic<uint64_t> metricsUpdates{0};
    std::atomic<uint64_t> imuPushes{0};
    std::atomic<uint64_t> snapshotReads{0};

    // Thread 1: Rapidly toggling SetActive(true) and SetActive(false)
    std::thread toggleThread([&] {
        bool state = true;
        while (running) {
            worker.SetActive(state);
            state = !state;
            activeToggles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 2: Continuously updating metrics
    std::thread metricsThread([&] {
        float w = 70.0F;
        while (running) {
            phonecast::core::protocol::StreamScreenMetrics m{};
            m.physicalWidthMm = w;
            m.physicalHeightMm = 150.0F;
            m.pixelWidth = 1080;
            m.pixelHeight = 2400;
            worker.SetScreenMetrics(m);
            w = (w >= 80.0F) ? 70.0F : (w + 0.5F);
            metricsUpdates.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 3: Continuously ingesting IMU
    std::thread imuThread([&] {
        float angle = 0.0F;
        while (running) {
            phonecast::core::protocol::StreamPhoneImu imu{};
            imu.qw = std::cos(angle * 0.5F);
            imu.qy = std::sin(angle * 0.5F);
            worker.IngestPhoneImu(imu);
            angle += 0.01F;
            imuPushes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 4: Continuously reading snapshots
    std::thread readerThread([&] {
        while (running) {
            auto snap = worker.GetSnapshot();
            (void)snap;
            ::vr::HmdMatrix34_t pose{};
            float width = 0.0F;
            worker.GetLatestPose(pose, &width);
            snapshotReads.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    running = false;

    toggleThread.join();
    metricsThread.join();
    imuThread.join();
    readerThread.join();

    worker.Stop();

    Check(activeToggles > 500, "active toggles under concurrency", std::to_string(activeToggles.load()));
    Check(metricsUpdates > 500, "metrics updates under concurrency", std::to_string(metricsUpdates.load()));
    Check(imuPushes > 500, "imu pushes under concurrency", std::to_string(imuPushes.load()));
    Check(snapshotReads > 500, "snapshot reads under concurrency", std::to_string(snapshotReads.load()));
}

// ============================================================================
// Suite 2: Low-Power Idle & Condition Variable Zero-CPU Verification
// ============================================================================
void TestLowPowerIdleBehavior() {
    std::cout << "\n=== [Suite 2] Low-Power Idle & Condition Variable Verification ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    worker.Start();

    // 1. Initial idle: active_ == false
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    Check(!worker.IsActive(), "worker starts inactive");
    Check(mock->acquireCount.load() == 0, "mock acquireCount is 0 while inactive");
    Check(mock->getFrameCount.load() == 0, "0 camera frames queried while inactive");
    Check(!mock->streamingActive.load(), "streamingActive is false");

    // 2. Activate: active_ == true
    auto tStartWake = std::chrono::steady_clock::now();
    worker.SetActive(true);
    while (mock->getFrameCount.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto tWakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStartWake).count();
    Check(tWakeMs < 30, "wake latency from condition variable is fast", std::to_string(tWakeMs) + " ms");
    Check(mock->acquireCount.load() == 1, "streaming service acquired once upon activation");
    Check(mock->streamingActive.load(), "streaming service is active");

    // 3. Deactivate: active_ == false -> must release streaming & park
    worker.SetActive(false);
    while (mock->streamingActive.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(!mock->streamingActive.load(), "streaming service released immediately on SetActive(false)");
    Check(mock->releaseCount.load() == 1, "releaseCount is 1 on suspension");

    const int frameCountAtSuspension = mock->getFrameCount.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    Check(mock->getFrameCount.load() == frameCountAtSuspension,
          "zero camera frames queried during suspension (thread safely parked on CV)",
          "frames remained at " + std::to_string(frameCountAtSuspension));

    worker.Stop();
}

// ============================================================================
// Suite 3: Snapshot Concurrency, Data Consistency & Lock Contention
// ============================================================================
void TestSnapshotConcurrencyAndIntegrity() {
    std::cout << "\n=== [Suite 3] Snapshot Concurrency, Data Consistency & Lock Latency ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    auto* mock = mockPtr.get();
    mock->RenderSyntheticRect(268, 128, 372, 352);

    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metrics{};
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    worker.SetScreenMetrics(metrics);

    phonecast::core::protocol::StreamPhoneImu imu{};
    imu.qw = 1.0F;
    worker.IngestPhoneImu(imu);

    worker.Start();
    worker.SetActive(true);

    // Wait until initial tracking is established
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (worker.GetSnapshot().valid) break;
    }
    Check(worker.GetSnapshot().valid, "initial valid tracking established");

    std::atomic<bool> running{true};
    std::atomic<uint64_t> tornReads{0};
    std::atomic<uint64_t> nanValues{0};
    std::atomic<uint64_t> inconsistentStates{0};
    std::atomic<uint64_t> totalReads{0};
    std::atomic<uint64_t> maxReadLatencyNs{0};

    // Reader thread 1 (simulating OpenVR render loop at 90/144 Hz)
    std::thread renderReader([&] {
        while (running) {
            auto t0 = std::chrono::high_resolution_clock::now();
            ::vr::HmdMatrix34_t pose{};
            float width = 0.0F;
            const bool hasPose = worker.GetLatestPose(pose, &width);
            auto t1 = std::chrono::high_resolution_clock::now();

            const uint64_t latNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            uint64_t curMax = maxReadLatencyNs.load(std::memory_order_relaxed);
            while (latNs > curMax && !maxReadLatencyNs.compare_exchange_weak(curMax, latNs)) {}

            totalReads.fetch_add(1, std::memory_order_relaxed);

            if (hasPose) {
                // Verify matrix is finite (no NaNs, no Infs)
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        if (std::isnan(pose.m[r][c]) || std::isinf(pose.m[r][c])) {
                            nanValues.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                // Verify width is positive and non-zero
                if (width <= 0.0F || width > 1.0F) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });

    // Reader thread 2 (simulating UI thread snapshot reader)
    std::thread uiReader([&] {
        while (running) {
            auto snap = worker.GetSnapshot();
            totalReads.fetch_add(1, std::memory_order_relaxed);
            if (snap.valid) {
                if (snap.state == phonecast::core::tracking::TrackingState::Lost) {
                    inconsistentStates.fetch_add(1, std::memory_order_relaxed);
                }
                if (snap.physicalWidthMeters <= 0.0F) {
                    tornReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });

    // Concurrently perturb metrics and IMUs
    std::thread perturber([&] {
        float toggle = 70.0F;
        while (running) {
            phonecast::core::protocol::StreamScreenMetrics m{};
            m.physicalWidthMm = toggle;
            m.physicalHeightMm = 150.0F;
            m.pixelWidth = 1080;
            m.pixelHeight = 2400;
            worker.SetScreenMetrics(m);
            toggle = (toggle == 70.0F) ? 75.0F : 70.0F;

            phonecast::core::protocol::StreamPhoneImu pImu{};
            pImu.qw = 1.0F;
            worker.IngestPhoneImu(pImu);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    running = false;

    renderReader.join();
    uiReader.join();
    perturber.join();

    worker.Stop();

    Check(tornReads.load() == 0, "zero torn reads detected under concurrency",
          "total reads: " + std::to_string(totalReads.load()));
    Check(nanValues.load() == 0, "zero NaN/Inf values observed in pose matrices");
    Check(inconsistentStates.load() == 0, "zero inconsistent state/valid snapshots");

    const double maxLatUs = maxReadLatencyNs.load() / 1000.0;
    Check(maxLatUs < 1000.0, "GetLatestPose max lock latency is < 1.0 ms",
          "max latency = " + std::to_string(maxLatUs) + " us");
}

// ============================================================================
// Suite 4: Clean Destruction & Latency SLA (< 50 ms)
// ============================================================================
void TestCleanDestructionSla() {
    std::cout << "\n=== [Suite 4] Clean Destruction Timing SLA (< 50 ms) & Release ===\n";
    TestLogger logger;

    constexpr int kTrials = 100;
    double maxStopMs = 0.0;
    double totalStopMs = 0.0;
    bool allReleased = true;

    for (int i = 0; i < kTrials; ++i) {
        auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
        auto* mock = mockPtr.get();
        mock->RenderSyntheticRect(268, 128, 372, 352);

        auto worker = std::make_unique<phonecast::platform::openvr::TrackedCameraWorker>(
            logger, std::move(mockPtr));

        worker->Start();
        worker->SetActive(true);

        // Let it run for a variable brief duration (0 to 10 ms)
        if (i % 5 != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        worker->Stop();
        auto t1 = std::chrono::high_resolution_clock::now();

        const double stopMs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e6;
        maxStopMs = std::max(maxStopMs, stopMs);
        totalStopMs += stopMs;

        // Verify camera streaming is released before destruction completes
        if (mock->streamingActive.load()) {
            allReleased = false;
        }

        worker.reset();
    }

    const double avgStopMs = totalStopMs / kTrials;
    Check(maxStopMs < 50.0, "maximum Stop() duration meets 50 ms SLA",
          "max = " + std::to_string(maxStopMs) + " ms, avg = " + std::to_string(avgStopMs) + " ms");
    Check(allReleased, "camera streaming is strictly released on or before Stop() completion");
}

// ============================================================================
// Suite 5: Pathological Input & Error Boundary Stress
// ============================================================================
void TestPathologicalInputStress() {
    std::cout << "\n=== [Suite 5] Pathological Input & Frame Corruption Stress ===\n";
    TestLogger logger;

    auto mockPtr = std::make_unique<AdversarialMockCameraSource>();
    phonecast::platform::openvr::TrackedCameraWorker worker(logger, std::move(mockPtr));

    phonecast::core::protocol::StreamScreenMetrics metrics{};
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 150.0F;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    worker.SetScreenMetrics(metrics);

    // 1. Corrupted / all-zero buffer
    std::vector<uint8_t> blackFrame(640 * 480 * 4, 0);
    ::vr::CameraVideoStreamFrameHeader_t header{};
    header.nWidth = 640;
    header.nHeight = 480;
    header.nBytesPerPixel = 4;
    header.nFrameSequence = 10;
    header.trackedDevicePose.bPoseIsValid = true;
    header.trackedDevicePose.mDeviceToAbsoluteTracking = {{{1.0F, 0.0F, 0.0F, 0.0F},
                                                           {0.0F, 1.0F, 0.0F, 1.7F},
                                                           {0.0F, 0.0F, 1.0F, 0.0F}}};

    worker.ProcessFrame(blackFrame.data(), header);
    auto snap = worker.GetSnapshot();
    Check(!snap.valid, "black frame does not produce false positive pose");

    // 2. High noise frame (random noise)
    std::vector<uint8_t> noiseFrame(640 * 480 * 4);
    std::mt19937 rng(42);
    for (auto& b : noiseFrame) b = static_cast<uint8_t>(rng() % 256);

    header.nFrameSequence = 11;
    worker.ProcessFrame(noiseFrame.data(), header);
    snap = worker.GetSnapshot();
    Check(!snap.valid, "pure noise frame rejected by RANSAC line fitter");

    // 3. Degenerate screen metrics (0mm or negative width)
    metrics.physicalWidthMm = -10.0F;
    metrics.physicalHeightMm = 0.0F;
    worker.SetScreenMetrics(metrics);

    header.nFrameSequence = 12;
    worker.ProcessFrame(noiseFrame.data(), header);
    snap = worker.GetSnapshot();
    Check(!snap.valid, "degenerate screen metrics safely rejected without crash or division by zero");

    // 4. Invalid HMD pose flag
    header.trackedDevicePose.bPoseIsValid = false;
    header.nFrameSequence = 13;
    worker.ProcessFrame(noiseFrame.data(), header);
    snap = worker.GetSnapshot();
    Check(!snap.valid, "invalid HMD pose rejected");

    worker.Stop();
}

} // namespace

int main() {
    std::cout << "=================================================================\n";
    std::cout << "CHALLENGER 1: TrackedCameraWorker Concurrency & Lifecycle Stress\n";
    std::cout << "=================================================================\n";

    TestLifecycleRapidCycles();
    TestLifecycleIdempotenceAndImmediateStop();
    TestLifecycleConcurrentHammer();
    TestLowPowerIdleBehavior();
    TestSnapshotConcurrencyAndIntegrity();
    TestCleanDestructionSla();
    TestPathologicalInputStress();

    std::cout << "\n=================================================================\n";
    std::cout << "TEST RESULT SUMMARY: " << g_pass << " PASSED, " << g_fail << " FAILED\n";
    std::cout << "=================================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
