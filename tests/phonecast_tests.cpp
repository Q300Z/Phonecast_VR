#include "phonecast/apps/Receiver.h"
#include "phonecast/core/config/AppConfig.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/streaming/DecoderRecoveryController.h"
#include "phonecast/core/streaming/GeneratedVideoSource.h"
#include "phonecast/platform/network/TcpVideoServer.h"
#include "phonecast/vr/interaction/ControllerShortcut.h"
#include "phonecast/vr/interaction/OverlayInteractionController.h"
#include "phonecast/vr/interaction/PanelDepth.h"
#include "phonecast/vr/interaction/StartLocation.h"
#include "phonecast/vr/overlay/GlanceController.h"
#include "phonecast/vr/overlay/OverlayController.h"
#include "phonecast/vr/overlay/OverlaySettingsStore.h"
#include "phonecast/vr/overlay/SettingsMenuController.h"
#include "phonecast/vr/overlay/WristMenuGesture.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
inline int closesocket(SOCKET s) { return close(s); }
#endif

#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

class FakeLogger final : public phonecast::core::ILogger {
public:
    void Log(phonecast::core::LogLevel, std::string_view, std::string_view) override { ++entries; }
    int entries{0};
};

class FakeOverlay final : public phonecast::vr::IOverlayRenderer {
public:
    bool Start(const phonecast::vr::OverlaySettings&, std::string&) override {
        started = true;
        return startResult;
    }
    bool SubmitFrame(const phonecast::core::VideoFrame& frame, std::string&) override {
        ++frames;
        lastSequence = frame.sequence;
        return submitResult;
    }
    bool ApplySettings(const phonecast::vr::OverlaySettings&, std::string&) override {
        return true;
    }
    bool SetVisible(bool, std::string&) override { return true; }
    bool PumpEvents() override { return keepRunning; }
    void Stop() noexcept override { stopped = true; }
    bool startResult{true};
    bool submitResult{true};
    bool keepRunning{true};
    bool started{false};
    bool stopped{false};
    int frames{0};
    std::uint64_t lastSequence{0};
};

int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestConfig() {
    const auto defaults = phonecast::core::ParseCommandLine({});
    Check(defaults.status == phonecast::core::ParseStatus::Success, "default config parses");
    Check(defaults.config.framesPerSecond == 30, "default FPS is 30");

    const auto custom = phonecast::core::ParseCommandLine(
        {"--width", "800", "--height", "600", "--fps", "60", "--alpha", "0.5"});
    Check(custom.status == phonecast::core::ParseStatus::Success, "custom config parses");
    Check(custom.config.textureWidth == 800 && custom.config.textureHeight == 600,
          "custom dimensions apply");
    Check(custom.config.framesPerSecond == 60 && custom.config.overlayAlpha == 0.5F,
          "custom FPS and alpha apply");
    Check(phonecast::core::ParseCommandLine({"--fps", "0"}).status ==
              phonecast::core::ParseStatus::Error,
          "invalid FPS rejected");
    Check(phonecast::core::ParseCommandLine({"--unknown", "1"}).status ==
              phonecast::core::ParseStatus::Error,
          "unknown option rejected");
    Check(phonecast::core::ParseCommandLine({"--alpha", "nan"}).status ==
              phonecast::core::ParseStatus::Error,
          "non-finite values rejected");
}

void TestDecoderRecovery() {
    using Controller = phonecast::core::DecoderRecoveryController;
    Controller recovery;
    const auto start = Controller::TimePoint{};

    Check(!recovery.ObserveWindow(30, 1), "decoded output keeps watchdog healthy");
    Check(!recovery.ObserveWindow(0, 0), "static input does not trip watchdog");
    Check(!recovery.ObserveWindow(30, 0), "first decode stall window is tolerated");
    Check(!recovery.ObserveWindow(30, 0), "second decode stall window is tolerated");
    Check(recovery.ObserveWindow(30, 0), "third active decode stall requests recovery");

    recovery.Begin(start);
    Check(recovery.Active() && !recovery.Ready(start), "recovery starts with a short reset delay");
    const auto first = start + std::chrono::milliseconds(100);
    Check(recovery.Ready(first), "first decoder restart becomes ready");
    recovery.RecordAttempt(false, first);
    Check(recovery.Active() && recovery.Attempts() == 1,
          "failed restart schedules another bounded attempt");
    Check(!recovery.Ready(first + std::chrono::milliseconds(249)),
          "restart backoff is enforced");
    const auto second = first + std::chrono::milliseconds(250);
    Check(recovery.Ready(second), "second restart becomes ready after backoff");
    recovery.RecordAttempt(false, second);
    const auto third = second + std::chrono::milliseconds(500);
    Check(recovery.Ready(third), "third restart uses increasing backoff");
    recovery.RecordAttempt(false, third);
    Check(!recovery.Active() && recovery.Exhausted() &&
              recovery.Attempts() == Controller::MaximumAttempts(),
          "decoder restart attempts are bounded");

    recovery.Reset();
    recovery.Begin(start);
    recovery.RecordAttempt(true, first);
    Check(!recovery.Active() && !recovery.Exhausted(),
          "successful decoder restart clears recovery state");
}

void TestGeneratedFrames() {
    phonecast::core::GeneratedVideoSource source(64, 64);
    phonecast::core::VideoFrame first;
    phonecast::core::VideoFrame second;
    std::string error;
    Check(source.NextFrame(first, error) && first.IsValid(), "first generated frame is valid");
    Check(source.NextFrame(second, error) && second.IsValid(), "second generated frame is valid");
    Check(first.sequence == 0 && second.sequence == 1, "frame sequence increments");
    Check(first.pixels != second.pixels, "animation changes frame pixels");
}

void TestStreamProtocol() {
    using namespace phonecast::core::protocol;
    Message outgoing;
    outgoing.type = MessageType::VideoFrame;
    outgoing.flags = MessageFlags::KeyFrame;
    outgoing.sequence = 42;
    outgoing.timestampMicros = 123456;
    outgoing.width = 1080;
    outgoing.height = 1920;
    outgoing.payload = {0, 0, 0, 1, 0x65};
    const auto bytes = Serialize(outgoing);
    Check(bytes.size() == kHeaderSize + outgoing.payload.size(), "protocol message serializes");

    Message parsed;
    std::uint32_t payloadSize = 0;
    std::string error;
    Check(ParseHeader(bytes.data(), kHeaderSize, parsed, payloadSize, error),
          "protocol header parses");
    Check(parsed.type == MessageType::VideoFrame && parsed.sequence == 42,
          "protocol identity fields round trip");
    Check(parsed.flags == MessageFlags::KeyFrame && parsed.timestampMicros == 123456,
          "protocol timing fields round trip");
    Check(parsed.width == 1080 && parsed.height == 1920 && payloadSize == 5,
          "protocol dimensions and payload length round trip");

    auto corrupt = bytes;
    corrupt[0] = 'X';
    Check(!ParseHeader(corrupt.data(), kHeaderSize, parsed, payloadSize, error),
          "invalid protocol magic is rejected");
    corrupt = bytes;
    corrupt[8] = 0x7f;
    Check(!ParseHeader(corrupt.data(), kHeaderSize, parsed, payloadSize, error),
          "oversized payload is rejected");
    Message keyFrameRequest;
    keyFrameRequest.type = MessageType::RequestKeyFrame;
    const auto requestBytes = Serialize(keyFrameRequest);
    Check(ParseHeader(requestBytes.data(), kHeaderSize, parsed, payloadSize, error) &&
              parsed.type == MessageType::RequestKeyFrame && payloadSize == 0,
          "key-frame recovery request round trips");
    Check(IsValidPairCode("123456") && !IsValidPairCode("12345x"),
          "pair codes require six digits");

    phonecast::core::PointerEvent pointer;
    pointer.type = phonecast::core::PointerEvent::Type::Scroll;
    pointer.normalizedX = 0.25F;
    pointer.normalizedY = 0.75F;
    pointer.scrollDelta = -0.5F;
    const auto pointerBytes = SerializePointerEvent(pointer);
    phonecast::core::PointerEvent parsedPointer;
    Check(ParsePointerEvent(pointerBytes.data(), pointerBytes.size(), parsedPointer, error) &&
              parsedPointer.type == phonecast::core::PointerEvent::Type::Scroll &&
              parsedPointer.normalizedX == 0.25F && parsedPointer.normalizedY == 0.75F &&
              parsedPointer.scrollDelta == -0.5F,
          "normalized remote input round trips");
    auto invalidPointer = pointerBytes;
    invalidPointer[4] = 0x7f;
    invalidPointer[5] = 0x80;
    invalidPointer[6] = 0x00;
    invalidPointer[7] = 0x00;
    Check(!ParsePointerEvent(invalidPointer.data(), invalidPointer.size(), parsedPointer, error),
          "non-finite remote input is rejected");

    NotificationEvent notification;
    notification.applicationName = "Messages";
    notification.title = "Are you coming?";
    notification.body = "Dinner is ready";
    notification.packageName = "com.example.messages";
    notification.postedAtMillis = 123456789;
    notification.actionToken = 987654321;
    notification.contentRedacted = false;
    const auto notificationBytes = SerializeNotification(notification);
    NotificationEvent parsedNotification;
    Check(ParseNotification(notificationBytes.data(), notificationBytes.size(),
                            parsedNotification, error) &&
              parsedNotification.applicationName == notification.applicationName &&
              parsedNotification.title == notification.title &&
              parsedNotification.body == notification.body &&
              parsedNotification.packageName == notification.packageName &&
              parsedNotification.postedAtMillis == notification.postedAtMillis &&
              parsedNotification.actionToken == notification.actionToken &&
              !parsedNotification.contentRedacted,
          "privacy-filtered notification payload round trips");
    std::vector<std::uint8_t> legacyNotification(
        notificationBytes.begin(), notificationBytes.begin() + 18);
    legacyNotification[0] = 1;
    legacyNotification.insert(legacyNotification.end(),
                              notificationBytes.begin() + 26, notificationBytes.end());
    Check(ParseNotification(legacyNotification.data(), legacyNotification.size(),
                            parsedNotification, error) &&
              parsedNotification.actionToken == 0,
          "legacy notification payloads remain readable without an action");
    Message notificationOpen;
    notificationOpen.type = MessageType::NotificationOpen;
    notificationOpen.sequence = notification.actionToken;
    const auto notificationOpenBytes = Serialize(notificationOpen);
    Check(ParseHeader(notificationOpenBytes.data(), kHeaderSize, parsed, payloadSize, error) &&
              parsed.type == MessageType::NotificationOpen &&
              parsed.sequence == notification.actionToken && payloadSize == 0,
          "notification open action token round trips in the common header");

    auto malformedNotification = notificationBytes;
    malformedNotification[2] = 0x7f;
    Check(!ParseNotification(malformedNotification.data(), malformedNotification.size(),
                             parsedNotification, error),
          "malformed notification lengths are rejected");

    RemoteControlStatus remoteStatus{true, false};
    const auto remoteStatusBytes = SerializeRemoteControlStatus(remoteStatus);
    RemoteControlStatus parsedRemoteStatus;
    Check(ParseRemoteControlStatus(remoteStatusBytes.data(), remoteStatusBytes.size(),
                                   parsedRemoteStatus, error) &&
              parsedRemoteStatus.appEnabled &&
              !parsedRemoteStatus.accessibilityEnabled &&
              !parsedRemoteStatus.Ready(),
          "remote-control consent gates round trip independently");
    Check(!ParseRemoteControlStatus(remoteStatusBytes.data(), 1, parsedRemoteStatus, error),
          "malformed remote-control gate status is rejected");
}

