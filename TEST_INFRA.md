# PhoneCast VR — End-to-End Tracking Test Infrastructure (`TEST_INFRA.md`)

## 1. Overview & Test Architecture

The PhoneCast VR End-to-End Tracking Test Suite (`phonecast-e2e-tracking-tests`, source: `tests/e2e_tracking_suite_tests.cpp`) provides comprehensive, opaque-box, mathematically rigorous verification of the entire PhoneCast VR phone tracking pipeline.

### Core Architecture & Boundaries
The testing framework spans the complete pipeline across all architectural tiers:
1. **Network & Protocol Tier**: Protocol serialization, deserialization, network buffering, bounded queues, token capability negotiation (`PCTR`), and session generation isolation (`core/protocol`, `platform/network`).
2. **Android Extraction Simulation**: Hardware display metrics translation, sensor sampling rates, timestamp nanosecond-to-microsecond conversions, and stale drop policies (`android/`).
3. **Core Geometric & Computer Vision Pipeline**: Eigen3 interop, subpixel parabolic edge detection, RANSAC line fitting, analytical closed-form 4-corner intersection, Planar PnP (DLT + SVD + Lu-Hager-Mjolsness iterative refinement), and gravity vector pose disambiguation (`core/tracking`).
4. **Temporal Filtering & Sensor Fusion**: 60 Hz 1-Euro adaptive low-pass positional/rotational filtering, 50-60 Hz inertial pose fusion with dynamic yaw boresighting, and dead reckoning under visual occlusion (`core/tracking`).
5. **OpenVR & SteamVR Integration Tier**: Tracked camera worker lifecycle (`ITrackedCameraSource`, `TrackedCameraWorker`), coordinate frame compounding (`Standing <- HMD <- Camera <- Phone`), automatic 1:1 metric scale matching, 1.5s occlusion hold policy, and UI mode switching (`vr/overlay`, `platform/openvr`).

### Testing Philosophy: Anti-Facade, Real-Logic Execution
In accordance with strict test integrity mandates:
- **Zero Mock Tautologies**: No dummy interfaces that unconditionally return hardcoded truths. The test suite operates on real OpenVR coordinate transforms, real Eigen SVD matrices, real RANSAC line fitting on synthetic edge point clouds, real DLT/LHM iterative minimization, and real loopback TCP sockets.
- **Physical Decoupling without Loss of Realism**: Uses a synthetic tracked camera source (`MockTrackedCameraSource`) capable of rendering realistic quadrilateral phone screens into synthetic pixel buffers with camera noise, distortion, and occlusions, fully verifying `TrackedCameraWorker` without physical hardware dependencies.

---

## 2. 4-Tier Test Suite Structure

The test suite is structured into four progressive tiers totaling **208 assertions** across 38 dedicated test suites:

### Tier 1: Feature Coverage (Features 1 through 20)
Isolates and validates the happy-path behavior of every single feature specified in `PROJECT.md`:
- **1.1 F-PROT-METRICS**: Serializes and parses `StreamScreenMetrics` (Type 15, 32 bytes) with exact field round-trip validation.
- **1.2 F-PROT-IMU**: Serializes and parses `StreamPhoneImu` (Type 16, 48 bytes) covering quaternion, gyroscope, and accelerometer fields.
- **1.3 F-PROT-NEGOT**: Tests `TrackingCapability()` token generation (`PCTR`), detection in standalone and combined streams, and rejection of foreign tokens.
- **1.4 F-NET-QUEUE**: Tests `TcpVideoServer`'s isolated bounded IMU ring buffer (16 entries), FIFO drop-oldest policy, and dropped packet counter.
- **1.5 F-AND-METRICS**: Simulates Android `DisplayMetrics` extraction, converting pixel dimensions and DPI to physical millimeter screen metrics.
- **1.6 F-AND-IMU**: Simulates Android IMU sampling rate (50-60 Hz), nanosecond to microsecond conversion, and 50ms stale packet drop policy.
- **1.7 F-CORE-TYPES**: Tests header-only C++17 portable geometric types (`Vector3`, `Quaternion`, `Pose6D`, `TrackingState`).
- **1.8 F-CORE-MATH**: Verifies Eigen 3 interop conversions (`ToEigen`, `FromEigen`) and SVD singular value decomposition.
- **1.9 F-CORE-RANSAC**: Validates subpixel parabolic peak interpolation and RANSAC line fitting robustness under 30% synthetic outlier noise.
- **1.10 F-CORE-CORNER**: Analytical closed-form intersection of four fitted boundary lines into four subpixel quadrilateral corners.
- **1.11 F-CORE-PNP**: Planar PnP solver combining Direct Linear Transform (DLT), SVD initialization, and Lu-Hager-Mjolsness (LHM) iterative refinement achieving < 0.5px reprojection RMSE.
- **1.12 F-CORE-DISAMBIG**: Gravity vector dot-product pose disambiguation selecting the physically upright solution between two planar pose hypotheses.
- **1.13 F-CORE-FUSION**: Inertial pose fusion with yaw boresighting and kinematic prediction.
- **1.14 F-CORE-FILTER**: Adaptive 1-Euro filter eliminating static jitter (< 0.02 amplitude) while dynamically following fast steps within 10 frames.
- **1.15 F-VR-MODE**: Overlay settings persistence (`OverlaySettingsStore`), menu display (`SettingsMenuController`), and `PlacementMode::PhoneTracked` state application.
- **1.16 F-OVR-WORKER**: Thread lifecycle of `TrackedCameraWorker`, verifying decoupled start, active acquisition, suspension, and clean stop.
- **1.17 F-OVR-TRANSFORM**: OpenVR rigid transform compounding (`Standing <- HMD <- Camera <- Phone`), placing the virtual overlay accurately in world space.
- **1.18 F-OVR-SCALE**: Automatic 1:1 metric scale matching, calculating physical quad width from screen metrics without user scaling.
- **1.19 F-OVR-HOLD**: 1.5-second occlusion hold behavior, freezing position stably during optical dropouts without snap-back.
- **1.20 Stream-Receiver Forwarding**: End-to-end integration verifying `TcpVideoServer` ingests metrics/IMU over loopback TCP and forwards them to `MockOverlayRenderer`.

### Tier 2: Boundary & Corner Cases
Stress tests numerical limits, extreme angles, and timing anomalies:
- **2.1 Zero & Negative Dimensions**: Rejection of zero/negative widths, heights, and pixel counts in `ScreenMetrics` and `PlanarPnpSolver`.
- **2.2 Grazing Angles (70° to 80° Oblique Tilt)**: Verification that Planar PnP solves steep oblique viewing angles with position error < 15mm and orientation error < 2.5°.
- **2.3 Near-Fronto-Parallel Micro-Tilts (0.0001° to 0.5°)**: Verifies numerical stability without singularity or division by zero.
- **2.4 180° Yaw Turnaround**: Verifies full phone rotation without gimbal lock or NaN values.
- **2.5 Antipodal Quaternion Flips**: Shortest-path quaternion dot product continuity ($q \to -q$).
- **2.6 Extreme Time Intervals ($dt \to 0$ and $dt = 5.0\text{s}$)**: Filter stability under clock freezes and large pauses.
- **2.7 Single Frame Optical Drop**: Seamless state retention during 1-frame visual occlusion.
- **2.8 1.5s Occlusion Hold Boundary**: Exact timeout transition from `Tracking` $\to$ `Occluded` $\to$ `Lost` at $t = 1.5\text{s}$.
- **2.9 Extreme Aspect Ratios**: Solves tall foldable screens (23:9) and ultrawide screens (32:9).

### Tier 3: Cross-Feature Interactions
Tests asynchronous multi-component concurrency and failure recovery:
- **3.1 Metrics Update During IMU Streaming**: Dynamically pushes new screen dimensions while a 60 Hz IMU stream is running, verifying thread safety and zero deadlock.
- **3.2 Visual Drop with Sustained Inertial Rotation**: Simulates camera blackout while the phone continues rotating via IMU dead reckoning.
- **3.3 Rapid PlacementMode Cycling**: Cycles `HeadLocked` $\leftrightarrow$ `PhoneTracked` 3 times, verifying camera stream acquisition and release.
- **3.4 TCP Disconnect / Reconnect Session Isolation**: Validates `TcpVideoServer` generation counter increment, socket teardown, reconnection, and bounded queue purging between clients.

### Tier 4: Real-World Workload Scenarios
Simulates realistic end-to-end VR usage sessions:
- **4.1 Scenario 1: Phone Pickup & Examine**: User lifts phone from desk towards face ($Z = -0.70\text{m} \to -0.48\text{m}$), verifying continuous optical-inertial tracking.
- **4.2 Scenario 2: Seated to Standing Room Walk**: User stands up ($Y = 1.15\text{m} \to 1.75\text{m}$) and walks across the room ($Z = -0.5\text{m} \to -2.0\text{m}$).
- **4.3 Scenario 3: Thumb Occlusion During Typing**: Synthetic thumb covering phone edge; RANSAC line fitter rejects occluded points and successfully solves pose.
- **4.4 Scenario 4: Head Turnaround Visual Drop**: User turns head 180° away from phone for 600ms; occlusion hold maintains stable position.
- **4.5 Scenario 5: Re-entry Relocalization**: Phone leaves camera view beyond 1.5s (state enters `Lost`), then re-enters FOV and instantaneously relocalizes.

---

## 3. 20-Feature Traceability Matrix