void TestOverlayInteraction() {
    const auto start = phonecast::vr::StartLocation(0.35F, 0.1F, 0.85F);
    const float length = std::sqrt(0.35F*0.35F + 0.1F*0.1F + 0.85F*0.85F);
    Check(start[3] == 0.35F && start[7] == 0.1F && start[11] == -0.85F &&
          std::fabs(start[2]*(-0.35F) + start[6]*(-0.1F) + start[10]*0.85F - length) < 0.001F,
          "start location retains position and its normal faces the snapshot user");
    Check(phonecast::vr::AdjustPanelDepth(1.0F, 1.0F, 0.04F) > 1.0F &&
          phonecast::vr::AdjustPanelDepth(1.0F, -1.0F, 0.04F) < 1.0F &&
          phonecast::vr::AdjustPanelDepth(1.0F, 0.1F, 0.04F) == 1.0F &&
          phonecast::vr::AdjustPanelDepth(3.0F, 1.0F, 0.04F) == 3.0F &&
          phonecast::vr::AdjustPanelDepth(0.20F, -1.0F, 0.04F) == 0.20F,
          "move-handle stick depth has correct direction, deadzone, and bounds");
    phonecast::vr::PanelDepthScroll scroll;
    scroll.Add(0.1F, true, true);
    Check(std::fabs(scroll.TakeDistance(1.0F) - 1.005F) < 0.0001F,
          "smooth scroll increments have no axis deadzone and positive means farther");
    Check(scroll.TakeDistance(1.0F) == 1.0F, "scroll increments are consumed once");
    scroll.Add(-1.0F, true, true);
    Check(std::fabs(scroll.TakeDistance(1.0F) - 0.97F) < 0.0001F,
          "negative scroll means nearer with bounded per-pump displacement");
    scroll.Add(1.0F, false, true);
    scroll.Add(1.0F, true, false);
    scroll.Add(std::numeric_limits<float>::infinity(), true, true);
    Check(scroll.TakeDistance(1.0F) == 1.0F && scroll.discreteEvents == 1 && scroll.smoothEvents == 2,
          "duplicate discrete events, other-hand input, and nonfinite deltas cannot move a drag");
    for (int i = 0; i < 100; ++i) scroll.Add(1.0F, true, true);
    Check(std::fabs(scroll.TakeDistance(1.0F) - 1.03F) < 0.0001F,
          "bursty scroll cannot create an uncontrolled depth jump");
    scroll.Add(1.0F, true, true);
    Check(scroll.TakeDistance(3.0F) == 3.0F, "scroll depth clamps far bound");
    scroll.Add(-1.0F, true, true);
    Check(scroll.TakeDistance(0.20F) == 0.20F, "scroll depth clamps near bound");
    scroll.Add(1.0F, true, true);
    scroll = {};
    Check(scroll.TakeDistance(1.0F) == 1.0F && scroll.smoothEvents == 0,
          "drag cancellation clears pending depth and diagnostic counts");
    phonecast::vr::OverlayInteractionController interaction;
    interaction.SetSurfaceSize(1000, 2000);
    const auto down = interaction.PointerDown(250.0F, 500.0F);
    const auto move = interaction.PointerMove(750.0F, 1500.0F);
    const auto back = interaction.Back();
    Check(down.type == phonecast::core::PointerEvent::Type::Down &&
              down.normalizedX == 0.25F && down.normalizedY == 0.75F,
          "OpenVR coordinates map to normalized top-left phone coordinates");
    Check(move.sequence == down.sequence + 1 && move.normalizedX == 0.75F &&
              move.normalizedY == 0.25F,
          "pointer motion retains ordering and coordinate orientation");
    Check(back.type == phonecast::core::PointerEvent::Type::Back &&
              back.sequence == move.sequence + 1,
          "Android Back is represented independently of renderer APIs");

    interaction.SetSurfaceSize(1000, 2000, 100);
    const auto top = interaction.PointerDown(250.0F, 2100.0F);
    const auto bottom = interaction.PointerUp(250.0F, 100.0F);
    Check(interaction.IsGrabHandle(50.0F) && !interaction.IsGrabHandle(100.0F),
          "the bottom inset is reserved for the overlay grab handle");
    Check(interaction.IsBackButton(50.0F, 50.0F) &&
              !interaction.IsBackButton(100.0F, 50.0F) &&
              !interaction.IsBackButton(50.0F, 100.0F),
          "the lower-left handle corner is reserved for Android Back");
    Check(interaction.IsCloseButton(850.0F, 50.0F) &&
              !interaction.IsCloseButton(50.0F, 50.0F) &&
              !interaction.IsCloseButton(850.0F, 100.0F),
          "Close is separated from Back and placed beside the resize corner");
    Check(interaction.IsResizeButton(950.0F, 50.0F) &&
              !interaction.IsResizeButton(899.0F, 50.0F) &&
              !interaction.IsResizeButton(950.0F, 100.0F),
          "the lower-right handle corner is reserved for resizing the phone");
    Check(top.normalizedY == 0.0F && bottom.normalizedY == 1.0F,
          "the grab handle inset does not offset phone touch coordinates");
}

void TestOverlayControls() {
    phonecast::vr::OverlaySettings initial;
    initial.widthMeters = 0.65F;
    initial.distanceMeters = 1.0F;
    phonecast::vr::OverlayController controls(initial);
    controls.Apply(phonecast::vr::OverlayAction::MoveRight);
    controls.Apply(phonecast::vr::OverlayAction::MoveUp);
    controls.Apply(phonecast::vr::OverlayAction::ScaleUp);
    Check(controls.Settings().offsetXMeters > 0.0F &&
              controls.Settings().offsetYMeters > 0.0F,
          "overlay controls move the transform");
    Check(controls.Settings().widthMeters > initial.widthMeters,
          "overlay controls change scale");
    controls.Apply(phonecast::vr::OverlayAction::ToggleVisibility);
    Check(!controls.Visible(), "overlay visibility toggles");
    controls.Apply(phonecast::vr::OverlayAction::Reset);
    Check(controls.Visible() && controls.Settings().offsetXMeters == 0.0F &&
              controls.Settings().widthMeters == initial.widthMeters,
          "overlay reset restores initial state");
    for (int count = 0; count < 100; ++count) {
        controls.Apply(phonecast::vr::OverlayAction::OpacityDown);
        controls.Apply(phonecast::vr::OverlayAction::DistanceNearer);
    }
    Check(controls.Settings().alpha == 0.0F && controls.Settings().distanceMeters == 0.2F,
          "overlay controls clamp safe ranges");

    controls.Apply(phonecast::vr::OverlayAction::WorldLocked);
    Check(controls.Settings().placementMode == phonecast::vr::PlacementMode::WorldLocked &&
              !controls.Settings().worldTransformValid,
          "world mode requests a fresh anchor");
    auto anchored = controls.Settings();
    anchored.worldTransformValid = true;
    controls.ReplaceSettings(anchored);
    const float originalWorldX = controls.Settings().worldTransform[3];
    controls.Apply(phonecast::vr::OverlayAction::MoveRight);
    Check(controls.Settings().worldTransform[3] > originalWorldX,
          "world controls move the absolute anchor");
    controls.Apply(phonecast::vr::OverlayAction::LeftControllerLocked);
    Check(controls.Settings().placementMode == phonecast::vr::PlacementMode::LeftControllerLocked,
          "left controller mode is selectable");
    controls.Apply(phonecast::vr::OverlayAction::MoveRight);
    controls.Apply(phonecast::vr::OverlayAction::MoveUp);
    controls.Apply(phonecast::vr::OverlayAction::DistanceFarther);
    controls.Apply(phonecast::vr::OverlayAction::ScaleDown);
    controls.Apply(phonecast::vr::OverlayAction::ControllerTiltUp);
    controls.Apply(phonecast::vr::OverlayAction::ControllerYawRight);
    controls.Apply(phonecast::vr::OverlayAction::ControllerOrientationNext);
    Check(controls.Settings().leftController.lateralMeters > 0.0F &&
              controls.Settings().leftController.heightMeters > 0.10F &&
              controls.Settings().leftController.distanceMeters > 0.18F &&
              controls.Settings().leftController.scale < 1.0F &&
              controls.Settings().leftController.tiltDegrees > 0.0F &&
              controls.Settings().leftController.yawDegrees > 0.0F &&
              controls.Settings().leftController.orientation ==
                  phonecast::vr::ControllerOrientation::WorldUpright,
          "active hand calibration is independently adjustable");
    Check(controls.Settings().rightController.lateralMeters == 0.0F,
          "inactive hand calibration is unchanged");
    controls.Apply(phonecast::vr::OverlayAction::ResetControllerCalibration);
    Check(controls.Settings().leftController.lateralMeters == 0.0F &&
              controls.Settings().leftController.scale == 1.0F,
          "active hand calibration resets");
    controls.Apply(phonecast::vr::OverlayAction::PhoneTracked);
    Check(controls.Settings().placementMode == phonecast::vr::PlacementMode::PhoneTracked,
          "phone-tracked mode is selectable via OverlayAction");
}

void TestControllerShortcut() {
    using phonecast::vr::ControllerDockShortcutAction;
    using phonecast::vr::GlanceInput;
    using phonecast::vr::PlacementMode;
    using phonecast::vr::RadialMenuAction;

    Check(ControllerDockShortcutAction(GlanceInput::LeftController, false,
              PlacementMode::LeftControllerLocked) == RadialMenuAction::LeftControllerLocked,
          "a hidden phone is reopened on the requested controller");
    Check(ControllerDockShortcutAction(GlanceInput::LeftController, true,
              PlacementMode::RightControllerLocked) == RadialMenuAction::LeftControllerLocked,
          "a different controller receives the visible phone");
    Check(ControllerDockShortcutAction(GlanceInput::LeftController, true,
              PlacementMode::LeftControllerLocked) == RadialMenuAction::PinCurrentPosition,
          "repeating the left dock shortcut pins at the current position");
    Check(ControllerDockShortcutAction(GlanceInput::RightController, true,
              PlacementMode::RightControllerLocked) == RadialMenuAction::PinCurrentPosition,
          "repeating the right dock shortcut pins at the current position");
}

void TestGlanceMode() {
    phonecast::vr::OverlaySettings base;
    base.placementMode = phonecast::vr::PlacementMode::HeadLocked;
    base.widthMeters = 0.8F;
    phonecast::vr::GlanceController glance;
    Check(glance.State() == phonecast::vr::GlanceState::Hidden && !glance.Visible(),
          "glance mode starts hidden");
    glance.SetHand(phonecast::vr::GlanceHand::Right);
    Check(glance.Hand() == phonecast::vr::GlanceHand::Right,
          "keyboard fallback can select the glance hand");
    glance.Cycle(glance.Hand());
    auto presented = glance.PresentationSettings(base);
    Check(glance.State() == phonecast::vr::GlanceState::Glance && glance.Visible() &&
              presented.placementMode == phonecast::vr::PlacementMode::RightControllerLocked &&
              presented.widthMeters < base.widthMeters,
          "glance preview uses the initiating hand and reduced size");
    glance.Cycle(phonecast::vr::GlanceHand::Right);
    presented = glance.PresentationSettings(base);
    Check(glance.State() == phonecast::vr::GlanceState::Expanded &&
              presented.placementMode == base.placementMode &&
              presented.widthMeters == base.widthMeters,
          "expanded view restores normal placement and size");
    glance.Cycle(phonecast::vr::GlanceHand::Right);
    Check(glance.State() == phonecast::vr::GlanceState::Pinned && glance.Visible(),
          "expanded view can be pinned");
    glance.Cycle(phonecast::vr::GlanceHand::Right);
    Check(glance.State() == phonecast::vr::GlanceState::Hidden && !glance.Visible(),
          "pinned view cycles back to hidden");
    glance.ToggleExpanded();
    Check(glance.State() == phonecast::vr::GlanceState::Expanded,
          "quick toggle opens the expanded view");
    glance.ToggleExpanded();
    Check(!glance.Visible(), "quick toggle hides a visible view");
    glance.ShowGlance(phonecast::vr::GlanceHand::Left);
    presented = glance.PresentationSettings(base);
    Check(glance.State() == phonecast::vr::GlanceState::Glance &&
              presented.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked,
          "radial menu can request a left-hand glance preview");
    glance.ShowPinned();
    Check(glance.State() == phonecast::vr::GlanceState::Pinned && glance.Visible(),
          "radial menu can pin the normal presentation");
}

void TestOverlaySettingsPersistence() {
    const auto path = std::filesystem::temp_directory_path() / "phonecast-overlay-settings-test.ini";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    phonecast::vr::OverlaySettingsStore store(path);
    phonecast::vr::OverlaySettings saved;
    saved.placementMode = phonecast::vr::PlacementMode::WorldLocked;
    saved.worldTransformValid = true;
    saved.worldTransform[3] = 2.25F;
    saved.widthMeters = 0.9F;
    saved.leftController.distanceMeters = 0.42F;
    saved.leftController.heightMeters = -0.12F;
    saved.leftController.lateralMeters = 0.08F;
    saved.leftController.tiltDegrees = 25.0F;
    saved.leftController.yawDegrees = -15.0F;
    saved.leftController.scale = 1.4F;
    saved.leftController.orientation = phonecast::vr::ControllerOrientation::ControllerRelative;
    saved.rightController.orientation = phonecast::vr::ControllerOrientation::Wrist;
    saved.glancePreviewScale = 0.7F;
    saved.radialLongPressMilliseconds = 850;
    saved.notificationWidthMeters = 0.75F;
    saved.notificationDistanceMeters = 0.65F;
    saved.notificationOffsetXMeters = -0.15F;
    saved.notificationOffsetYMeters = 0.05F;
    saved.startDistanceMeters = 1.1F;
    saved.startOffsetXMeters = 0.22F;
    saved.startOffsetYMeters = -0.12F;
    std::string error;
    Check(store.Save(saved, error), "overlay settings save");
    phonecast::vr::OverlaySettings loaded;
    bool found = false;
    Check(store.Load(loaded, found, error) && found, "overlay settings load");
    Check(loaded.placementMode == phonecast::vr::PlacementMode::WorldLocked &&
              loaded.worldTransformValid && loaded.worldTransform[3] == 2.25F &&
              loaded.widthMeters == 0.9F &&
              loaded.leftController.distanceMeters == 0.42F &&
              loaded.leftController.heightMeters == -0.12F &&
              loaded.leftController.lateralMeters == 0.08F &&
              loaded.leftController.tiltDegrees == 25.0F &&
              loaded.leftController.yawDegrees == -15.0F &&
              loaded.leftController.scale == 1.4F &&
              loaded.leftController.orientation == phonecast::vr::ControllerOrientation::ControllerRelative &&
              loaded.rightController.orientation == phonecast::vr::ControllerOrientation::Wrist &&
              loaded.glancePreviewScale == 0.7F &&
              loaded.radialLongPressMilliseconds == 850 &&
              loaded.notificationWidthMeters == 0.75F &&
              loaded.notificationDistanceMeters == 0.65F &&
              loaded.notificationOffsetXMeters == -0.15F &&
              loaded.notificationOffsetYMeters == 0.05F &&
              loaded.startDistanceMeters == 1.1F &&
              loaded.startOffsetXMeters == 0.22F && loaded.startOffsetYMeters == -0.12F,
          "overlay settings round trip");
    std::ifstream source(path);
    std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    source.close();
    text.replace(text.find("version=5"), 9, "version=4");
    { std::ofstream output(path); output << text; }
    Check(store.Load(loaded, found, error) && found && loaded.startOffsetXMeters == 0.35F &&
          loaded.startDistanceMeters == 0.85F,
          "version four migrates with safer start-location defaults");
    Check(store.Save(saved, error), "restore version five settings");
    std::ifstream current(path);
    text.assign(std::istreambuf_iterator<char>(current), std::istreambuf_iterator<char>());
    current.close();
    const auto key = text.find("start_distance=");
    text.replace(key, text.find('\n', key) - key, "start_distance=nan");
    { std::ofstream output(path); output << text; }
    const float before = loaded.startDistanceMeters;
    Check(!store.Load(loaded, found, error) && loaded.startDistanceMeters == before,
          "nonfinite start location is rejected without replacing live settings");

    saved.placementMode = phonecast::vr::PlacementMode::PhoneTracked;
    Check(store.Save(saved, error), "save settings with phone-tracked mode");
    Check(store.Load(loaded, found, error) && found &&
          loaded.placementMode == phonecast::vr::PlacementMode::PhoneTracked,
          "load settings with phone-tracked mode");

    // Verify phone screen dimensions serialize and deserialize cleanly
    const float testPhoneWidths[] = {0.072F, 0.057F, 0.04F};
    for (float phoneWidth : testPhoneWidths) {
        saved.widthMeters = phoneWidth;
        Check(store.Save(saved, error), "save settings with phone screen width");
        Check(store.Load(loaded, found, error) && found &&
              std::abs(loaded.widthMeters - phoneWidth) < 1e-4F,
              "load settings cleanly deserializes phone screen width");
    }

    // Verify values below 0.04m or above 5.0m are correctly rejected
    std::ifstream validStream(path);
    std::string settingsText((std::istreambuf_iterator<char>(validStream)),
                             std::istreambuf_iterator<char>());
    validStream.close();

    const auto widthKey = settingsText.find("width=");
    Check(widthKey != std::string::npos, "width key found in settings file");

    std::string textBelow = settingsText;
    textBelow.replace(widthKey, textBelow.find('\n', widthKey) - widthKey, "width=0.039");
    { std::ofstream out(path); out << textBelow; }
    Check(!store.Load(loaded, found, error) && error == "Overlay settings contain an invalid value.",
          "width below 0.04m is rejected with invalid value error");

    std::string textZero = settingsText;
    textZero.replace(widthKey, textZero.find('\n', widthKey) - widthKey, "width=0.0");
    { std::ofstream out(path); out << textZero; }
    Check(!store.Load(loaded, found, error) && error == "Overlay settings contain an invalid value.",
          "width of 0.0m is rejected");

    std::string textAbove = settingsText;
    textAbove.replace(widthKey, textAbove.find('\n', widthKey) - widthKey, "width=5.01");
    { std::ofstream out(path); out << textAbove; }
    Check(!store.Load(loaded, found, error) && error == "Overlay settings contain an invalid value.",
          "width above 5.0m is rejected with invalid value error");

    std::filesystem::remove(path, ignored);
}