| Feature ID | Feature Name | Implementation Files | E2E Test Suite Reference |
|---|---|---|---|
| **1** | `F-PROT-METRICS` | `core/src/StreamProtocol.cpp`, `core/include/phonecast/core/protocol/StreamProtocol.h` | Tier 1: §1.1 |
| **2** | `F-PROT-IMU` | `core/src/StreamProtocol.cpp`, `core/include/phonecast/core/protocol/StreamProtocol.h` | Tier 1: §1.2 |
| **3** | `F-PROT-NEGOT` | `core/src/StreamProtocol.cpp`, `core/include/phonecast/core/protocol/StreamProtocol.h` | Tier 1: §1.3 |
| **4** | `F-NET-QUEUE` | `platform/network/src/TcpVideoServer.cpp`, `platform/network/include/.../TcpVideoServer.h` | Tier 1: §1.4, Tier 3: §3.4 |
| **5** | `F-AND-METRICS` | `android/PhoneCastStream/app/src/main/java/.../ScreenMetricsProvider.kt` | Tier 1: §1.5 |
| **6** | `F-AND-IMU` | `android/PhoneCastStream/app/src/main/java/.../PhoneImuStreamer.kt` | Tier 1: §1.6 |
| **7** | `F-CORE-TYPES` | `core/include/phonecast/core/tracking/TrackingTypes.h` | Tier 1: §1.7 |
| **8** | `F-CORE-MATH` | `core/include/phonecast/core/tracking/EigenInterop.h` | Tier 1: §1.8 |
| **9** | `F-CORE-RANSAC` | `core/src/tracking/LineFitterRansac.cpp`, `core/include/.../LineFitterRansac.h` | Tier 1: §1.9, Tier 4: §4.3 |
| **10** | `F-CORE-CORNER` | `core/src/tracking/CornerIntersection.cpp`, `core/include/.../CornerIntersection.h` | Tier 1: §1.10 |
| **11** | `F-CORE-PNP` | `core/src/tracking/PlanarPnpSolver.cpp`, `core/include/.../PlanarPnpSolver.h` | Tier 1: §1.11, Tier 2: §2.2, §2.3 |
| **12** | `F-CORE-DISAMBIG`| `core/src/tracking/GravityDisambiguation.cpp`, `core/include/.../PlanarPnpSolver.h` | Tier 1: §1.12 |
| **13** | `F-CORE-FUSION` | `core/src/tracking/InertialPoseFusion.cpp`, `core/include/.../InertialPoseFusion.h` | Tier 1: §1.13, Tier 3: §3.2 |
| **14** | `F-CORE-FILTER` | `core/src/tracking/OneEuroFilter.cpp`, `core/include/.../OneEuroFilter.h` | Tier 1: §1.14, Tier 2: §2.6 |
| **15** | `F-VR-MODE` | `vr/overlay/src/OverlaySettingsStore.cpp`, `vr/overlay/src/SettingsMenuController.cpp` | Tier 1: §1.15, Tier 3: §3.3 |
| **16** | `F-OVR-WORKER` | `platform/openvr/src/TrackedCameraWorker.cpp`, `platform/openvr/include/...` | Tier 1: §1.16, Tier 3: §3.3 |
| **17** | `F-OVR-TRANSFORM`| `platform/openvr/src/TrackedCameraWorker.cpp`, `vr/overlay/src/OverlayController.cpp`| Tier 1: §1.17, Tier 4: §4.2 |
| **18** | `F-OVR-SCALE` | `platform/openvr/src/TrackedCameraWorker.cpp` | Tier 1: §1.18 |
| **19** | `F-OVR-HOLD` | `platform/openvr/src/TrackedCameraWorker.cpp` | Tier 1: §1.19, Tier 2: §2.8, Tier 4: §4.4 |
| **20** | `F-VR-RECV` | `vr/stream_receiver/src/main.cpp`, `platform/network/src/TcpVideoServer.cpp` | Tier 1: §1.20 |

---

## 4. Test Runner Instructions

### Building the Test Suite
The test target is configured via CMake and integrated into CTest:
```bash
cmake -B out/build/linux-test -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/linux-test --target phonecast-e2e-tracking-tests
```

### Running the Standalone E2E Executable
To run the verbose 4-tier tracking test suite directly:
```bash
./out/build/linux-test/tests/phonecast-e2e-tracking-tests
```
Expected output:
```
======================================================================
E2E TRACKING TEST SUITE SUMMARY
======================================================================
Tier 1 (Feature Coverage):       111 passed, 0 failed.
Tier 2 (Boundary & Corner Cases):40 passed, 0 failed.
Tier 3 (Cross-Feature):          33 passed, 0 failed.
Tier 4 (Real-World Scenarios):   24 passed, 0 failed.
----------------------------------------------------------------------
Total Assertions Verified:       208
Total Passes:                    208
Total Failures:                  0
Total Execution Time:            ~4.2 seconds
======================================================================
ALL E2E TRACKING TESTS PASSED (100% SUCCESS)!
```

### Running via CTest
To run all tests including the E2E tracking suite and adversarial test suites (19 suites total):
```bash
ctest --test-dir out/build/linux-test --output-on-failure
```
Expected output:
```
100% tests passed out of 19 (Total Test time ~16.4 sec)
```

To run only the E2E tracking test target:
```bash
ctest --test-dir out/build/linux-test -R phonecast-e2e-tracking-tests --output-on-failure
```
Expected output:
```
Test project /home/tboutin/Documents/Phonecast_VR/out/build/linux-test
    Start 15: phonecast-e2e-tracking-tests
1/1 Test #15: phonecast-e2e-tracking-tests .....   Passed    ~4.2 sec

100% tests passed out of 1
```