void TestSettingsMenu() {
    phonecast::vr::OverlaySettings initial;
    initial.widthMeters = 0.65F;
    phonecast::vr::SettingsMenuController menu;
    menu.Open(initial);
    Check(menu.IsOpen() && menu.View().title == "PHONECAST SETTINGS",
          "settings menu opens at its root");
    auto rendererUpdate = initial;
    rendererUpdate.widthMeters = 0.80F;
    rendererUpdate.worldTransformValid = true;
    rendererUpdate.worldTransform[3] = 1.25F;
    menu.MergeRendererUpdate(rendererUpdate);
    Check(menu.Draft().widthMeters == 0.80F && menu.Draft().worldTransformValid &&
              menu.Draft().worldTransform[3] == 1.25F &&
              menu.Original().widthMeters == initial.widthMeters &&
              !menu.Original().worldTransformValid,
          "renderer resize and anchors merge into the draft without changing cancel state");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(menu.View().title == "APPEARANCE", "settings menu opens Appearance");
    Check(menu.Handle(phonecast::vr::SettingsMenuCommand::Increase) ==
              phonecast::vr::SettingsMenuResult::Updated &&
              menu.Draft().widthMeters > initial.widthMeters,
          "settings changes update the transactional draft");
    Check(menu.View().normalizedValues[0] >= 0.0F &&
              menu.Handle({phonecast::vr::SettingsMenuCommand::SetNormalized, 1.0F}) ==
                  phonecast::vr::SettingsMenuResult::Updated &&
              menu.Draft().widthMeters == 2.0F,
          "settings sliders expose and apply normalized values");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Back);
    for (int index = 0; index < 9; ++index)
        menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    Check(menu.Handle(phonecast::vr::SettingsMenuCommand::Activate) ==
              phonecast::vr::SettingsMenuResult::Applied && !menu.IsOpen(),
          "settings can be applied explicitly");

    menu.Open(initial);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    for (int i = 0; i < 4; ++i) menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(menu.View().showStartLocationPreview && menu.View().title == "START LOCATION",
          "Placement Start Location requests a separate dummy preview");
    menu.Handle({phonecast::vr::SettingsMenuCommand::SetNormalized, 1.0F});
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle({phonecast::vr::SettingsMenuCommand::SetNormalized, 0.0F});
    Check(menu.Draft().startDistanceMeters == 2.5F && menu.Draft().startOffsetXMeters == -1.0F &&
          menu.Original().startDistanceMeters == initial.startDistanceMeters,
          "start-location sliders are bounded and preserve the cancel snapshot");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Back);
    Check(!menu.View().showStartLocationPreview, "leaving Start Location removes its dummy preview");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Back);
    Check(!menu.IsOpen(), "start location changes can be cancelled");

    menu.Open(initial);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(menu.View().title == "GLANCE AND CONTROLS", "Glance settings are reachable");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Increase);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Increase);
    Check(menu.Draft().glancePreviewScale > initial.glancePreviewScale &&
              menu.Draft().radialLongPressMilliseconds > initial.radialLongPressMilliseconds,
          "Glance preview and long-press settings are editable");
    Check(menu.Handle(phonecast::vr::SettingsMenuCommand::Back) ==
              phonecast::vr::SettingsMenuResult::None,
          "Back returns from a settings page");
    for (int index = 0; index < 5; ++index)
        menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(menu.View().title == "NOTIFICATIONS" &&
              menu.View().showNotificationPreview,
          "Notification placement settings request a live card preview");
    menu.Handle(phonecast::vr::SettingsMenuCommand::Increase);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Decrease);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Decrease);
    menu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    menu.Handle(phonecast::vr::SettingsMenuCommand::Decrease);
    Check(menu.Draft().notificationWidthMeters > initial.notificationWidthMeters &&
              menu.Draft().notificationDistanceMeters < initial.notificationDistanceMeters &&
              menu.Draft().notificationOffsetXMeters < initial.notificationOffsetXMeters &&
              menu.Draft().notificationOffsetYMeters < initial.notificationOffsetYMeters,
          "notification size and head-relative placement are editable");
    Check(menu.Handle(phonecast::vr::SettingsMenuCommand::Back) ==
              phonecast::vr::SettingsMenuResult::None,
          "Back returns from notification settings");
    Check(menu.Handle(phonecast::vr::SettingsMenuCommand::Back) ==
              phonecast::vr::SettingsMenuResult::Cancelled && !menu.IsOpen() &&
              menu.Original().widthMeters == initial.widthMeters,
          "Back from the root cancels the transaction");

    // Test 5-way PlacementMode cycling including PhoneTracked
    phonecast::vr::SettingsMenuController modeMenu;
    modeMenu.Open(initial);
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::NextItem);
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Activate);
    Check(modeMenu.View().title == "PLACEMENT", "enters Placement page");
    Check(modeMenu.View().values[0] == "HEAD", "starts at HEAD");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Increase); // 1: World
    Check(modeMenu.View().values[0] == "WORLD", "cycles to WORLD");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Increase); // 2: Left Controller
    Check(modeMenu.View().values[0] == "LEFT CONTROLLER", "cycles to LEFT CONTROLLER");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Increase); // 3: Right Controller
    Check(modeMenu.View().values[0] == "RIGHT CONTROLLER", "cycles to RIGHT CONTROLLER");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Increase); // 4: PhoneTracked
    Check(modeMenu.Draft().placementMode == phonecast::vr::PlacementMode::PhoneTracked,
          "cycles forward to PhoneTracked");
    Check(modeMenu.View().values[0] == "PHONE TRACKED", "displays PHONE TRACKED");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Increase); // 0: Wrap forward to Head
    Check(modeMenu.Draft().placementMode == phonecast::vr::PlacementMode::HeadLocked,
          "cycles forward back to HeadLocked");
    Check(modeMenu.View().values[0] == "HEAD", "displays HEAD after wrap");
    modeMenu.Handle(phonecast::vr::SettingsMenuCommand::Decrease); // 4: Wrap backward to PhoneTracked
    Check(modeMenu.Draft().placementMode == phonecast::vr::PlacementMode::PhoneTracked,
          "backward wrap reaches PhoneTracked");
    Check(modeMenu.View().values[0] == "PHONE TRACKED", "displays PHONE TRACKED on backward wrap");
}

void TestWristMenuGesture() {
    using phonecast::vr::WristMenuGestureAction;
    phonecast::vr::WristMenuGesture gesture;
    phonecast::vr::WristMenuGestureObservation observation;

    observation.nowMilliseconds = 100;
    Check(gesture.Update(observation).action == WristMenuGestureAction::None,
          "neutral wrists arm the gesture without opening");
    observation.leftFacing = true;
    observation.nowMilliseconds = 200;
    Check(gesture.Update(observation).action == WristMenuGestureAction::None,
          "wrist gesture waits for its opening dwell");
    observation.nowMilliseconds = 1700;
    const auto halfway = gesture.Update(observation);
    Check(halfway.action == WristMenuGestureAction::None &&
              halfway.progress > 0.45F && halfway.progress < 0.55F,
          "wrist gesture reports visible hold progress");
    observation.nowMilliseconds = 3200;
    Check(gesture.Update(observation).action == WristMenuGestureAction::OpenLeft,
          "stable three-second left wrist flip opens the left menu");

    observation.menuVisible = true;
    observation.leftFacing = false;
    observation.nowMilliseconds = 1200;
    Check(gesture.Update(observation).action == WristMenuGestureAction::None,
          "open menu remains available after the wrist moves away");

    gesture.Reset();
    observation = {};
    observation.nowMilliseconds = 100;
    gesture.Update(observation);
    observation.rightFacing = true;
    observation.nowMilliseconds = 200;
    gesture.Update(observation);
    observation.nowMilliseconds = 3200;
    Check(gesture.Update(observation).action == WristMenuGestureAction::OpenRight,
          "stable three-second right wrist flip opens the right menu");
}

void TestReceiverLifecycle() {
    phonecast::core::GeneratedVideoSource source(64, 64);
    FakeOverlay overlay;
    FakeLogger logger;
    phonecast::apps::Receiver receiver(source, overlay, logger);
    std::string error;
    Check(receiver.Start({}, error), "receiver starts");
    Check(receiver.Tick(error), "receiver submits a frame");
    Check(overlay.frames == 1 && receiver.SubmittedFrames() == 1, "submission is counted");
    receiver.Stop();
    receiver.Stop();
    Check(overlay.stopped, "renderer stops safely");

    FakeOverlay quittingOverlay;
    phonecast::apps::Receiver quittingReceiver(source, quittingOverlay, logger);
    Check(quittingReceiver.Start({}, error), "second receiver starts");
    quittingOverlay.keepRunning = false;
    error = "stale";
    Check(!quittingReceiver.Tick(error) && error.empty(), "runtime shutdown is clean");
    quittingReceiver.Stop();
}

void TestTrackingProtocol() {
    using namespace phonecast::core::protocol;
    std::string error;

    // 1. ScreenMetrics round-trip
    StreamScreenMetrics metrics;
    metrics.version = 1;
    metrics.flags = 1;
    metrics.densityDpi = 420;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    metrics.physicalWidthMm = 68.5F;
    metrics.physicalHeightMm = 152.2F;
    metrics.xdpi = 400.2F;
    metrics.ydpi = 400.5F;

    const auto metricsBytes = SerializeScreenMetrics(metrics);
    Check(metricsBytes.size() == 32, "screen metrics serializes to 32 bytes");
    Check(metricsBytes[0] == 1 && metricsBytes[1] == 1, "screen metrics header fields");
    Check(metricsBytes[28] == 0 && metricsBytes[29] == 0 &&
          metricsBytes[30] == 0 && metricsBytes[31] == 0,
          "screen metrics trailing reserved bytes are zero");

    StreamScreenMetrics parsedMetrics;
    Check(ParseScreenMetrics(metricsBytes.data(), metricsBytes.size(), parsedMetrics, error),
          "screen metrics parses valid payload");
    Check(parsedMetrics.version == 1 && parsedMetrics.flags == 1 &&
          parsedMetrics.densityDpi == 420 &&
          parsedMetrics.pixelWidth == 1080 && parsedMetrics.pixelHeight == 2400 &&
          std::abs(parsedMetrics.physicalWidthMm - 68.5F) < 0.001F &&
          std::abs(parsedMetrics.physicalHeightMm - 152.2F) < 0.001F &&
          std::abs(parsedMetrics.xdpi - 400.2F) < 0.001F &&
          std::abs(parsedMetrics.ydpi - 400.5F) < 0.001F,
          "screen metrics fields round trip correctly");

    // Rejection checks for ScreenMetrics
    Check(!ParseScreenMetrics(metricsBytes.data(), 31, parsedMetrics, error),
          "screen metrics rejects payload smaller than 32 bytes");
    Check(!ParseScreenMetrics(metricsBytes.data(), 33, parsedMetrics, error),
          "screen metrics rejects payload larger than 32 bytes");
    Check(!ParseScreenMetrics(nullptr, 32, parsedMetrics, error),
          "screen metrics rejects null data");

    auto badMetricsBytes = metricsBytes;
    badMetricsBytes[0] = 2;
    Check(!ParseScreenMetrics(badMetricsBytes.data(), badMetricsBytes.size(), parsedMetrics, error),
          "screen metrics rejects unsupported version");

    badMetricsBytes = metricsBytes;
    badMetricsBytes[1] = 0x02;
    Check(!ParseScreenMetrics(badMetricsBytes.data(), badMetricsBytes.size(), parsedMetrics, error),
          "screen metrics rejects reserved flags");

    badMetricsBytes = metricsBytes;
    badMetricsBytes[28] = 1;
    Check(!ParseScreenMetrics(badMetricsBytes.data(), badMetricsBytes.size(), parsedMetrics, error),
          "screen metrics rejects non-zero reserved byte");

    StreamScreenMetrics invalidMetrics = metrics;
    invalidMetrics.pixelWidth = 0;
    Check(SerializeScreenMetrics(invalidMetrics).empty(),
          "screen metrics serializer rejects zero width");
    invalidMetrics = metrics;
    invalidMetrics.pixelHeight = 20000;
    Check(SerializeScreenMetrics(invalidMetrics).empty(),
          "screen metrics serializer rejects excessive height");
    invalidMetrics = metrics;
    invalidMetrics.densityDpi = 0;
    Check(SerializeScreenMetrics(invalidMetrics).empty(),
          "screen metrics serializer rejects zero density DPI");
    invalidMetrics = metrics;
    invalidMetrics.physicalWidthMm = std::numeric_limits<float>::quiet_NaN();
    Check(SerializeScreenMetrics(invalidMetrics).empty(),
          "screen metrics serializer rejects non-finite physical width");

    // 2. PhoneImu round-trip
    StreamPhoneImu imu;
    imu.version = 1;
    imu.sensorType = 1;
    imu.accuracy = 3;
    imu.flags = 0;
    imu.qx = 0.0F;
    imu.qy = 0.0F;
    imu.qz = 0.0F;
    imu.qw = 1.0F;
    imu.gyroX = 0.1F;
    imu.gyroY = 0.2F;
    imu.gyroZ = 0.3F;
    imu.accelX = 0.4F;
    imu.accelY = 0.5F;
    imu.accelZ = 0.6F;

    const auto imuBytes = SerializePhoneImu(imu);
    Check(imuBytes.size() == 48, "phone IMU serializes to 48 bytes");
    Check(imuBytes[0] == 1 && imuBytes[1] == 1 && imuBytes[2] == 3 && imuBytes[3] == 0,
          "phone IMU header bytes match");
    Check(imuBytes[44] == 0 && imuBytes[45] == 0 && imuBytes[46] == 0 && imuBytes[47] == 0,
          "phone IMU trailing reserved bytes are zero");

    StreamPhoneImu parsedImu;
    Check(ParsePhoneImu(imuBytes.data(), imuBytes.size(), parsedImu, error),
          "phone IMU parses valid payload");
    Check(parsedImu.version == 1 && parsedImu.sensorType == 1 &&
          parsedImu.accuracy == 3 && parsedImu.flags == 0 &&
          std::abs(parsedImu.qx - 0.0F) < 0.0001F &&
          std::abs(parsedImu.qy - 0.0F) < 0.0001F &&
          std::abs(parsedImu.qz - 0.0F) < 0.0001F &&
          std::abs(parsedImu.qw - 1.0F) < 0.0001F &&
          std::abs(parsedImu.gyroX - 0.1F) < 0.0001F &&
          std::abs(parsedImu.gyroY - 0.2F) < 0.0001F &&
          std::abs(parsedImu.gyroZ - 0.3F) < 0.0001F &&
          std::abs(parsedImu.accelX - 0.4F) < 0.0001F &&
          std::abs(parsedImu.accelY - 0.5F) < 0.0001F &&
          std::abs(parsedImu.accelZ - 0.6F) < 0.0001F,
          "phone IMU fields round trip correctly");

    // Rejection checks for PhoneImu
    Check(!ParsePhoneImu(imuBytes.data(), 47, parsedImu, error),
          "phone IMU rejects payload smaller than 48 bytes");
    Check(!ParsePhoneImu(imuBytes.data(), 49, parsedImu, error),
          "phone IMU rejects payload larger than 48 bytes");
    Check(!ParsePhoneImu(nullptr, 48, parsedImu, error),
          "phone IMU rejects null data");

    auto badImuBytes = imuBytes;
    badImuBytes[0] = 2;
    Check(!ParsePhoneImu(badImuBytes.data(), badImuBytes.size(), parsedImu, error),
          "phone IMU rejects unsupported version");

    badImuBytes = imuBytes;
    badImuBytes[1] = 0;
    Check(!ParsePhoneImu(badImuBytes.data(), badImuBytes.size(), parsedImu, error),
          "phone IMU rejects unsupported sensor type");

    badImuBytes = imuBytes;
    badImuBytes[2] = 4;
    Check(!ParsePhoneImu(badImuBytes.data(), badImuBytes.size(), parsedImu, error),
          "phone IMU rejects invalid accuracy level");

    badImuBytes = imuBytes;
    badImuBytes[3] = 0x04;
    Check(!ParsePhoneImu(badImuBytes.data(), badImuBytes.size(), parsedImu, error),
          "phone IMU rejects reserved flags");

    badImuBytes = imuBytes;
    badImuBytes[44] = 1;
    Check(!ParsePhoneImu(badImuBytes.data(), badImuBytes.size(), parsedImu, error),
          "phone IMU rejects non-zero reserved byte");

    StreamPhoneImu unnormImu = imu;
    unnormImu.qw = 0.0F;
    Check(SerializePhoneImu(unnormImu).empty(),
          "phone IMU serializer rejects unnormalized quaternion");

    StreamPhoneImu extremeGyroImu = imu;
    extremeGyroImu.gyroX = 55.0F;
    Check(SerializePhoneImu(extremeGyroImu).empty(),
          "phone IMU serializer rejects excessive gyro rate");

    StreamPhoneImu extremeAccelImu = imu;
    extremeAccelImu.accelZ = 250.0F;
    Check(SerializePhoneImu(extremeAccelImu).empty(),
          "phone IMU serializer rejects excessive acceleration");

    // 3. Tracking Capability checks
    const auto cap = TrackingCapability();
    Check(cap.size() == 8, "tracking capability token is 8 bytes");
    Check(cap[0] == 'P' && cap[1] == 'C' && cap[2] == 'T' && cap[3] == 'R' &&
          cap[4] == 1 && cap[5] == 1 && cap[6] == 0 && cap[7] == 0,
          "tracking capability token magic and version match");
    Check(HasTrackingCapability(cap), "HasTrackingCapability recognizes tracking token");
    Check(!HasTrackingCapability({}), "HasTrackingCapability rejects empty payload");
    Check(!HasTrackingCapability({'P', 'C', 'T', 'R'}), "HasTrackingCapability rejects short payload");
    Check(!HasTrackingCapability({'P', 'C', 'A', 'C', 1, 1, 0, 0}), "HasTrackingCapability rejects other tokens");

    std::vector<std::uint8_t> compound = {'P', 'C', 'A', 'C', 1, 1, 0, 0,
                                          'P', 'C', 'T', 'R', 1, 1, 0, 0};
    Check(HasTrackingCapability(compound), "HasTrackingCapability recognizes token in compound payload");

    Check(static_cast<std::uint8_t>(MessageType::ScreenMetrics) == 15, "ScreenMetrics wire type is 15");
    Check(static_cast<std::uint8_t>(MessageType::PhoneImu) == 16, "PhoneImu wire type is 16");

    // Check ParseHeader bounds enforcement
    Message headerMsg;
    std::uint32_t payloadSize = 0;
    Message metricsWireMsg;
    metricsWireMsg.type = MessageType::ScreenMetrics;
    metricsWireMsg.payload = metricsBytes;
    const auto metricsSerialized = Serialize(metricsWireMsg);
    Check(ParseHeader(metricsSerialized.data(), kHeaderSize, headerMsg, payloadSize, error) &&
          payloadSize == 32 && headerMsg.type == MessageType::ScreenMetrics,
          "ParseHeader accepts 32-byte ScreenMetrics payload");

    auto badHeader = metricsSerialized;
    badHeader[8] = 0; badHeader[9] = 0; badHeader[10] = 0; badHeader[11] = 31;
    Check(!ParseHeader(badHeader.data(), kHeaderSize, headerMsg, payloadSize, error),
          "ParseHeader rejects non-32-byte ScreenMetrics payload");

    Message imuWireMsg;
    imuWireMsg.type = MessageType::PhoneImu;
    imuWireMsg.payload = imuBytes;
    const auto imuSerialized = Serialize(imuWireMsg);
    Check(ParseHeader(imuSerialized.data(), kHeaderSize, headerMsg, payloadSize, error) &&
          payloadSize == 48 && headerMsg.type == MessageType::PhoneImu,
          "ParseHeader accepts 48-byte PhoneImu payload");

    badHeader = imuSerialized;
    badHeader[8] = 0; badHeader[9] = 0; badHeader[10] = 0; badHeader[11] = 47;
    Check(!ParseHeader(badHeader.data(), kHeaderSize, headerMsg, payloadSize, error),
          "ParseHeader rejects non-48-byte PhoneImu payload");
}

void TestTcpVideoServerImuQueueIsolation() {
    using namespace phonecast::core::protocol;

    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(probe != INVALID_SOCKET, "probe socket creates");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "ephemeral test port binds");
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closesocket(probe);

    phonecast::platform::network::TcpVideoServer server("123456", port, false);
    std::string error;
    Check(server.Start(error), "tcp video server starts");

    auto waitCondition = [](auto condition, int milliseconds = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(client != INVALID_SOCKET, "client socket creates");
    Check(waitCondition([&] {
        return connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }), "client connects to server");

#ifdef _WIN32
    DWORD timeout = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    auto sendMsg = [](SOCKET s, const Message& m) {
        const auto bytes = Serialize(m);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent),
#ifdef _WIN32
                               0
#else
                               MSG_NOSIGNAL
#endif
            );
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvExact = [](SOCKET s, std::uint8_t* dest, std::size_t count) {
        std::size_t received = 0;
        while (received < count) {
            const int n = recv(s, reinterpret_cast<char*>(dest + received),
                               static_cast<int>(count - received), 0);
            if (n <= 0) return false;
            received += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvMsg = [&](SOCKET s) {
        std::uint8_t headerBytes[kHeaderSize]{};
        Message m;
        std::uint32_t pSize = 0;
        std::string err;
        if (!recvExact(s, headerBytes, sizeof(headerBytes))) return m;
        if (!ParseHeader(headerBytes, sizeof(headerBytes), m, pSize, err)) return m;
        m.payload.resize(pSize);
        if (pSize > 0) {
            if (!recvExact(s, m.payload.data(), pSize)) {
                m.payload.clear();
            }
        }
        return m;
    };

    // Authenticate with pairing code
    Message hello;
    hello.type = MessageType::Hello;
    hello.payload = {'1', '2', '3', '4', '5', '6'};
    Check(sendMsg(client, hello), "send hello pairing message");
    Check(waitCondition([&] { return server.Connected(); }), "server accepts client");

    // Ping with tracking capability
    Message ping;
    ping.type = MessageType::Ping;
    ping.timestampMicros = 777;
    ping.payload = TrackingCapability();
    Check(sendMsg(client, ping), "send ping with tracking capability");

    Message pong = recvMsg(client);
    Check(pong.type == MessageType::Pong && pong.timestampMicros == 777,
          "pong type and timestamp preserved");
    Check(HasTrackingCapability(pong.payload),
          "pong reply includes tracking capability token");

    // Send screen metrics
    StreamScreenMetrics metrics;
    metrics.version = 1;
    metrics.flags = 1;
    metrics.densityDpi = 440;
    metrics.pixelWidth = 1080;
    metrics.pixelHeight = 2400;
    metrics.physicalWidthMm = 70.0F;
    metrics.physicalHeightMm = 155.0F;
    metrics.xdpi = 440.0F;
    metrics.ydpi = 440.0F;
    Message metricsMsg;
    metricsMsg.type = MessageType::ScreenMetrics;
    metricsMsg.payload = SerializeScreenMetrics(metrics);
    Check(sendMsg(client, metricsMsg), "send screen metrics message");

    Check(waitCondition([&] { return server.LatestScreenMetrics().has_value(); }),
          "server received screen metrics");
    auto latestMetrics = server.LatestScreenMetrics().value();
    Check(latestMetrics.densityDpi == 440 && latestMetrics.pixelWidth == 1080 &&
          latestMetrics.pixelHeight == 2400 && latestMetrics.physicalWidthMm == 70.0F,
          "server LatestScreenMetrics content matches");

    // Send 20 IMU messages into queue of capacity 16
    for (unsigned i = 0; i < 20; ++i) {
        StreamPhoneImu imu;
        imu.version = 1;
        imu.sensorType = 1;
        imu.accuracy = 3;
        imu.flags = 0;
        imu.qw = 1.0F;
        imu.gyroX = static_cast<float>(i) * 0.5F;
        Message imuMsg;
        imuMsg.type = MessageType::PhoneImu;
        imuMsg.sequence = i;
        imuMsg.payload = SerializePhoneImu(imu);
        Check(sendMsg(client, imuMsg), "send phone IMU message");
    }

    Check(waitCondition([&] { return server.ImuReceived() == 20; }),
          "server received all 20 IMU messages");
    Check(server.ImuDropped() >= 4,
          "server dropped overflow IMU messages to honor bounded capacity 16");

    // Pop all items and check FIFO drop-oldest ordering
    StreamPhoneImu popped;
    int poppedCount = 0;
    std::vector<float> poppedGyro;
    while (server.PopImu(popped)) {
        ++poppedCount;
        poppedGyro.push_back(popped.gyroX);
    }
    Check(poppedCount == 16, "exactly 16 IMU packets drained from queue");
    Check(!poppedGyro.empty() && std::abs(poppedGyro.front() - (4.0F * 0.5F)) < 0.001F,
          "earliest packet in bounded queue is index 4 (oldest dropped)");
    Check(!poppedGyro.empty() && std::abs(poppedGyro.back() - (19.0F * 0.5F)) < 0.001F,
          "latest packet in bounded queue is index 19");
    Check(!server.PopImu(popped), "IMU queue is empty after popping");

    // Video queue and stats must be completely isolated and untouched
    const auto stats = server.Stats();
    Check(stats.receivedFrames == 0, "video frames received is 0");
    Check(stats.droppedFrames == 0, "video frames dropped is 0");
    Check(stats.resyncRequests == 0, "video resync requests is 0");
    Check(stats.queueDepth == 0, "video queue depth is 0");

    closesocket(client);
    Check(waitCondition([&] { return !server.Connected(); }),
          "server observes client disconnect");
    server.Stop();
}

void TestTcpVideoServerImuFloodingUnderVideoLoad() {
    using namespace phonecast::core::protocol;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(probe != INVALID_SOCKET, "flood: probe socket creates");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "flood: ephemeral port binds");
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closesocket(probe);

    phonecast::platform::network::TcpVideoServer server("123456", port, false);
    std::string error;
    Check(server.Start(error), "flood: tcp server starts");

    auto waitCondition = [](auto condition, int milliseconds = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(client != INVALID_SOCKET, "flood: client socket creates");
    Check(waitCondition([&] {
        return connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }), "flood: client connects");

    auto sendMsg = [](SOCKET s, const Message& m) {
        const auto bytes = Serialize(m);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent),
#ifdef _WIN32
                               0
#else
                               MSG_NOSIGNAL
#endif
            );
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvExact = [](SOCKET s, std::uint8_t* dest, std::size_t count) {
        std::size_t received = 0;
        while (received < count) {
            const int n = recv(s, reinterpret_cast<char*>(dest + received),
                               static_cast<int>(count - received), 0);
            if (n <= 0) return false;
            received += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvMsg = [&](SOCKET s) {
        std::uint8_t headerBytes[kHeaderSize]{};
        Message m;
        std::uint32_t pSize = 0;
        std::string err;
        if (!recvExact(s, headerBytes, sizeof(headerBytes))) return m;
        if (!ParseHeader(headerBytes, sizeof(headerBytes), m, pSize, err)) return m;
        m.payload.resize(pSize);
        if (pSize > 0) {
            if (!recvExact(s, m.payload.data(), pSize)) {
                m.payload.clear();
            }
        }
        return m;
    };

    // Authenticate
    Message hello;
    hello.type = MessageType::Hello;
    hello.payload = {'1', '2', '3', '4', '5', '6'};
    Check(sendMsg(client, hello), "flood: send hello");
    Check(waitCondition([&] { return server.Connected(); }), "flood: server connected");

    // Negotiate tracking
    Message ping;
    ping.type = MessageType::Ping;
    ping.timestampMicros = 888;
    ping.payload = TrackingCapability();
    Check(sendMsg(client, ping), "flood: send ping tracking");
    Message pong = recvMsg(client);
    Check(HasTrackingCapability(pong.payload), "flood: tracking capability negotiated");

    // Send video config
    Message configMsg;
    configMsg.type = MessageType::VideoConfig;
    configMsg.payload = {0x01, 0x02, 0x03};
    Check(sendMsg(client, configMsg), "flood: send video config");

    // Drain initial keyframe request triggered by VideoConfig
    Message reqKey = recvMsg(client);
    Check(reqKey.type == MessageType::RequestKeyFrame, "flood: received expected initial keyframe request");

    // Pop the VideoConfig on server side
    Message poppedConfig;
    Check(waitCondition([&] { return server.Pop(poppedConfig); }), "flood: server pops video config");
    Check(poppedConfig.type == MessageType::VideoConfig, "flood: popped config matches");

    // Send Key Frame (seq 0)
    Message keyFrame;
    keyFrame.type = MessageType::VideoFrame;
    keyFrame.flags = MessageFlags::KeyFrame;
    keyFrame.sequence = 0;
    keyFrame.payload = {0x00, 0x00, 0x00, 0x01, 0x65};
    Check(sendMsg(client, keyFrame), "flood: send initial keyframe");

    // Start background thread that pops video frames from the server
    std::atomic<bool> stopPopping{false};
    std::atomic<int> poppedVideoCount{0};
    std::thread popThread([&] {
        while (!stopPopping.load()) {
            Message vMsg;
            if (server.Pop(vMsg)) {
                ++poppedVideoCount;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        // Drain any remaining
        Message vMsg;
        while (server.Pop(vMsg)) {
            ++poppedVideoCount;
        }
    });

    // Start background thread polling LatestImu and PopImu
    std::thread imuDrainThread([&] {
        StreamPhoneImu imu;
        while (!stopPopping.load()) {
            (void)server.LatestImu();
            server.PopImu(imu);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Now send 50 delta video frames interleaved with 2,000 IMU packets!
    // Total video frames: 1 keyframe + 50 deltas = 51 frames.
    const int kTotalDeltas = 50;
    const int kTotalImu = 2000;
    const int imuPerDelta = kTotalImu / kTotalDeltas;

    StreamPhoneImu baseImu;
    baseImu.version = 1;
    baseImu.sensorType = 1;
    baseImu.accuracy = 3;
    baseImu.flags = 0;
    baseImu.qw = 1.0F;
    const auto serializedImu = SerializePhoneImu(baseImu);

    int imuSent = 0;
    for (int delta = 1; delta <= kTotalDeltas; ++delta) {
        // Send a burst of IMU packets
        for (int i = 0; i < imuPerDelta; ++i) {
            Message imuMsg;
            imuMsg.type = MessageType::PhoneImu;
            imuMsg.sequence = imuSent++;
            imuMsg.payload = serializedImu;
            sendMsg(client, imuMsg);
        }

        // Send delta video frame
        Message deltaMsg;
        deltaMsg.type = MessageType::VideoFrame;
        deltaMsg.flags = 0;
        deltaMsg.sequence = delta;
        deltaMsg.payload = {0x00, 0x00, 0x00, 0x01, 0x41};
        sendMsg(client, deltaMsg);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Check(waitCondition([&] { return server.ImuReceived() == kTotalImu; }, 4000),
          "flood: server received all 2000 IMU packets");
    Check(waitCondition([&] { return poppedVideoCount.load() >= 51; }, 4000),
          "flood: server processed all 51 video frames");

    stopPopping.store(true);
    popThread.join();
    imuDrainThread.join();

    // Verify video queue stats under 2,000 packet IMU flood:
    const auto stats = server.Stats();
    Check(stats.receivedFrames == 51, "flood: receivedFrames exactly 51");
    Check(stats.droppedFrames == 0, "flood: droppedFrames is 0 (NO video drops)");
    Check(stats.resyncRequests == 1, "flood: resyncRequests is 1 (only the startup VideoConfig)");
    Check(stats.queueDepth == 0, "flood: video queue depth is 0");
    Check(server.ImuReceived() == kTotalImu, "flood: imuReceived matches");

    closesocket(client);
    server.Stop();
}

void TestTcpVideoServerImuFifoDropOldestExactness() {
    using namespace phonecast::core::protocol;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closesocket(probe);

    phonecast::platform::network::TcpVideoServer server("654321", port, false);
    std::string error;
    Check(server.Start(error), "fifo: server starts");

    auto waitCondition = [](auto condition, int milliseconds = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    waitCondition([&] {
        return connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    });

    auto sendMsg = [](SOCKET s, const Message& m) {
        const auto bytes = Serialize(m);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent),
#ifdef _WIN32
                               0
#else
                               MSG_NOSIGNAL
#endif
            );
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvExact = [](SOCKET s, std::uint8_t* dest, std::size_t count) {
        std::size_t received = 0;
        while (received < count) {
            const int n = recv(s, reinterpret_cast<char*>(dest + received),
                               static_cast<int>(count - received), 0);
            if (n <= 0) return false;
            received += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvMsg = [&](SOCKET s) {
        std::uint8_t headerBytes[kHeaderSize]{};
        Message m;
        std::uint32_t pSize = 0;
        std::string err;
        if (!recvExact(s, headerBytes, sizeof(headerBytes))) return m;
        if (!ParseHeader(headerBytes, sizeof(headerBytes), m, pSize, err)) return m;
        m.payload.resize(pSize);
        if (pSize > 0) recvExact(s, m.payload.data(), pSize);
        return m;
    };

    Message hello;
    hello.type = MessageType::Hello;
    hello.payload = {'6', '5', '4', '3', '2', '1'};
    sendMsg(client, hello);
    waitCondition([&] { return server.Connected(); });

    Message ping;
    ping.type = MessageType::Ping;
    ping.payload = TrackingCapability();
    sendMsg(client, ping);
    recvMsg(client);

    Check(server.ImuReceived() == 0, "fifo: initial received is 0");
    Check(server.ImuDropped() == 0, "fifo: initial dropped is 0");

    // Send 500 IMU packets with gyroX = -25.0f + i * 0.1f (within [-50, 50] limit)
    const int kTotal = 500;
    for (int i = 0; i < kTotal; ++i) {
        StreamPhoneImu imu;
        imu.version = 1;
        imu.sensorType = 1;
        imu.accuracy = 3;
        imu.flags = 0;
        imu.qw = 1.0F;
        imu.gyroX = -25.0F + static_cast<float>(i) * 0.1F;
        Message msg;
        msg.type = MessageType::PhoneImu;
        msg.sequence = i;
        msg.payload = SerializePhoneImu(imu);
        Check(!msg.payload.empty(), "fifo: payload serialization succeeds within bounds");
        sendMsg(client, msg);
    }

    Check(waitCondition([&] { return server.ImuReceived() == kTotal; }, 3000),
          "fifo: all 500 packets received");
    // Bounded capacity is 16 -> exactly 500 - 16 = 484 packets must be dropped
    Check(server.ImuDropped() == 484, "fifo: exactly 484 packets dropped");

    // LatestImu must reflect the very last packet (index 499)
    auto latest = server.LatestImu();
    Check(latest.has_value(), "fifo: LatestImu has value");
    float expectedLatest = -25.0F + 499.0F * 0.1F;
    Check(std::abs(latest->gyroX - expectedLatest) < 0.001F, "fifo: LatestImu is 24.9f");

    // Pop the 16 remaining items and verify exact order 484 .. 499
    StreamPhoneImu popped;
    int popCount = 0;
    std::vector<float> poppedValues;
    while (server.PopImu(popped)) {
        ++popCount;
        poppedValues.push_back(popped.gyroX);
    }
    Check(popCount == 16, "fifo: exactly 16 items popped");
    Check(poppedValues.size() == 16, "fifo: 16 values recorded");
    bool exactOrder = true;
    for (int j = 0; j < 16; ++j) {
        float expected = -25.0F + static_cast<float>(484 + j) * 0.1F;
        if (std::abs(poppedValues[j] - expected) > 0.001F) {
            exactOrder = false;
        }
    }
    Check(exactOrder, "fifo: popped items are strictly 484 to 499 in sequential FIFO order");
    Check(!server.PopImu(popped), "fifo: queue is completely empty after 16 pops");

    closesocket(client);
    server.Stop();
}

void TestTcpVideoServerImuAdversarialValidation() {
    using namespace phonecast::core::protocol;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closesocket(probe);

    phonecast::platform::network::TcpVideoServer server("112233", port, false);
    std::string error;
    Check(server.Start(error), "adv: server starts");

    auto waitCondition = [](auto condition, int milliseconds = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    waitCondition([&] {
        return connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    });

    auto sendMsg = [](SOCKET s, const Message& m) {
        const auto bytes = Serialize(m);
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent),
#ifdef _WIN32
                               0
#else
                               MSG_NOSIGNAL
#endif
            );
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvExact = [](SOCKET s, std::uint8_t* dest, std::size_t count) {
        std::size_t received = 0;
        while (received < count) {
            const int n = recv(s, reinterpret_cast<char*>(dest + received),
                               static_cast<int>(count - received), 0);
            if (n <= 0) return false;
            received += static_cast<std::size_t>(n);
        }
        return true;
    };

    auto recvMsg = [&](SOCKET s) {
        std::uint8_t headerBytes[kHeaderSize]{};
        Message m;
        std::uint32_t pSize = 0;
        std::string err;
        if (!recvExact(s, headerBytes, sizeof(headerBytes))) return m;
        if (!ParseHeader(headerBytes, sizeof(headerBytes), m, pSize, err)) return m;
        m.payload.resize(pSize);
        if (pSize > 0) recvExact(s, m.payload.data(), pSize);
        return m;
    };

    // Authenticate
    Message hello;
    hello.type = MessageType::Hello;
    hello.payload = {'1', '1', '2', '2', '3', '3'};
    sendMsg(client, hello);
    waitCondition([&] { return server.Connected(); });

    // Attack 1: Send 10 IMU packets BEFORE negotiating tracking capability
    StreamPhoneImu validImu;
    validImu.version = 1;
    validImu.sensorType = 1;
    validImu.accuracy = 3;
    validImu.flags = 0;
    validImu.qw = 1.0F;
    const auto validBytes = SerializePhoneImu(validImu);

    for (int i = 0; i < 10; ++i) {
        Message msg;
        msg.type = MessageType::PhoneImu;
        msg.sequence = i;
        msg.payload = validBytes;
        sendMsg(client, msg);
    }

    Check(waitCondition([&] { return server.ImuDropped() == 10; }),
          "adv: unnegotiated IMU packets dropped");
    Check(server.ImuReceived() == 0, "adv: unnegotiated IMU not counted in received");
    Check(!server.LatestImu().has_value(), "adv: unnegotiated IMU not exposed in LatestImu");

    // Negotiate tracking capability now
    Message ping;
    ping.type = MessageType::Ping;
    ping.payload = TrackingCapability();
    sendMsg(client, ping);
    recvMsg(client);

    // Attack 2: Send 8 malformed payloads (48 bytes each to pass header framing, but invalid content)
    std::vector<std::vector<std::uint8_t>> malformedPayloads;

    // Bad version (2)
    auto badVersion = validBytes; badVersion[0] = 2;
    malformedPayloads.push_back(badVersion);

    // Bad sensor type (99)
    auto badSensor = validBytes; badSensor[1] = 99;
    malformedPayloads.push_back(badSensor);

    // Bad accuracy (4)
    auto badAcc = validBytes; badAcc[2] = 4;
    malformedPayloads.push_back(badAcc);

    // Bad flags (0x04)
    auto badFlags = validBytes; badFlags[3] = 0x04;
    malformedPayloads.push_back(badFlags);

    // Unnormalized quaternion (all zeros)
    auto zeroQuat = validBytes;
    std::fill(zeroQuat.begin() + 4, zeroQuat.begin() + 20, 0);
    malformedPayloads.push_back(zeroQuat);

    // Extreme angular velocity (>50 rad/s)
    auto extremeGyro = validBytes;
    float highRate = 55.0F;
    std::uint32_t highRateBits{};
    std::memcpy(&highRateBits, &highRate, 4);
    extremeGyro[20] = static_cast<std::uint8_t>(highRateBits >> 24);
    extremeGyro[21] = static_cast<std::uint8_t>(highRateBits >> 16);
    extremeGyro[22] = static_cast<std::uint8_t>(highRateBits >> 8);
    extremeGyro[23] = static_cast<std::uint8_t>(highRateBits);
    malformedPayloads.push_back(extremeGyro);

    // Extreme acceleration (>200 m/s^2)
    auto extremeAccel = validBytes;
    float highAccel = 250.0F;
    std::uint32_t highAccelBits{};
    std::memcpy(&highAccelBits, &highAccel, 4);
    extremeAccel[32] = static_cast<std::uint8_t>(highAccelBits >> 24);
    extremeAccel[33] = static_cast<std::uint8_t>(highAccelBits >> 16);
    extremeAccel[34] = static_cast<std::uint8_t>(highAccelBits >> 8);
    extremeAccel[35] = static_cast<std::uint8_t>(highAccelBits);
    malformedPayloads.push_back(extremeAccel);

    // Non-zero reserved bytes (byte 44 = 1)
    auto nonZeroReserved = validBytes;
    nonZeroReserved[44] = 1;
    malformedPayloads.push_back(nonZeroReserved);

    for (std::size_t i = 0; i < malformedPayloads.size(); ++i) {
        Message msg;
        msg.type = MessageType::PhoneImu;
        msg.sequence = 100 + i;
        msg.payload = malformedPayloads[i];
        sendMsg(client, msg);
    }

    Check(waitCondition([&] { return server.ImuDropped() == 18; }),
          "adv: all 8 malformed packets dropped (10 + 8 = 18)");
    Check(server.ImuReceived() == 0, "adv: no malformed packet counted as received");
    Check(!server.LatestImu().has_value(), "adv: LatestImu still empty");

    // Attack 3: Send 1 valid IMU packet to confirm stream remains functional
    Message validMsg;
    validMsg.type = MessageType::PhoneImu;
    validMsg.sequence = 200;
    validMsg.payload = validBytes;
    sendMsg(client, validMsg);

    Check(waitCondition([&] { return server.ImuReceived() == 1; }),
          "adv: valid packet received after malformed flood");
    Check(server.LatestImu().has_value(), "adv: LatestImu now populated");
    StreamPhoneImu popped;
    Check(server.PopImu(popped), "adv: valid packet popped successfully");
    Check(!server.PopImu(popped), "adv: queue empty after pop");

    // Video stats must remain completely 0
    const auto stats = server.Stats();
    Check(stats.droppedFrames == 0, "adv: video dropped frames is 0");
    Check(stats.resyncRequests == 0, "adv: video resync requests is 0");

    closesocket(client);
    server.Stop();
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    TestConfig();
    TestDecoderRecovery();
    TestGeneratedFrames();
    TestStreamProtocol();
    TestTrackingProtocol();
    TestOverlayInteraction();
    TestOverlayControls();
    TestControllerShortcut();
    TestGlanceMode();
    TestOverlaySettingsPersistence();
    TestSettingsMenu();
    TestWristMenuGesture();
    TestReceiverLifecycle();
    TestTcpVideoServerImuQueueIsolation();
    TestTcpVideoServerImuFloodingUnderVideoLoad();
    TestTcpVideoServerImuFifoDropOldestExactness();
    TestTcpVideoServerImuAdversarialValidation();
#ifdef _WIN32
    WSACleanup();
#endif
    if (failures == 0) std::cout << "All PhoneCast tests passed.\n";
    return failures == 0 ? 0 : 1;
}

