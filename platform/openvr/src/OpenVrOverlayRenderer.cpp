#include "phonecast/platform/openvr/OpenVrOverlayRenderer.h"
#include "phonecast/vr/interaction/ControllerShortcut.h"
#include "phonecast/vr/interaction/OverlayInteractionController.h"
#include "phonecast/vr/interaction/PanelDepth.h"
#include "phonecast/vr/interaction/StartLocation.h"
#include "phonecast/vr/overlay/WristMenuGesture.h"
#include "phonecast/vr/ui/OverlayUiRenderer.h"
#include "TrackedCameraWorker.h"

#include <openvr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#else
#include "SteamFrameVulkanTexture.h"

#include <unistd.h>
#endif

namespace phonecast::platform::openvr {
namespace vr = ::vr;
namespace {

constexpr char kApplicationKey[] = "com.phonecastvr.receiver";
constexpr char kOverlayKey[] = "com.phonecastvr.receiver.phone";
constexpr char kOverlayName[] = "PhoneCast VR Receiver";
constexpr char kMenuOverlayKey[] = "com.phonecastvr.receiver.radial-menu";
constexpr char kMenuOverlayName[] = "PhoneCast Controls";
constexpr char kGestureOverlayKey[] = "com.phonecastvr.receiver.gesture-progress";
constexpr char kGestureOverlayName[] = "PhoneCast Gesture Progress";
constexpr char kSettingsOverlayKey[] = "com.phonecastvr.receiver.settings";
constexpr char kSettingsOverlayName[] = "PhoneCast Settings";
constexpr char kDashboardOverlayKey[] = "com.phonecastvr.receiver.dashboard";
constexpr char kDashboardOverlayName[] = "PhoneCast";
constexpr char kNotificationOverlayKey[] = "com.phonecastvr.receiver.notification";
constexpr char kNotificationOverlayName[] = "PhoneCast Notification";
constexpr char kPassthroughOverlayKey[] = "com.phonecastvr.receiver.passthrough";
constexpr char kPassthroughOverlayName[] = "PhoneCast Passthrough Cutout";
using namespace phonecast::vr::ui;

// The dashboard is the approved in-headset entry point. Keep the earlier pose,
// thumbstick-menu, and overlay-global input experiments dormant until product
// direction changes. An active overlay-global set can suppress game bindings
// even when PhoneCast ignores the resulting actions.
constexpr bool kEnableExperimentalGestureControls = false;
constexpr bool kEnableExperimentalOverlayGlobalInput = false;
constexpr std::array<const char*, 9> kMenuLabels{
    "SHOW", "GLANCE", "PIN", "SETTINGS", "HEAD", "WORLD", "LEFT", "RIGHT", "CLOSE"};
constexpr std::array<phonecast::vr::RadialMenuAction, 8> kMenuActions{
    phonecast::vr::RadialMenuAction::ToggleVisible,
    phonecast::vr::RadialMenuAction::ShowGlance,
    phonecast::vr::RadialMenuAction::ShowPinned,
    phonecast::vr::RadialMenuAction::OpenSettings,
    phonecast::vr::RadialMenuAction::HeadLocked,
    phonecast::vr::RadialMenuAction::WorldLocked,
    phonecast::vr::RadialMenuAction::LeftControllerLocked,
    phonecast::vr::RadialMenuAction::RightControllerLocked};

bool IsQuitEvent(std::uint32_t type) {
    // VREvent_ProcessQuit reports that some VR process exited; it is not a request
    // for this overlay process to stop during scene-application transitions.
    return type == vr::VREvent_Quit || type == vr::VREvent_DriverRequestedQuit;
}

constexpr float kPi = 3.14159265358979323846F;

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

bool IsControllerMode(phonecast::vr::PlacementMode mode) {
    return mode == phonecast::vr::PlacementMode::LeftControllerLocked ||
           mode == phonecast::vr::PlacementMode::RightControllerLocked;
}

const phonecast::vr::ControllerCalibration& ControllerFor(
        const phonecast::vr::OverlaySettings& settings) {
    return settings.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked
        ? settings.leftController : settings.rightController;
}

phonecast::vr::ControllerCalibration& ControllerFor(phonecast::vr::OverlaySettings& settings) {
    return settings.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked
        ? settings.leftController : settings.rightController;
}

vr::HmdMatrix34_t LocalTransform(const phonecast::vr::OverlaySettings& settings) {
    return {{{1.0F, 0.0F, 0.0F, settings.offsetXMeters},
             {0.0F, 1.0F, 0.0F, settings.offsetYMeters},
             {0.0F, 0.0F, 1.0F, -settings.distanceMeters}}};
}

vr::HmdMatrix34_t Rotation(float tiltDegrees, float yawDegrees) {
    const float x = tiltDegrees * kPi / 180.0F;
    const float y = yawDegrees * kPi / 180.0F;
    const float cx = std::cos(x), sx = std::sin(x);
    const float cy = std::cos(y), sy = std::sin(y);
    return {{{cy, sy * sx, sy * cx, 0.0F},
             {0.0F, cx, -sx, 0.0F},
             {-sy, cy * sx, cy * cx, 0.0F}}};
}

vr::HmdMatrix34_t ControllerLocalTransform(
        const phonecast::vr::ControllerCalibration& calibration,
        float extraTilt = 0.0F, float extraYaw = 0.0F) {
    auto result = Rotation(calibration.tiltDegrees + extraTilt,
                           calibration.yawDegrees + extraYaw);
    result.m[0][3] = calibration.lateralMeters;
    result.m[1][3] = calibration.heightMeters;
    result.m[2][3] = -calibration.distanceMeters;
    return result;
}

vr::HmdMatrix34_t FromArray(const std::array<float, 12>& values) {
    vr::HmdMatrix34_t result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result.m[row][column] = values[row * 4 + column];
    return result;
}

std::array<float, 12> ToArray(const vr::HmdMatrix34_t& matrix) {
    std::array<float, 12> result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result[row * 4 + column] = matrix.m[row][column];
    return result;
}

vr::HmdMatrix34_t Multiply(const vr::HmdMatrix34_t& left, const vr::HmdMatrix34_t& right) {
    vr::HmdMatrix34_t result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t inner = 0; inner < 3; ++inner)
                result.m[row][column] += left.m[row][inner] * right.m[inner][column];
        }
        result.m[row][3] = left.m[row][3];
        for (std::size_t inner = 0; inner < 3; ++inner)
            result.m[row][3] += left.m[row][inner] * right.m[inner][3];
    }
    return result;
}

vr::HmdMatrix34_t InverseRigid(const vr::HmdMatrix34_t& matrix) {
    vr::HmdMatrix34_t result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column)
            result.m[row][column] = matrix.m[column][row];
        for (std::size_t inner = 0; inner < 3; ++inner)
            result.m[row][3] -= result.m[row][inner] * matrix.m[inner][3];
    }
    return result;
}

inline std::vector<std::uint8_t> MakeSettingsTexture(const phonecast::vr::SettingsMenuView& view) {
    return phonecast::vr::ui::RenderSettingsMenu(view);
}

inline std::vector<std::uint8_t> MakeGestureProgressTexture(int completedSteps) {
    return phonecast::vr::ui::RenderGestureProgress(completedSteps);
}

inline std::vector<std::uint8_t> MakeNotificationTexture(
        const phonecast::core::protocol::NotificationEvent& notification) {
    return phonecast::vr::ui::RenderNotification(notification);
}

inline void DrawGrabFooter(phonecast::core::VideoFrame& composite,
                    std::uint32_t phoneHeight,
                    FooterHoverTarget hover = FooterHoverTarget::None) {
    phonecast::vr::ui::RenderGrabFooter(composite, phoneHeight, hover);
}

inline void AddGrabHandle(const phonecast::core::VideoFrame& frame,
                   phonecast::core::VideoFrame& composite,
                   FooterHoverTarget hover = FooterHoverTarget::None) {
    phonecast::vr::ui::AddGrabFooter(frame, composite, hover);
}

inline std::vector<std::uint8_t> MakeDashboardTexture(
        bool currentlyVisible, phonecast::vr::PlacementMode placementMode,
        bool remoteStatusKnown, bool remoteAppEnabled,
        bool remoteAccessibilityEnabled, const std::string& pairingCode,
        int hoveredCell = -1) {
    return phonecast::vr::ui::RenderDashboard(
        currentlyVisible, placementMode, remoteStatusKnown,
        remoteAppEnabled, remoteAccessibilityEnabled, pairingCode, hoveredCell);
}

inline std::vector<std::uint8_t> MakeDashboardThumbnail() {
    return phonecast::vr::ui::RenderDashboardThumbnail();
}

inline int DashboardCell(float mouseX, float mouseY) {
    return phonecast::vr::ui::HitTestDashboard(mouseX, mouseY);
}

inline std::vector<std::uint8_t> MakeRadialMenuTexture(int selected, bool currentlyVisible) {
    return phonecast::vr::ui::RenderRadialMenu(selected, currentlyVisible);
}

}  // namespace

class OpenVrOverlayRenderer::Impl {
public:
    Impl(core::ILogger& logger, std::string executablePath)
        : logger(logger), executablePath(std::move(executablePath)) {}

    bool OverlayCall(vr::EVROverlayError result, const char* operation, std::string& error) {
        if (result == vr::VROverlayError_None) return true;
        const char* name = overlayApi != nullptr ? overlayApi->GetOverlayErrorNameFromEnum(result) : nullptr;
        error = std::string(operation) + " failed: " + (name != nullptr ? name : "unknown error") +
                " (" + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }

    void RegisterManifest() {
        vr::IVRApplications* applications = vr::VRApplications();
        if (applications == nullptr) {
            logger.Log(core::LogLevel::Warning, "openvr", "IVRApplications is unavailable; manifest not registered.");
            return;
        }
        std::error_code pathError;
        const auto executable = std::filesystem::absolute(executablePath, pathError);
        const auto manifest = executable.parent_path() / "phonecast-receiver.vrmanifest";
        if (pathError || !std::filesystem::exists(manifest)) {
            logger.Log(core::LogLevel::Warning, "openvr", "Receiver manifest was not found beside the executable.");
            return;
        }
        const auto result = applications->AddApplicationManifest(manifest.string().c_str(), false);
        if (result != vr::VRApplicationError_None) {
            const char* name = applications->GetApplicationsErrorNameFromEnum(result);
            logger.Log(core::LogLevel::Warning, "openvr",
                       std::string("Manifest registration failed: ") + (name != nullptr ? name : "unknown error"));
            return;
        }
        logger.Log(core::LogLevel::Info, "openvr", std::string("Registered ") + kApplicationKey + '.');
#ifdef _WIN32
        const auto processId = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
        const auto processId = static_cast<std::uint32_t>(getpid());
#endif
        const auto identifyResult = applications->IdentifyApplication(processId, kApplicationKey);
        if (identifyResult != vr::VRApplicationError_None) {
            const char* identifyName = applications->GetApplicationsErrorNameFromEnum(identifyResult);
            logger.Log(core::LogLevel::Warning, "openvr",
                       std::string("IdentifyApplication failed: ") +
                       (identifyName != nullptr ? identifyName : "unknown error"));
        } else {
            logger.Log(core::LogLevel::Info, "openvr", "Associated this process with the registered application key.");
        }
    }

    void InitializeShortcut() {
        inputApi = vr::VRInput();
        if (!inputApi) return;
        const auto manifest = std::filesystem::absolute(executablePath).parent_path() /
            "phonecast-actions.json";
        const bool ok =
            inputApi->SetActionManifestPath(manifest.string().c_str()) == vr::VRInputError_None &&
            inputApi->GetActionSetHandle("/actions/phonecast_shortcuts", &shortcutSet) == vr::VRInputError_None &&
            inputApi->GetActionHandle("/actions/phonecast_shortcuts/in/toggle_left", &shortcutToggleLeft) == vr::VRInputError_None &&
            inputApi->GetActionHandle("/actions/phonecast_shortcuts/in/toggle_right", &shortcutToggleRight) == vr::VRInputError_None &&
            inputApi->GetActionHandle("/actions/phonecast_shortcuts/in/dock_left", &shortcutDockLeft) == vr::VRInputError_None &&
            inputApi->GetActionHandle("/actions/phonecast_shortcuts/in/dock_right", &shortcutDockRight) == vr::VRInputError_None;
        if (!ok) {
            logger.Log(core::LogLevel::Warning, "openvr-shortcut", "Shortcut/panel action initialization failed.");
            return;
        }
        inputApi->GetActionHandle("/actions/phonecast_shortcuts/out/haptic", &shortcutHaptic);
        shortcutReady = true;
        logger.Log(core::LogLevel::Info, "openvr-shortcut",
                   "Both-stick hold toggles and double-click docking initialized at normal priority.");
    }

    bool DigitalPressed(vr::VRActionHandle_t action, bool& active) const {
        vr::InputDigitalActionData_t data{};
        if (inputApi->GetDigitalActionData(action, &data, sizeof(data),
                vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None) return false;
        active = active || data.bActive;
        return data.bActive && data.bChanged && data.bState;
    }

    void PollShortcut() {
        if (!shortcutReady) return;
        const bool dashboardVisible = overlayApi->IsDashboardVisible();
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = shortcutSet;
        active.nPriority = 0;
        // Explicitly deactivate the shortcut set when the dashboard owns input.
        if (inputApi->UpdateActionState(&active, sizeof(active), dashboardVisible ? 0 : 1) !=
            vr::VRInputError_None) return;
        if (dashboardVisible) {
            shortcutActive = false;
            return;
        }
        bool anyActive = false;
        const bool dockLeft = DigitalPressed(shortcutDockLeft, anyActive);
        const bool dockRight = DigitalPressed(shortcutDockRight, anyActive);
        const bool toggle = DigitalPressed(shortcutToggleLeft, anyActive) |
                            DigitalPressed(shortcutToggleRight, anyActive);
        if (anyActive != shortcutActive) {
            shortcutActive = anyActive;
            logger.Log(core::LogLevel::Info, "openvr-shortcut",
                       shortcutActive ? "Shortcut bindings active." : "Shortcut bindings inactive.");
        }
        if (hasPendingRadialMenuSelection) return;
        if (dockLeft || dockRight) {
            pendingRadialMenuSelection.hand = dockLeft
                ? phonecast::vr::GlanceInput::LeftController
                : phonecast::vr::GlanceInput::RightController;
            pendingRadialMenuSelection.action = phonecast::vr::ControllerDockShortcutAction(
                pendingRadialMenuSelection.hand, desiredVisible, currentSettings.placementMode);
            hasPendingRadialMenuSelection = true;
        } else if (toggle) {
            pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::ToggleVisible;
            hasPendingRadialMenuSelection = true;
        }
    }

    void RenderDashboard() {
        if (dashboardOverlay == vr::k_ulOverlayHandleInvalid) return;
        auto image = MakeDashboardTexture(desiredVisible, currentSettings.placementMode,
                                           remoteStatusKnown, remoteAppEnabled,
                                           remoteAccessibilityEnabled, pairingCode,
                                           hoveredDashboardCell);
        const auto result = overlayApi->SetOverlayRaw(
            dashboardOverlay, image.data(), kDashboardTextureWidth, kDashboardTextureHeight, 4);
        if (result != vr::VROverlayError_None)
            logger.Log(core::LogLevel::Warning, "openvr-dashboard",
                       "Failed to update the PhoneCast dashboard panel.");
    }

    void UpdateStartLocationPreview(bool visible) {
        if (gestureOverlay == vr::k_ulOverlayHandleInvalid) return;
        if (!visible) {
            if (startLocationPreviewVisible) overlayApi->HideOverlay(gestureOverlay);
            startLocationPreviewVisible = false;
            return;
        }
        if (!startLocationPreviewVisible) {
            if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, startPreviewHead)) return;
            constexpr std::uint32_t width = 256;
            const auto phoneHeight = compositeFrame.height > kGrabHandleHeightPixels
                ? compositeFrame.height - kGrabHandleHeightPixels : 512U;
            const auto height = static_cast<std::uint32_t>(Clamp(
                width * static_cast<float>(phoneHeight) / std::max(1U, compositeFrame.width), 128, 768));
            UiCanvas canvas(width, height);
            canvas.FillRoundedRect(0, 0, width, height, 16, {48, 153, 245, 255});
            canvas.FillRoundedRect(4, 4, width - 8, height - 8, 12, {10, 25, 42, 230});
            canvas.DrawLabel("PHONE PREVIEW", 128, height / 2, 2, {224, 235, 249, 255});
            if (overlayApi->SetOverlayRaw(gestureOverlay, canvas.Pixels().data(), width, height, 4) !=
                vr::VROverlayError_None) return;
        }
        float width = currentSettings.widthMeters;
        overlayApi->GetOverlayWidthInMeters(overlay, &width);
        const auto transform = Multiply(startPreviewHead, FromArray(phonecast::vr::StartLocation(
            currentSettings.startOffsetXMeters, currentSettings.startOffsetYMeters,
            currentSettings.startDistanceMeters)));
        overlayApi->SetOverlayWidthInMeters(gestureOverlay, width);
        overlayApi->SetOverlayTransformAbsolute(gestureOverlay, vr::TrackingUniverseStanding, &transform);
        overlayApi->ShowOverlay(gestureOverlay);
        startLocationPreviewVisible = true;
    }

    void UpdateNotificationPreview(bool visible) {
        if (notificationOverlay == vr::k_ulOverlayHandleInvalid) return;
        if (!visible) {
            if (notificationPreviewVisible) overlayApi->HideOverlay(notificationOverlay);
            notificationPreviewVisible = false;
            return;
        }
        core::protocol::NotificationEvent preview;
        preview.applicationName = "PHONECAST PREVIEW";
        preview.title = "NOTIFICATION CARD";
        preview.body = "DRAG THE SLIDERS TO PLACE ME";
        preview.packageName = "com.phonecastvr.preview";
        preview.contentRedacted = false;
        auto image = MakeNotificationTexture(preview);
        const auto textureResult = overlayApi->SetOverlayRaw(
            notificationOverlay, image.data(), kNotificationTextureWidth,
            kNotificationTextureHeight, 4);
        const auto showResult = textureResult == vr::VROverlayError_None
            ? overlayApi->ShowOverlay(notificationOverlay) : textureResult;
        if (textureResult != vr::VROverlayError_None ||
            showResult != vr::VROverlayError_None) {
            logger.Log(core::LogLevel::Warning, "openvr-notification",
                       "Could not display the notification placement preview.");
            return;
        }
        notificationVisible = false;
        currentNotificationActionToken = 0;
        notificationPreviewVisible = true;
    }

    void DestroyDashboard() {
        if (overlayApi == nullptr) return;
        if (dashboardOverlay != vr::k_ulOverlayHandleInvalid)
            overlayApi->DestroyOverlay(dashboardOverlay);
        if (dashboardThumbnail != vr::k_ulOverlayHandleInvalid)
            overlayApi->DestroyOverlay(dashboardThumbnail);
        dashboardOverlay = vr::k_ulOverlayHandleInvalid;
        dashboardThumbnail = vr::k_ulOverlayHandleInvalid;
    }

    void InitializeDashboard() {
        const auto result = overlayApi->CreateDashboardOverlay(
            kDashboardOverlayKey, kDashboardOverlayName,
            &dashboardOverlay, &dashboardThumbnail);
        if (result != vr::VROverlayError_None) {
            const char* name = overlayApi->GetOverlayErrorNameFromEnum(result);
            logger.Log(core::LogLevel::Warning, "openvr-dashboard",
                       std::string("Dashboard tab is unavailable: ") +
                       (name != nullptr ? name : "unknown error") + ".");
            dashboardOverlay = vr::k_ulOverlayHandleInvalid;
            dashboardThumbnail = vr::k_ulOverlayHandleInvalid;
            return;
        }

        auto thumbnail = MakeDashboardThumbnail();
        const vr::HmdVector2_t mouseScale{{static_cast<float>(kDashboardTextureWidth),
                                           static_cast<float>(kDashboardTextureHeight)}};
        const auto panelResult = overlayApi->SetOverlayRaw(
            dashboardOverlay,
            MakeDashboardTexture(desiredVisible, currentSettings.placementMode,
                                 remoteStatusKnown, remoteAppEnabled,
                                 remoteAccessibilityEnabled, pairingCode,
                                 hoveredDashboardCell).data(),
            kDashboardTextureWidth, kDashboardTextureHeight, 4);
        const auto thumbnailResult = overlayApi->SetOverlayRaw(
            dashboardThumbnail, thumbnail.data(),
            kDashboardThumbnailSize, kDashboardThumbnailSize, 4);
        const auto scaleResult = overlayApi->SetOverlayMouseScale(dashboardOverlay, &mouseScale);
        if (panelResult != vr::VROverlayError_None ||
            thumbnailResult != vr::VROverlayError_None ||
            scaleResult != vr::VROverlayError_None) {
            logger.Log(core::LogLevel::Warning, "openvr-dashboard",
                       "Dashboard tab was created but its panel, icon, or pointer scale failed.");
            DestroyDashboard();
            return;
        }
        logger.Log(core::LogLevel::Info, "openvr-dashboard",
                   "PhoneCast dashboard tab and launcher icon created.");
    }

    void EnsureDashboardHealthy() {
        if (overlayApi == nullptr) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDashboardHealthCheck < std::chrono::seconds(2)) return;
        lastDashboardHealthCheck = now;
        bool healthy = false;
        if (dashboardOverlay != vr::k_ulOverlayHandleInvalid) {
            std::array<char, 256> key{};
            vr::EVROverlayError keyError = vr::VROverlayError_None;
            overlayApi->GetOverlayKey(dashboardOverlay, key.data(),
                                      static_cast<std::uint32_t>(key.size()), &keyError);
            healthy = keyError == vr::VROverlayError_None &&
                      std::string(key.data()) == kDashboardOverlayKey;
        }
        if (healthy) return;
        logger.Log(core::LogLevel::Warning, "openvr-dashboard",
                   "Dashboard handle was lost; recreating the PhoneCast tab.");
        if (dashboardThumbnail != vr::k_ulOverlayHandleInvalid)
            overlayApi->DestroyOverlay(dashboardThumbnail);
        dashboardOverlay = vr::k_ulOverlayHandleInvalid;
        dashboardThumbnail = vr::k_ulOverlayHandleInvalid;
        InitializeDashboard();
    }

    void QueueDashboardCell(int cell) {
        if (cell < 0 || cell >= static_cast<int>(kDashboardTargets.size())) return;
        const auto control = kDashboardTargets[static_cast<std::size_t>(cell)].control;
        if (control == DashboardControl::AndroidBack) {
            pendingPointerEvents.push_back(interaction.Back());
            logger.Log(core::LogLevel::Info, "openvr-dashboard",
                       "Android Back requested from the dashboard.");
            return;
        }
        if (control == DashboardControl::Quit) {
            quitRequested = true;
            logger.Log(core::LogLevel::Info, "openvr-dashboard",
                       "Intentional receiver shutdown requested from the dashboard.");
            return;
        }
        switch (control) {
            case DashboardControl::ToggleVisible:
                pendingRadialMenuSelection.action = desiredVisible
                    ? phonecast::vr::RadialMenuAction::ToggleVisible
                    : phonecast::vr::RadialMenuAction::ShowBesideDashboard;
                break;
            case DashboardControl::HeadLocked:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::HeadLocked;
                break;
            case DashboardControl::WorldLocked:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::WorldLocked;
                break;
            case DashboardControl::PhoneTracked:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::PhoneTracked;
                break;
            case DashboardControl::LeftControllerLocked:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::LeftControllerLocked;
                break;
            case DashboardControl::RightControllerLocked:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::RightControllerLocked;
                break;
            case DashboardControl::OpenSettings:
                pendingRadialMenuSelection.action = phonecast::vr::RadialMenuAction::OpenSettings;
                break;
            case DashboardControl::AndroidBack:
            case DashboardControl::Quit:
                return;
        }
        pendingRadialMenuSelection.hand =
            currentSettings.placementMode == phonecast::vr::PlacementMode::RightControllerLocked
                ? phonecast::vr::GlanceInput::RightController
                : phonecast::vr::GlanceInput::LeftController;
        hasPendingRadialMenuSelection = true;
        logger.Log(core::LogLevel::Info, "openvr-dashboard",
                   "Dashboard control selected.");
    }

    vr::TrackedDeviceIndex_t DeviceForMode(phonecast::vr::PlacementMode mode) const {
        if (mode == phonecast::vr::PlacementMode::PhoneTracked)
            return vr::k_unTrackedDeviceIndexInvalid;
        if (mode == phonecast::vr::PlacementMode::HeadLocked)
            return vr::k_unTrackedDeviceIndex_Hmd;
        const auto role = mode == phonecast::vr::PlacementMode::LeftControllerLocked
            ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
        return system->GetTrackedDeviceIndexForControllerRole(role);
    }

    bool DevicePose(vr::TrackedDeviceIndex_t device, vr::HmdMatrix34_t& pose) const {
        if (device == vr::k_unTrackedDeviceIndexInvalid || device >= vr::k_unMaxTrackedDeviceCount)
            return false;
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0F,
                                                poses, vr::k_unMaxTrackedDeviceCount);
        if (!poses[device].bPoseIsValid) return false;
        pose = poses[device].mDeviceToAbsoluteTracking;
        return true;
    }

    bool ControllerTransform(const phonecast::vr::OverlaySettings& settings,
                             vr::HmdMatrix34_t& absolute) const {
        const auto controller = DeviceForMode(settings.placementMode);
        const auto& calibration = ControllerFor(settings);
        vr::HmdMatrix34_t controllerPose{};
        if (!DevicePose(controller, controllerPose)) return false;

        if (calibration.orientation == phonecast::vr::ControllerOrientation::ControllerRelative) {
            absolute = Multiply(controllerPose, ControllerLocalTransform(calibration));
            return true;
        }
        if (calibration.orientation == phonecast::vr::ControllerOrientation::Wrist) {
            const float handYaw = settings.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked
                ? 20.0F : -20.0F;
            absolute = Multiply(controllerPose, ControllerLocalTransform(calibration, -55.0F, handYaw));
            return true;
        }

        vr::HmdMatrix34_t hmdPose{};
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmdPose)) return false;
        absolute = Multiply(controllerPose, ControllerLocalTransform(calibration));
        float normal[3]{hmdPose.m[0][3] - absolute.m[0][3],
                        hmdPose.m[1][3] - absolute.m[1][3],
                        hmdPose.m[2][3] - absolute.m[2][3]};
        if (calibration.orientation == phonecast::vr::ControllerOrientation::WorldUpright)
            normal[1] = 0.0F;
        const float normalLength = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                             normal[2] * normal[2]);
        if (normalLength < 0.001F) return false;
        for (float& component : normal) component /= normalLength;

        float right[3]{normal[2], 0.0F, -normal[0]};
        const float rightLength = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        if (rightLength < 0.001F) {
            right[0] = 1.0F;
            right[1] = 0.0F;
            right[2] = 0.0F;
        } else {
            right[0] /= rightLength;
            right[2] /= rightLength;
        }
        const float up[3]{normal[1] * right[2] - normal[2] * right[1],
                          normal[2] * right[0] - normal[0] * right[2],
                          normal[0] * right[1] - normal[1] * right[0]};
        for (std::size_t row = 0; row < 3; ++row) {
            absolute.m[row][0] = right[row];
            absolute.m[row][1] = up[row];
            absolute.m[row][2] = normal[row];
        }
        const float px = absolute.m[0][3], py = absolute.m[1][3], pz = absolute.m[2][3];
        absolute = Multiply(absolute, Rotation(calibration.tiltDegrees, calibration.yawDegrees));
        absolute.m[0][3] = px;
        absolute.m[1][3] = py;
        absolute.m[2][3] = pz;
        return true;
    }

    bool AbsoluteForSettings(const phonecast::vr::OverlaySettings& settings,
                             vr::HmdMatrix34_t& absolute) const {
        if (settings.placementMode == phonecast::vr::PlacementMode::PhoneTracked) {
            if (cameraWorker && cameraWorker->GetLatestPose(absolute)) {
                return true;
            }
            vr::HmdMatrix34_t devicePose{};
            if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, devicePose)) return false;
            absolute = Multiply(devicePose, LocalTransform(settings));
            return true;
        }
        if (settings.placementMode == phonecast::vr::PlacementMode::WorldLocked &&
            settings.worldTransformValid) {
            absolute = FromArray(settings.worldTransform);
            return true;
        }
        if (settings.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked ||
            settings.placementMode == phonecast::vr::PlacementMode::RightControllerLocked)
            return ControllerTransform(settings, absolute);
        vr::HmdMatrix34_t devicePose{};
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, devicePose)) return false;
        absolute = Multiply(devicePose, LocalTransform(settings));
        return true;
    }

    bool ApplySettings(phonecast::vr::OverlaySettings settings, std::string& error) {
        const bool dashboardPlacementChanged =
            settings.placementMode != currentSettings.placementMode;
        auto notificationTransform = vr::HmdMatrix34_t{{
            {1.0F, 0.0F, 0.0F, settings.notificationOffsetXMeters},
            {0.0F, 1.0F, 0.0F, settings.notificationOffsetYMeters},
            {0.0F, 0.0F, 1.0F, -settings.notificationDistanceMeters}}};
        if (!OverlayCall(overlayApi->SetOverlayWidthInMeters(
                             notificationOverlay, settings.notificationWidthMeters),
                         "Set notification width", error) ||
            !OverlayCall(overlayApi->SetOverlayTransformTrackedDeviceRelative(
                             notificationOverlay, vr::k_unTrackedDeviceIndex_Hmd,
                             &notificationTransform),
                         "Set notification transform", error)) return false;

        if (cameraWorker) {
            cameraWorker->SetActive(settings.placementMode == phonecast::vr::PlacementMode::PhoneTracked);
        }
        if (settings.placementMode != phonecast::vr::PlacementMode::PhoneTracked &&
            passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
            overlayApi->HideOverlay(passthroughOverlay);
        }

        const float placementScale = IsControllerMode(settings.placementMode)
            ? ControllerFor(settings).scale : 1.0F;
        if (!OverlayCall(overlayApi->SetOverlayWidthInMeters(overlay, settings.widthMeters * placementScale),
                         "SetOverlayWidthInMeters", error) ||
            !OverlayCall(overlayApi->SetOverlayAlpha(overlay, settings.alpha),
                         "SetOverlayAlpha", error)) return false;

        if (settings.placementMode == phonecast::vr::PlacementMode::WorldLocked) {
            if (!settings.worldTransformValid) {
                vr::HmdMatrix34_t hmdPose{};
                if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmdPose)) {
                    error = "Cannot create a world anchor because the HMD pose is unavailable.";
                    return false;
                }
                settings.worldTransform = ToArray(Multiply(hmdPose, LocalTransform(settings)));
                settings.worldTransformValid = true;
                pendingSettings = settings;
                hasPendingSettings = true;
            }
            auto transform = FromArray(settings.worldTransform);
            if (!OverlayCall(overlayApi->SetOverlayTransformAbsolute(
                                 overlay, vr::TrackingUniverseStanding, &transform),
                             "SetOverlayTransformAbsolute", error)) return false;
        } else if (settings.placementMode == phonecast::vr::PlacementMode::HeadLocked) {
            auto transform = LocalTransform(settings);
            if (!OverlayCall(overlayApi->SetOverlayTransformTrackedDeviceRelative(
                                 overlay, vr::k_unTrackedDeviceIndex_Hmd, &transform),
                             "SetOverlayTransformTrackedDeviceRelative", error)) return false;
        } else if (settings.placementMode == phonecast::vr::PlacementMode::PhoneTracked) {
            vr::HmdMatrix34_t transform{};
            float physicalWidthMeters = 0.0F;
            if (cameraWorker && cameraWorker->GetLatestPose(transform, &physicalWidthMeters)) {
                if (physicalWidthMeters > 0.0F) {
                    overlayApi->SetOverlayWidthInMeters(overlay, physicalWidthMeters);
                }
                if (!OverlayCall(overlayApi->SetOverlayTransformAbsolute(
                                     overlay, vr::TrackingUniverseStanding, &transform),
                                 "SetOverlayTransformAbsolute", error)) return false;
            } else {
                vr::HmdMatrix34_t hmdPose{};
                if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmdPose)) {
                    transform = Multiply(hmdPose, LocalTransform(settings));
                    if (!OverlayCall(overlayApi->SetOverlayTransformAbsolute(
                                         overlay, vr::TrackingUniverseStanding, &transform),
                                     "SetOverlayTransformAbsolute", error)) return false;
                }
            }
        } else {
            vr::HmdMatrix34_t transform{};
            if (!ControllerTransform(settings, transform)) {
                // Keep the visible panel at its last valid pose until tracking
                // becomes available; a transient tracking miss must not hide it.
                currentSettings = settings;
                if (dashboardPlacementChanged) RenderDashboard();
                error.clear();
                return true;
            }
            if (!OverlayCall(overlayApi->SetOverlayTransformAbsolute(
                                 overlay, vr::TrackingUniverseStanding, &transform),
                             "SetOverlayTransformAbsolute", error)) return false;
        }
        currentSettings = settings;
        if (startLocationPreviewVisible) UpdateStartLocationPreview(true);
        if (dashboardPlacementChanged) RenderDashboard();
        return true;
    }

    void UpdatePassthroughCutout(const vr::HmdMatrix34_t& phoneTransform, float physicalWidthMeters) {
        if (passthroughOverlay == vr::k_ulOverlayHandleInvalid || !cameraWorker) return;
        CameraFrameSnapshot camSnapshot{};
        if (!cameraWorker->GetLatestCameraFrame(camSnapshot) || camSnapshot.rgbaBuffer.empty() ||
            camSnapshot.width == 0 || camSnapshot.height == 0) {
            overlayApi->HideOverlay(passthroughOverlay);
            return;
        }

        float minX = static_cast<float>(camSnapshot.width) * 0.25F;
        float maxX = static_cast<float>(camSnapshot.width) * 0.75F;
        float minY = static_cast<float>(camSnapshot.height) * 0.25F;
        float maxY = static_cast<float>(camSnapshot.height) * 0.75F;

        if (camSnapshot.corners.valid) {
            minX = std::min({camSnapshot.corners.corners[0].x, camSnapshot.corners.corners[1].x,
                             camSnapshot.corners.corners[2].x, camSnapshot.corners.corners[3].x});
            maxX = std::max({camSnapshot.corners.corners[0].x, camSnapshot.corners.corners[1].x,
                             camSnapshot.corners.corners[2].x, camSnapshot.corners.corners[3].x});
            minY = std::min({camSnapshot.corners.corners[0].y, camSnapshot.corners.corners[1].y,
                             camSnapshot.corners.corners[2].y, camSnapshot.corners.corners[3].y});
            maxY = std::max({camSnapshot.corners.corners[0].y, camSnapshot.corners.corners[1].y,
                             camSnapshot.corners.corners[2].y, camSnapshot.corners.corners[3].y});
        }

        const float screenW = std::max(10.0F, maxX - minX);
        const float screenH = std::max(10.0F, maxY - minY);
        const float marginX = screenW * 0.40F;
        const float marginY = screenH * 0.45F;

        const int cropLeft = std::max(0, static_cast<int>(minX - marginX));
        const int cropRight = std::min(static_cast<int>(camSnapshot.width), static_cast<int>(maxX + marginX));
        const int cropTop = std::max(0, static_cast<int>(minY - marginY * 0.3F));
        const int cropBottom = std::min(static_cast<int>(camSnapshot.height), static_cast<int>(maxY + marginY));

        const int cropW = cropRight - cropLeft;
        const int cropH = cropBottom - cropTop;
        if (cropW <= 32 || cropH <= 32) {
            overlayApi->HideOverlay(passthroughOverlay);
            return;
        }

        std::vector<uint8_t> cutout(static_cast<std::size_t>(cropW) * cropH * 4U);
        constexpr int feather = 18;

        for (int y = 0; y < cropH; ++y) {
            const int srcY = cropTop + y;
            const uint8_t* srcRow = camSnapshot.rgbaBuffer.data() +
                                    (static_cast<std::size_t>(srcY) * camSnapshot.width + cropLeft) * 4U;
            uint8_t* dstRow = cutout.data() + (static_cast<std::size_t>(y) * cropW) * 4U;

            for (int x = 0; x < cropW; ++x) {
                const int distEdge = std::min({x, cropW - 1 - x, y, cropH - 1 - y});
                float alphaFactor = 1.0F;
                if (distEdge < feather) {
                    alphaFactor = static_cast<float>(distEdge) / static_cast<float>(feather);
                }

                dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 3] = static_cast<uint8_t>(255.0F * alphaFactor);
            }
        }

        overlayApi->SetOverlayRaw(passthroughOverlay, cutout.data(), cropW, cropH, 4);
        const float scaleFactor = static_cast<float>(cropW) / screenW;
        const float passthroughWidth = physicalWidthMeters > 0.0F ? (physicalWidthMeters * scaleFactor) : 0.12F;
        overlayApi->SetOverlayWidthInMeters(passthroughOverlay, passthroughWidth);
        overlayApi->SetOverlayTransformAbsolute(passthroughOverlay, vr::TrackingUniverseStanding, &phoneTransform);
        overlayApi->ShowOverlay(passthroughOverlay);
    }

    void UpdateControllerPlacement() {
        if (grabbedDevice != vr::k_unTrackedDeviceIndexInvalid) return;
        if (currentSettings.placementMode == phonecast::vr::PlacementMode::PhoneTracked) {
            if (!cameraWorker) {
                if (passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
                    overlayApi->HideOverlay(passthroughOverlay);
                }
                return;
            }
            vr::HmdMatrix34_t transform{};
            float physicalWidthMeters = 0.0F;
            if (cameraWorker->GetLatestPose(transform, &physicalWidthMeters)) {
                if (physicalWidthMeters > 0.0F) {
                    overlayApi->SetOverlayWidthInMeters(overlay, physicalWidthMeters);
                }
                overlayApi->SetOverlayTransformAbsolute(overlay, vr::TrackingUniverseStanding, &transform);
                UpdatePassthroughCutout(transform, physicalWidthMeters);
            } else {
                if (passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
                    overlayApi->HideOverlay(passthroughOverlay);
                }
            }
            return;
        }
        if (passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
            overlayApi->HideOverlay(passthroughOverlay);
        }
        if (!IsControllerMode(currentSettings.placementMode)) return;
        vr::HmdMatrix34_t transform{};
        if (ControllerTransform(currentSettings, transform))
            overlayApi->SetOverlayTransformAbsolute(overlay, vr::TrackingUniverseStanding, &transform);
    }

    void PublishCalibration() {
        pendingSettings = currentSettings;
        hasPendingSettings = true;
        calibrationDirty = false;
    }

    struct HandActions {
        vr::VRActionHandle_t glance{vr::k_ulInvalidActionHandle};
        vr::VRActionHandle_t calibrate{vr::k_ulInvalidActionHandle};
        vr::VRActionHandle_t grip{vr::k_ulInvalidActionHandle};
        vr::VRActionHandle_t trigger{vr::k_ulInvalidActionHandle};
        vr::VRActionHandle_t axis{vr::k_ulInvalidActionHandle};
    };

    struct HandInput {
        bool available{false};
        bool glanceDown{false};
        bool glancePressed{false};
        bool glanceReleased{false};
        bool calibratePressed{false};
        bool grip{false};
        bool trigger{false};
        float x{0.0F};
        float y{0.0F};
    };

    struct HoldState {
        bool tracking{false};
        std::chrono::steady_clock::time_point started{};
    };

    bool ResolveAction(const char* name, vr::VRActionHandle_t& handle) {
        const auto result = inputApi->GetActionHandle(name, &handle);
        if (result == vr::VRInputError_None) return true;
        logger.Log(core::LogLevel::Warning, "openvr-input",
                   std::string("GetActionHandle failed for ") + name + " (" +
                   std::to_string(static_cast<int>(result)) + ").");
        return false;
    }

    bool InitializeControllerInput() {
        inputApi = vr::VRInput();
        if (inputApi == nullptr) {
            logger.Log(core::LogLevel::Warning, "openvr-input", "IVRInput is unavailable; controller shortcuts are disabled.");
            return false;
        }
        std::error_code pathError;
        const auto executable = std::filesystem::absolute(executablePath, pathError);
        const auto manifest = executable.parent_path() / "phonecast-actions.json";
        if (pathError || !std::filesystem::exists(manifest)) {
            logger.Log(core::LogLevel::Warning, "openvr-input", "phonecast-actions.json was not found beside the executable.");
            return false;
        }
        auto result = inputApi->SetActionManifestPath(manifest.string().c_str());
        if (result != vr::VRInputError_None) {
            logger.Log(core::LogLevel::Warning, "openvr-input",
                       "SetActionManifestPath failed (" + std::to_string(static_cast<int>(result)) + ").");
            return false;
        }
        result = inputApi->GetActionSetHandle("/actions/phonecast", &actionSet);
        if (result != vr::VRInputError_None) {
            logger.Log(core::LogLevel::Warning, "openvr-input",
                       "GetActionSetHandle failed (" + std::to_string(static_cast<int>(result)) + ").");
            return false;
        }
        const bool resolved =
            ResolveAction("/actions/phonecast/in/glance_left", leftActions.glance) &&
            ResolveAction("/actions/phonecast/in/glance_right", rightActions.glance) &&
            ResolveAction("/actions/phonecast/in/calibrate_left", leftActions.calibrate) &&
            ResolveAction("/actions/phonecast/in/calibrate_right", rightActions.calibrate) &&
            ResolveAction("/actions/phonecast/in/grip_left", leftActions.grip) &&
            ResolveAction("/actions/phonecast/in/grip_right", rightActions.grip) &&
            ResolveAction("/actions/phonecast/in/trigger_left", leftActions.trigger) &&
            ResolveAction("/actions/phonecast/in/trigger_right", rightActions.trigger) &&
            ResolveAction("/actions/phonecast/in/axis_left", leftActions.axis) &&
            ResolveAction("/actions/phonecast/in/axis_right", rightActions.axis);
        if (!resolved) return false;
        logger.Log(core::LogLevel::Info, "openvr-input", "Explicit SteamVR Input actions initialized.");
        return true;
    }

    bool ReadDigital(vr::VRActionHandle_t action, bool& state, bool& rising,
                     bool& falling) const {
        vr::InputDigitalActionData_t data{};
        const auto result = inputApi->GetDigitalActionData(
            action, &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
        if (result != vr::VRInputError_None || !data.bActive) return false;
        state = data.bState;
        rising = data.bChanged && data.bState;
        falling = data.bChanged && !data.bState;
        return true;
    }

    HandInput ReadHandInput(const HandActions& actions) const {
        HandInput input{};
        bool ignoredRising = false;
        bool ignoredFalling = false;
        bool calibrate = false;
        const bool haveGlance = ReadDigital(actions.glance, input.glanceDown,
                                            input.glancePressed, input.glanceReleased);
        const bool haveCalibrate = ReadDigital(actions.calibrate, calibrate,
                                               input.calibratePressed, ignoredFalling);
        const bool haveGrip = ReadDigital(actions.grip, input.grip, ignoredRising, ignoredFalling);
        const bool haveTrigger = ReadDigital(actions.trigger, input.trigger, ignoredRising, ignoredFalling);
        vr::InputAnalogActionData_t axis{};
        const auto axisResult = inputApi->GetAnalogActionData(
            actions.axis, &axis, sizeof(axis), vr::k_ulInvalidInputValueHandle);
        const bool haveAxis = axisResult == vr::VRInputError_None && axis.bActive;
        if (haveAxis) {
            input.x = axis.x;
            input.y = axis.y;
        }
        input.available = haveGlance || haveCalibrate || haveGrip || haveTrigger || haveAxis;
        return input;
    }

    bool PollExplicitInput(HandInput& left, HandInput& right) {
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = actionSet;
        active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        // PhoneCast is an overlay rather than the focused scene application. A
        // normal-priority action set is therefore suppressed by the active game,
        // even though its manifest and bindings load successfully. Request the
        // runtime's overlay-global range so the configured PhoneCast controls can
        // be observed while a scene application owns input focus.
        active.nPriority = vr::k_nActionSetOverlayGlobalPriorityMin;
        const auto result = inputApi->UpdateActionState(&active, sizeof(active), 1);
        if (result != vr::VRInputError_None) {
            if (!actionUpdateErrorReported) {
                logger.Log(core::LogLevel::Warning, "openvr-input",
                           "UpdateActionState failed (" + std::to_string(static_cast<int>(result)) + ").");
                actionUpdateErrorReported = true;
            }
            return false;
        }
        left = ReadHandInput(leftActions);
        right = ReadHandInput(rightActions);
        return true;
    }

    void PollLegacyInput(HandInput& left, HandInput& right) {
        const auto read = [this](vr::ETrackedControllerRole role, std::uint64_t& previous) {
            HandInput input{};
            const auto device = system->GetTrackedDeviceIndexForControllerRole(role);
            vr::VRControllerState_t state{};
            if (device == vr::k_unTrackedDeviceIndexInvalid ||
                !system->GetControllerState(device, &state, sizeof(state))) return input;
            const auto oldButtons = previous;
            const auto rising = state.ulButtonPressed & ~oldButtons;
            const auto falling = oldButtons & ~state.ulButtonPressed;
            previous = state.ulButtonPressed;
            input.available = true;
            const auto padMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
            input.glanceDown = (state.ulButtonPressed & padMask) != 0;
            input.glancePressed = (rising & padMask) != 0;
            input.glanceReleased = (falling & padMask) != 0;
            input.calibratePressed = (rising & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
            input.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
            input.trigger = (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
            input.x = state.rAxis[0].x;
            input.y = state.rAxis[0].y;
            return input;
        };
        left = read(vr::TrackedControllerRole_LeftHand, previousLeftControllerButtons);
        right = read(vr::TrackedControllerRole_RightHand, previousRightControllerButtons);
    }

    bool CaptureMenuTransform() {
        vr::HmdMatrix34_t hmd{};
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmd)) return false;
        const vr::HmdMatrix34_t local{{{1.0F, 0.0F, 0.0F, 0.0F},
                                       {0.0F, 1.0F, 0.0F, -0.04F},
                                       {0.0F, 0.0F, 1.0F, -0.85F}}};
        auto transform = Multiply(hmd, local);
        return overlayApi->SetOverlayTransformAbsolute(
            menuOverlay, vr::TrackingUniverseStanding, &transform) ==
            vr::VROverlayError_None;
    }

    bool WristFacesHead(bool left) const {
        vr::HmdMatrix34_t hmd{};
        vr::HmdMatrix34_t controller{};
        const auto mode = left ? phonecast::vr::PlacementMode::LeftControllerLocked
                               : phonecast::vr::PlacementMode::RightControllerLocked;
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmd) ||
            !DevicePose(DeviceForMode(mode), controller)) return false;

        float toHead[3]{hmd.m[0][3] - controller.m[0][3],
                        hmd.m[1][3] - controller.m[1][3],
                        hmd.m[2][3] - controller.m[2][3]};
        const float distance = std::sqrt(toHead[0] * toHead[0] + toHead[1] * toHead[1] +
                                         toHead[2] * toHead[2]);
        const float vertical = controller.m[1][3] - hmd.m[1][3];
        if (distance < 0.18F || distance > 0.90F || vertical < -0.75F || vertical > 0.20F)
            return false;
        for (float& component : toHead) component /= distance;

        // OpenVR controller poses use local +Y as the top/control-face normal.
        // A high dot product means the user has turned that face toward the HMD.
        const float facing = controller.m[0][1] * toHead[0] +
                             controller.m[1][1] * toHead[1] +
                             controller.m[2][1] * toHead[2];
        return facing >= 0.62F;
    }

    void UpdateGestureProgress(float progress) {
        if (gestureOverlay == vr::k_ulOverlayHandleInvalid) return;
        if (progress <= 0.0F) {
            if (gestureProgressVisible) overlayApi->HideOverlay(gestureOverlay);
            gestureProgressVisible = false;
            gestureProgressStep = -1;
            return;
        }
        const int step = std::max(1, std::min(kGestureProgressSteps,
            static_cast<int>(std::ceil(progress * kGestureProgressSteps))));
        if (step != gestureProgressStep) {
            auto image = MakeGestureProgressTexture(step);
            if (overlayApi->SetOverlayRaw(gestureOverlay, image.data(),
                                          kGestureTextureSize, kGestureTextureSize, 4) ==
                vr::VROverlayError_None) {
                gestureProgressStep = step;
            }
        }
        if (!gestureProgressVisible) {
            overlayApi->ShowOverlay(gestureOverlay);
            gestureProgressVisible = true;
        }
    }

    bool HandleWristMenuGesture() {
        phonecast::vr::WristMenuGestureObservation observation;
        observation.leftFacing = WristFacesHead(true);
        observation.rightFacing = WristFacesHead(false);
        observation.menuVisible = radialMenuVisible;
        observation.nowMilliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        const auto result = wristMenuGesture.Update(observation);
        UpdateGestureProgress(result.progress);
        if (result.action == phonecast::vr::WristMenuGestureAction::OpenLeft ||
            result.action == phonecast::vr::WristMenuGestureAction::OpenRight) {
            UpdateGestureProgress(0.0F);
            const bool left = result.action == phonecast::vr::WristMenuGestureAction::OpenLeft;
            OpenRadialMenu(left, false);
            logger.Log(core::LogLevel::Info, "openvr-menu",
                       left ? "Wrist gesture opened the control grid for the left hand."
                            : "Wrist gesture opened the control grid for the right hand.");
            return true;
        }
        return false;
    }

    int MenuGridCell(float mouseX, float mouseY) const {
        return phonecast::vr::ui::HitTestRadialMenu(mouseX, mouseY);
    }

    void AcceptMenuCell(int cell) {
        if (cell == kCloseMenuCell) {
            CloseRadialMenu();
            logger.Log(core::LogLevel::Info, "openvr-menu", "Control grid closed.");
            return;
        }
        if (cell < 0 || cell >= static_cast<int>(kMenuActions.size())) return;
        pendingRadialMenuSelection.action = kMenuActions[static_cast<std::size_t>(cell)];
        pendingRadialMenuSelection.hand = radialMenuLeft
            ? phonecast::vr::GlanceInput::LeftController
            : phonecast::vr::GlanceInput::RightController;
        hasPendingRadialMenuSelection = true;
        CloseRadialMenu();
        logger.Log(core::LogLevel::Info, "openvr-menu",
                   "Control-grid selection accepted from the controller laser.");
    }

    void RenderRadialMenu() {
        if (menuOverlay == vr::k_ulOverlayHandleInvalid) return;
        auto image = MakeRadialMenuTexture(radialMenuSelection, desiredVisible);
        const auto result = overlayApi->SetOverlayRaw(
            menuOverlay, image.data(), kMenuTextureWidth, kMenuTextureHeight, 4);
        if (result != vr::VROverlayError_None)
            logger.Log(core::LogLevel::Warning, "openvr-menu", "Failed to update the control-grid texture.");
    }

    void OpenRadialMenu(bool left, bool waitForRelease) {
        if (menuOverlay == vr::k_ulOverlayHandleInvalid) return;
        radialMenuLeft = left;
        radialMenuSelection = -1;
        radialAwaitRelease = waitForRelease;
        if (!CaptureMenuTransform()) {
            logger.Log(core::LogLevel::Warning, "openvr-menu",
                       "Could not place the control grid in front of the headset.");
            return;
        }
        radialMenuVisible = true;
        RenderRadialMenu();
        overlayApi->ShowOverlay(menuOverlay);
        logger.Log(core::LogLevel::Info, "openvr-menu",
                   "Phone-shaped control grid opened in front of the headset.");
    }

    void CloseRadialMenu() {
        if (menuOverlay != vr::k_ulOverlayHandleInvalid) overlayApi->HideOverlay(menuOverlay);
        radialMenuVisible = false;
        radialAwaitRelease = false;
    }

    int SettingsRow(float mouseX, float mouseY) const {
        const int x = static_cast<int>(mouseX);
        const int y = static_cast<int>(kSettingsTextureHeight - mouseY);
        if (settingsMenuView.title == "PHONECAST SETTINGS") {
            constexpr int firstCenter = 150;
            constexpr int rowStep = 78;
            for (int row = 0; row < 7; ++row) {
                const int center = firstCenter + row * rowStep;
                if (y >= center - 32 && y < center + 32 && x >= 16 && x < 496)
                    return row;
            }
            if (y >= 666 && y < 712 && x >= 16 && x < 496) return 10;
            constexpr std::array<int, 3> actionLeft{16, 174, 332};
            constexpr std::array<int, 3> actionWidth{142, 142, 164};
            for (int action = 0; action < 3; ++action) {
                if (x >= actionLeft[static_cast<std::size_t>(action)] &&
                    x < actionLeft[static_cast<std::size_t>(action)] +
                        actionWidth[static_cast<std::size_t>(action)] &&
                    y >= 730 && y < 808)
                    return action + 7;
            }
            return -1;
        }
        constexpr int rowHeight = 82;
        constexpr int firstRow = 126;
        for (std::size_t row = 0; row < settingsMenuView.labels.size(); ++row) {
            const int center = firstRow + static_cast<int>(row) * rowHeight;
            if (y >= center - 34 && y < center + 34) return static_cast<int>(row);
        }
        return -1;
    }

    bool IsSettingsGrabHandle(float mouseY) const {
        const int y = static_cast<int>(kSettingsTextureHeight - mouseY);
        return y >= static_cast<int>(kSettingsTextureHeight) - 70;
    }

    bool IsSettingsResizeHandle(float mouseX, float mouseY) const {
        const int x = static_cast<int>(mouseX);
        const int y = static_cast<int>(kSettingsTextureHeight - mouseY);
        return y >= static_cast<int>(kSettingsTextureHeight) - 70 &&
               x >= static_cast<int>(kSettingsTextureWidth) - 60;
    }

    void QueueSettingsLaserClick(float mouseX, float mouseY) {
        const int y = static_cast<int>(kSettingsTextureHeight - mouseY);
        // Header navigation (Back button on the left, Close button on the right)
        if (y >= 16 && y <= 86) {
            if (mouseX <= 72.0F && settingsMenuView.title != "PHONECAST SETTINGS") {
                settingsLaserInput = {phonecast::vr::SettingsMenuCommand::Back, 0.0F};
                return;
            }
            if (mouseX >= static_cast<float>(kSettingsTextureWidth) - 72.0F) {
                if (settingsMenuView.title == "PHONECAST SETTINGS") {
                    overlayApi->HideOverlay(settingsOverlay);
                    settingsMenuVisible = false;
                } else {
                    settingsLaserInput = {phonecast::vr::SettingsMenuCommand::Back, 0.0F};
                }
                return;
            }
        }

        const int row = SettingsRow(mouseX, mouseY);
        if (row < 0) return;
        settingsLaserTargetRow = row;
        const auto index = static_cast<std::size_t>(row);
        const bool adjustable = settingsMenuView.title != "PHONECAST SETTINGS" &&
                                settingsMenuView.title != "ABOUT PHONECAST" &&
                                index < settingsMenuView.values.size() &&
                                !settingsMenuView.values[index].empty() &&
                                settingsMenuView.values[index] != ">";
        const bool slider = index < settingsMenuView.normalizedValues.size() &&
                            settingsMenuView.normalizedValues[index] >= 0.0F;
        if (adjustable && mouseX < 72.0F) {
            settingsLaserInput = {phonecast::vr::SettingsMenuCommand::Decrease, 0.0F};
        } else if (adjustable && mouseX > static_cast<float>(kSettingsTextureWidth) - 72.0F) {
            settingsLaserInput = {phonecast::vr::SettingsMenuCommand::Increase, 0.0F};
        } else if (slider && mouseX >= 88.0F && mouseX <= 424.0F) {
            settingsLaserInput = {phonecast::vr::SettingsMenuCommand::SetNormalized,
                                  Clamp((mouseX - 88.0F) / 336.0F, 0.0F, 1.0F)};
        } else {
            settingsLaserInput = {phonecast::vr::SettingsMenuCommand::Activate, 0.0F};
        }
    }

    bool CaptureSettingsTransform() {
        if (settingsWorldTransformValid) {
            return overlayApi->SetOverlayTransformAbsolute(
                settingsOverlay, vr::TrackingUniverseStanding, &settingsWorldTransform) ==
                vr::VROverlayError_None;
        }
        vr::HmdMatrix34_t hmd{};
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmd)) return false;
        const vr::HmdMatrix34_t local{{{1.0F, 0.0F, 0.0F, 0.0F},
                                       {0.0F, 1.0F, 0.0F, -0.04F},
                                       {0.0F, 0.0F, 1.0F, -0.85F}}};
        settingsWorldTransform = Multiply(hmd, local);
        settingsWorldTransformValid = true;
        return overlayApi->SetOverlayTransformAbsolute(
            settingsOverlay, vr::TrackingUniverseStanding, &settingsWorldTransform) ==
            vr::VROverlayError_None;
    }

    void TriggerHaptic(vr::VROverlayHandle_t targetOverlay, float durationSeconds,
                       float frequency, float amplitude,
                       vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid) {
        if (overlayApi != nullptr && targetOverlay != vr::k_ulOverlayHandleInvalid) {
            overlayApi->TriggerLaserMouseHapticVibration(targetOverlay, durationSeconds, frequency, amplitude);
        }
        if (inputApi != nullptr && shortcutHaptic != vr::k_ulInvalidActionHandle) {
            inputApi->TriggerHapticVibrationAction(
                shortcutHaptic, 0.0F, durationSeconds, frequency, amplitude, vr::k_ulInvalidInputValueHandle);
        } else if (system != nullptr && device < vr::k_unMaxTrackedDeviceCount) {
            const auto us = static_cast<unsigned short>(
                std::max(0.0F, std::min(65535.0F, durationSeconds * 1e6F * amplitude)));
            if (us > 0) {
                system->TriggerHapticPulse(device, 0, us);
            }
        }
    }

    bool UploadCompositeTexture(std::string& error) {
        if (!hasFrame || compositeFrame.pixels.empty() || overlay == vr::k_ulOverlayHandleInvalid) return false;
#ifdef _WIN32
        if (!EnsureTexture(compositeFrame.width, compositeFrame.height, error)) return false;
        context->UpdateSubresource(texture, 0, nullptr, compositeFrame.pixels.data(),
                                   compositeFrame.width * 4U, 0);
        context->Flush();
        vr::Texture_t tex{texture, vr::TextureType_DirectX, vr::ColorSpace_Auto};
        return OverlayCall(overlayApi->SetOverlayTexture(overlay, &tex), "SetOverlayTexture", error);
#else
        if (vulkanTexture == nullptr) {
            error = "Vulkan texture is not initialized.";
            return false;
        }
        return vulkanTexture->Update(overlayApi, overlay, compositeFrame.pixels.data(),
                                     compositeFrame.width, compositeFrame.height, error);
#endif
    }

    void UpdateFooterHover(FooterHoverTarget hover) {
        if (!hasFrame || compositeFrame.pixels.empty() || compositeFrame.height <= kGrabHandleHeightPixels) return;
        const auto phoneHeight = compositeFrame.height - kGrabHandleHeightPixels;
        DrawGrabFooter(compositeFrame, phoneHeight, hover);
        std::string uploadError;
        UploadCompositeTexture(uploadError);
    }

    vr::TrackedDeviceIndex_t ResolvePointerDevice(
            vr::TrackedDeviceIndex_t device,
            vr::VROverlayHandle_t targetOverlay = vr::k_ulOverlayHandleInvalid) const {
        const auto valid = [&](vr::TrackedDeviceIndex_t candidate) {
            return candidate < vr::k_unMaxTrackedDeviceCount &&
                system->GetTrackedDeviceClass(candidate) == vr::TrackedDeviceClass_Controller;
        };
        if (valid(device)) return device;
        device = overlayApi->GetPrimaryDashboardDevice();
        if (valid(device)) return device;

        const auto testOverlay = targetOverlay != vr::k_ulOverlayHandleInvalid ? targetOverlay : overlay;
        vr::TrackedDeviceIndex_t bestCandidate = vr::k_unTrackedDeviceIndexInvalid;
        float bestDistance = std::numeric_limits<float>::max();

        if (testOverlay != vr::k_ulOverlayHandleInvalid) {
            for (const auto role : {vr::TrackedControllerRole_RightHand, vr::TrackedControllerRole_LeftHand}) {
                const auto candidate = system->GetTrackedDeviceIndexForControllerRole(role);
                if (!valid(candidate)) continue;
                vr::HmdMatrix34_t pose{};
                if (!DevicePose(candidate, pose)) continue;

                vr::VROverlayIntersectionParams_t params{};
                params.eOrigin = vr::TrackingUniverseStanding;
                params.vSource = {pose.m[0][3], pose.m[1][3], pose.m[2][3]};
                params.vDirection = {-pose.m[0][2], -pose.m[1][2], -pose.m[2][2]};
                vr::VROverlayIntersectionResults_t results{};
                if (overlayApi->ComputeOverlayIntersection(testOverlay, &params, &results)) {
                    if (results.fDistance < bestDistance) {
                        bestDistance = results.fDistance;
                        bestCandidate = candidate;
                    }
                }
            }
        }
        if (bestCandidate != vr::k_unTrackedDeviceIndexInvalid) return bestCandidate;

        vr::HmdMatrix34_t overlayPose{};
        if (testOverlay != vr::k_ulOverlayHandleInvalid &&
            AbsoluteForSettings(currentSettings, overlayPose)) {
            float bestAlignment = 0.0F;
            for (const auto role : {vr::TrackedControllerRole_RightHand, vr::TrackedControllerRole_LeftHand}) {
                const auto candidate = system->GetTrackedDeviceIndexForControllerRole(role);
                if (!valid(candidate)) continue;
                vr::HmdMatrix34_t pose{};
                if (!DevicePose(candidate, pose)) continue;

                const float posX = pose.m[0][3];
                const float posY = pose.m[1][3];
                const float posZ = pose.m[2][3];
                const float fwdX = -pose.m[0][2];
                const float fwdY = -pose.m[1][2];
                const float fwdZ = -pose.m[2][2];

                float toTargetX = overlayPose.m[0][3] - posX;
                float toTargetY = overlayPose.m[1][3] - posY;
                float toTargetZ = overlayPose.m[2][3] - posZ;
                const float dist = std::sqrt(toTargetX * toTargetX + toTargetY * toTargetY + toTargetZ * toTargetZ);
                if (dist > 0.001F) {
                    toTargetX /= dist;
                    toTargetY /= dist;
                    toTargetZ /= dist;
                    const float alignment = fwdX * toTargetX + fwdY * toTargetY + fwdZ * toTargetZ;
                    if (alignment > bestAlignment) {
                        bestAlignment = alignment;
                        bestCandidate = candidate;
                    }
                }
            }
        }
        if (bestCandidate != vr::k_unTrackedDeviceIndexInvalid) return bestCandidate;

        for (const auto role : {vr::TrackedControllerRole_RightHand, vr::TrackedControllerRole_LeftHand}) {
            const auto candidate = system->GetTrackedDeviceIndexForControllerRole(role);
            if (valid(candidate)) return candidate;
        }
        return vr::k_unTrackedDeviceIndexInvalid;
    }

    void BeginSettingsGrab(vr::TrackedDeviceIndex_t device) {
        device = ResolvePointerDevice(device, settingsOverlay);
        vr::HmdMatrix34_t controllerPose{};
        if (!DevicePose(device, controllerPose) || !settingsWorldTransformValid) return;
        settingsGrabRelative = Multiply(InverseRigid(controllerPose), settingsWorldTransform);
        if (overlayApi->SetOverlayTransformTrackedDeviceRelative(
                settingsOverlay, device, &settingsGrabRelative) == vr::VROverlayError_None)
        {
            settingsGrabbedDevice = device;
            settingsDepthScroll = {};
        }
    }

    void EndSettingsGrab(vr::TrackedDeviceIndex_t device) {
        if (settingsGrabbedDevice == vr::k_unTrackedDeviceIndexInvalid) return;
        device = settingsGrabbedDevice;
        vr::HmdMatrix34_t controllerPose{};
        if (DevicePose(device, controllerPose))
            settingsWorldTransform = Multiply(controllerPose, settingsGrabRelative);
        settingsWorldTransformValid = true;
        overlayApi->SetOverlayTransformAbsolute(
            settingsOverlay, vr::TrackingUniverseStanding, &settingsWorldTransform);
        LogDragScroll("Settings", settingsDepthScroll);
        settingsDepthScroll = {};
        settingsGrabbedDevice = vr::k_unTrackedDeviceIndexInvalid;
    }

    bool HandleSettingsMenu(const HandInput& left, const HandInput& right) {
        if (!settingsMenuVisible) return false;
        const auto& active = settingsMenuLeft ? left : right;
        if (!hasPendingSettingsMenuCommand &&
            (left.calibratePressed || right.calibratePressed)) {
            pendingSettingsMenuInput = {phonecast::vr::SettingsMenuCommand::Back, 0.0F};
            hasPendingSettingsMenuCommand = true;
        } else if (!hasPendingSettingsMenuCommand && active.glancePressed) {
            pendingSettingsMenuInput = {phonecast::vr::SettingsMenuCommand::Activate, 0.0F};
            hasPendingSettingsMenuCommand = true;
        }
        const float magnitude = std::sqrt(active.x * active.x + active.y * active.y);
        if (magnitude < 0.30F) {
            settingsAxisLatched = false;
        } else if (!settingsAxisLatched && !hasPendingSettingsMenuCommand) {
            if (std::fabs(active.y) >= std::fabs(active.x))
                pendingSettingsMenuInput = {
                    active.y > 0.0F ? phonecast::vr::SettingsMenuCommand::PreviousItem
                                    : phonecast::vr::SettingsMenuCommand::NextItem,
                    0.0F};
            else
                pendingSettingsMenuInput = {
                    active.x > 0.0F ? phonecast::vr::SettingsMenuCommand::Increase
                                    : phonecast::vr::SettingsMenuCommand::Decrease,
                    0.0F};
            hasPendingSettingsMenuCommand = true;
            settingsAxisLatched = true;
        }
        return true;
    }

    bool HandleRadialMenu(const HandInput& left, const HandInput& right) {
        if (!radialMenuVisible) return false;
        const auto& active = radialMenuLeft ? left : right;
        if (active.glanceReleased) radialAwaitRelease = false;
        if (left.calibratePressed || right.calibratePressed) {
            CloseRadialMenu();
            logger.Log(core::LogLevel::Info, "openvr-menu", "Control grid cancelled.");
        } else if (!radialAwaitRelease && active.glancePressed && radialMenuSelection >= 0) {
            AcceptMenuCell(radialMenuSelection);
        }
        return true;
    }

    bool HandleGlancePress(bool left, const HandInput& input, HoldState& hold) {
        if (input.glancePressed) {
            hold.tracking = true;
            hold.started = std::chrono::steady_clock::now();
        }
        if (hold.tracking && input.glanceDown &&
            std::chrono::steady_clock::now() - hold.started >=
                std::chrono::milliseconds(currentSettings.radialLongPressMilliseconds)) {
            hold.tracking = false;
            OpenRadialMenu(left, true);
            return true;
        }
        if (hold.tracking && input.glanceReleased) {
            hold.tracking = false;
            pendingGlanceInput = left
                ? phonecast::vr::GlanceInput::LeftController
                : phonecast::vr::GlanceInput::RightController;
            hasPendingGlanceInput = true;
        }
        return false;
    }

    void PollControllerCalibration() {
        // Calibration is exclusively edited through transactional Settings.
        // Legacy menu-button polling must not activate it or consume pointer drags.
        if (!kEnableExperimentalGestureControls) return;
        HandInput left{};
        HandInput right{};
        if (!explicitInputReady || !PollExplicitInput(left, right)) PollLegacyInput(left, right);

        if (HandleSettingsMenu(left, right)) return;
        if (kEnableExperimentalGestureControls &&
            !settingsMenuVisible && HandleWristMenuGesture()) return;
        if (HandleRadialMenu(left, right)) return;
        if (kEnableExperimentalGestureControls && !calibrationActive) {
            if (HandleGlancePress(true, left, leftHold)) return;
            if (HandleGlancePress(false, right, rightHold)) return;
        }
        const bool selectLeft = left.calibratePressed &&
            currentSettings.placementMode != phonecast::vr::PlacementMode::LeftControllerLocked;
        const bool selectRight = right.calibratePressed &&
            currentSettings.placementMode != phonecast::vr::PlacementMode::RightControllerLocked;
        if (selectLeft || selectRight) {
            if (calibrationDirty) PublishCalibration();
            currentSettings.placementMode = selectLeft
                ? phonecast::vr::PlacementMode::LeftControllerLocked
                : phonecast::vr::PlacementMode::RightControllerLocked;
            calibrationActive = true;
            std::string ignored;
            ApplySettings(currentSettings, ignored);
            PublishCalibration();
            logger.Log(core::LogLevel::Info, "openvr",
                       selectLeft ? "Left-controller calibration enabled."
                                  : "Right-controller calibration enabled.");
            return;
        }
        if (!IsControllerMode(currentSettings.placementMode)) {
            if (calibrationDirty) PublishCalibration();
            calibrationActive = false;
            return;
        }

        const bool leftSelected =
            currentSettings.placementMode == phonecast::vr::PlacementMode::LeftControllerLocked;
        const auto& state = leftSelected ? left : right;
        if (!state.available) return;

        if (state.calibratePressed) {
            calibrationActive = !calibrationActive;
            if (!calibrationActive && calibrationDirty) PublishCalibration();
            logger.Log(core::LogLevel::Info, "openvr", calibrationActive
                ? "Controller calibration enabled: axis moves; grip+axis rotates; trigger+axis scales/distances; pad click changes orientation."
                : "Controller calibration disabled and saved.");
        }
        if (!calibrationActive) return;

        auto& calibration = ControllerFor(currentSettings);
        if (state.glancePressed) {
            if (state.grip) {
                calibration = {};
                logger.Log(core::LogLevel::Info, "openvr", "Controller calibration reset for the active hand.");
            } else {
                calibration.orientation = static_cast<phonecast::vr::ControllerOrientation>(
                    (static_cast<int>(calibration.orientation) + 1) % 4);
                logger.Log(core::LogLevel::Info, "openvr", "Controller orientation mode changed.");
            }
            calibrationDirty = true;
            std::string ignored;
            ApplySettings(currentSettings, ignored);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastCalibrationAdjustment < std::chrono::milliseconds(40)) return;
        lastCalibrationAdjustment = now;
        float x = state.x;
        float y = state.y;
        if (std::fabs(x) < 0.18F) x = 0.0F;
        if (std::fabs(y) < 0.18F) y = 0.0F;
        if (x == 0.0F && y == 0.0F) {
            if (calibrationDirty && !state.grip && !state.trigger && !state.glancePressed)
                PublishCalibration();
            return;
        }
        if (state.grip) {
            calibration.yawDegrees = Clamp(calibration.yawDegrees + x * 2.0F, -180.0F, 180.0F);
            calibration.tiltDegrees = Clamp(calibration.tiltDegrees + y * 2.0F, -180.0F, 180.0F);
        } else if (state.trigger) {
            calibration.scale = Clamp(calibration.scale + x * 0.02F, 0.25F, 3.0F);
            calibration.distanceMeters = Clamp(calibration.distanceMeters - y * 0.005F, 0.05F, 2.0F);
        } else {
            calibration.lateralMeters = Clamp(calibration.lateralMeters + x * 0.005F, -1.0F, 1.0F);
            calibration.heightMeters = Clamp(calibration.heightMeters + y * 0.005F, -1.0F, 1.0F);
        }
        calibrationDirty = true;
        std::string ignored;
        ApplySettings(currentSettings, ignored);
    }

    void QueuePointer(core::PointerEvent event) {
        // Keep input bounded if the application stalls; preserve down/up and drop
        // only old motion samples.
        if (pendingPointerEvents.size() >= 64 && event.type == core::PointerEvent::Type::Move) {
            auto found = std::find_if(pendingPointerEvents.begin(), pendingPointerEvents.end(),
                [](const core::PointerEvent& queued) {
                    return queued.type == core::PointerEvent::Type::Move;
                });
            if (found != pendingPointerEvents.end()) pendingPointerEvents.erase(found);
        }
        if (pendingPointerEvents.size() < 64) pendingPointerEvents.push_back(event);
    }

    void UpdateDragDepth() {
        const auto update = [&](vr::TrackedDeviceIndex_t device, vr::VROverlayHandle_t handle,
                                vr::HmdMatrix34_t& relative) {
            if (device == vr::k_unTrackedDeviceIndexInvalid) return;
            vr::HmdMatrix34_t pose{}, head{};
            if (!DevicePose(device, pose)) {
                if (handle == overlay) EndGrab(device);
                else EndSettingsGrab(device);
                return;
            }
            const auto tracked = Multiply(pose, relative);
            if (handle == overlay) grabLastAbsolute = tracked;
            else settingsWorldTransform = tracked;
            if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, head)) return;
            auto absolute = Multiply(pose, relative);
            float direction[3]{};
            float distanceSquared = 0;
            for (int row = 0; row < 3; ++row) {
                direction[row] = absolute.m[row][3] - head.m[row][3];
                distanceSquared += direction[row] * direction[row];
            }
            const float distance = std::sqrt(distanceSquared);
            if (distance < 0.001F) return;
            auto& scroll = handle == overlay ? phoneDepthScroll : settingsDepthScroll;
            const float next = scroll.TakeDistance(distance);
            if (next == distance) return;
            for (int row = 0; row < 3; ++row)
                absolute.m[row][3] += direction[row] * ((next - distance) / distance);
            relative = Multiply(InverseRigid(pose), absolute);
            if (handle == overlay) grabLastAbsolute = absolute;
            else settingsWorldTransform = absolute;
            overlayApi->SetOverlayTransformTrackedDeviceRelative(handle, device, &relative);
        };
        update(grabbedDevice, overlay, grabRelative);
        update(settingsGrabbedDevice, settingsOverlay, settingsGrabRelative);
    }

    void QueueDragScroll(const vr::VREvent_t& event, bool settings) {
        const auto owner = settings ? settingsGrabbedDevice : grabbedDevice;
        if (owner == vr::k_unTrackedDeviceIndexInvalid) return;
        auto& scroll = settings ? settingsDepthScroll : phoneDepthScroll;
        const auto activeDevice = ResolvePointerDevice(event.trackedDeviceIndex, settings ? settingsOverlay : overlay);
        scroll.Add(event.data.scroll.ydelta, event.eventType == vr::VREvent_ScrollSmooth,
                   activeDevice == owner || activeDevice == vr::k_unTrackedDeviceIndexInvalid);
    }

    void LogDragScroll(const char* panel, const phonecast::vr::PanelDepthScroll& scroll) {
        logger.Log(core::LogLevel::Info, "openvr-depth", std::string(panel) +
            " drag scroll: smooth=" + std::to_string(scroll.smoothEvents) +
            " discrete=" + std::to_string(scroll.discreteEvents) +
            " positive=" + std::to_string(scroll.positiveEvents) +
            " negative=" + std::to_string(scroll.negativeEvents));
    }

    void BeginResize(float overlayX) {
        resizing = true;
        resizeStartX = overlayX;
        resizeStartWidthMeters = currentSettings.widthMeters;
        logger.Log(core::LogLevel::Info, "openvr", "Overlay resize started.");
    }

    void UpdateResize(float overlayX) {
        if (!resizing) return;
        const float pixelWidth = static_cast<float>(std::max<std::uint32_t>(1U, compositeFrame.width));
        const float delta = (overlayX - resizeStartX) / pixelWidth;
        currentSettings.widthMeters = Clamp(
            resizeStartWidthMeters * (1.0F + delta * 2.0F), 0.10F, 2.0F);
        std::string ignored;
        ApplySettings(currentSettings, ignored);
    }

    void EndResize() {
        if (!resizing) return;
        resizing = false;
        pendingSettings = currentSettings;
        hasPendingSettings = true;
        logger.Log(core::LogLevel::Info, "openvr", "Overlay resize ended and was saved.");
    }

    void BeginSettingsResize(float overlayX) {
        settingsResizing = true;
        settingsResizeStartX = overlayX;
        overlayApi->GetOverlayWidthInMeters(settingsOverlay, &settingsResizeStartWidthMeters);
        logger.Log(core::LogLevel::Info, "openvr", "Settings overlay resize started.");
    }

    void UpdateSettingsResize(float overlayX) {
        if (!settingsResizing) return;
        const float delta = (overlayX - settingsResizeStartX) / static_cast<float>(kSettingsTextureWidth);
        const float newWidth = Clamp(settingsResizeStartWidthMeters * (1.0F + delta * 2.0F), 0.20F, 1.20F);
        overlayApi->SetOverlayWidthInMeters(settingsOverlay, newWidth);
    }

    void EndSettingsResize() {
        if (!settingsResizing) return;
        settingsResizing = false;
        logger.Log(core::LogLevel::Info, "openvr", "Settings overlay resize ended.");
    }

    void BeginGrab(vr::TrackedDeviceIndex_t device) {
        device = ResolvePointerDevice(device, overlay);
        vr::HmdMatrix34_t controllerPose{};
        vr::HmdMatrix34_t overlayPose{};
        if (!DevicePose(device, controllerPose) || !AbsoluteForSettings(currentSettings, overlayPose)) return;
        grabLastAbsolute = overlayPose;
        grabRelative = Multiply(InverseRigid(controllerPose), overlayPose);
        std::string ignored;
        if (OverlayCall(overlayApi->SetOverlayTransformTrackedDeviceRelative(
                            overlay, device, &grabRelative), "begin overlay grab", ignored)) {
            grabbedDevice = device;
            phoneDepthScroll = {};
            logger.Log(core::LogLevel::Info, "openvr", "Overlay grab started.");
        }
    }

    void EndGrab(vr::TrackedDeviceIndex_t device) {
        if (grabbedDevice == vr::k_unTrackedDeviceIndexInvalid) return;
        (void)device;
        vr::HmdMatrix34_t controllerPose{};
        {
            const auto absolute = DevicePose(grabbedDevice, controllerPose)
                ? Multiply(controllerPose, grabRelative) : grabLastAbsolute;
            currentSettings.placementMode = phonecast::vr::PlacementMode::WorldLocked;
            currentSettings.worldTransform = ToArray(absolute);
            currentSettings.worldTransformValid = true;
            overlayApi->SetOverlayTransformAbsolute(overlay, vr::TrackingUniverseStanding, &absolute);
            pendingSettings = currentSettings;
            hasPendingSettings = true;
            logger.Log(core::LogLevel::Info, "openvr", "Overlay grab ended; placement is world-locked.");
        }
        LogDragScroll("Phone", phoneDepthScroll);
        phoneDepthScroll = {};
        grabbedDevice = vr::k_unTrackedDeviceIndexInvalid;
    }

#ifdef _WIN32
    bool EnsureTexture(std::uint32_t width, std::uint32_t height, std::string& error) {
        if (texture != nullptr && textureWidth == width && textureHeight == height) return true;
        if (texture != nullptr) {
            texture->Release();
            texture = nullptr;
        }
        textureWidth = 0;
        textureHeight = 0;
        if (device == nullptr) {
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            D3D_FEATURE_LEVEL selected{};
            HRESULT result = E_FAIL;
            int32_t adapterIndex = -1;
            if (system != nullptr) system->GetDXGIOutputInfo(&adapterIndex);
            IDXGIFactory1* factory = nullptr;
            IDXGIAdapter1* adapter = nullptr;
            if (adapterIndex >= 0 && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter))) {
                result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                           nullptr, 0, D3D11_SDK_VERSION, &device,
                                           &selected, &context);
            }
            if (adapter != nullptr) adapter->Release();
            if (factory != nullptr) factory->Release();
            if (FAILED(result)) {
                logger.Log(core::LogLevel::Warning, "openvr",
                           "Could not create D3D11 on SteamVR's compositor adapter; trying the default adapter.");
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           nullptr, 0, D3D11_SDK_VERSION, &device,
                                           &selected, &context);
            }
            if (FAILED(result)) {
                error = "D3D11CreateDevice failed (HRESULT " + std::to_string(result) + ").";
                return false;
            }
            logger.Log(core::LogLevel::Info, "openvr",
                       "Created the overlay texture device on DXGI adapter " +
                       std::to_string(adapterIndex) + '.');
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const HRESULT result = device->CreateTexture2D(&description, nullptr, &texture);
        if (FAILED(result)) {
            error = "Creating the OpenVR D3D11 texture failed (HRESULT " +
                    std::to_string(result) + ").";
            return false;
        }
        textureWidth = width;
        textureHeight = height;
        return true;
    }
#endif

    core::ILogger& logger;
    std::string executablePath;
    vr::IVRSystem* system{nullptr};
    vr::IVROverlay* overlayApi{nullptr};
    vr::IVRInput* inputApi{nullptr};
    vr::VRActionSetHandle_t actionSet{vr::k_ulInvalidActionSetHandle};
    HandActions leftActions{};
    HandActions rightActions{};
    bool explicitInputReady{false};
    bool actionUpdateErrorReported{false};
    vr::VROverlayHandle_t overlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t menuOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t gestureOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t settingsOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t notificationOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t passthroughOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t dashboardOverlay{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t dashboardThumbnail{vr::k_ulOverlayHandleInvalid};
    std::deque<std::string> pendingTextQueue;
    std::mutex textQueueMutex;

    void QueueTextEntered(std::string text) {
        std::lock_guard<std::mutex> lock(textQueueMutex);
        if (pendingTextQueue.size() < 16) {
            pendingTextQueue.push_back(std::move(text));
        }
    }
    bool startLocationPreviewVisible{false};
    vr::HmdMatrix34_t startPreviewHead{};
    bool gestureProgressVisible{false};
    int gestureProgressStep{-1};
    bool settingsMenuVisible{false};
    bool notificationVisible{false};
    bool notificationPreviewVisible{false};
    bool notificationOpenRequested{false};
    std::uint64_t currentNotificationActionToken{};
    std::uint64_t pendingNotificationActionToken{};
    bool hideRequested{false};
    bool quitRequested{false};
    bool settingsMenuLeft{true};
    phonecast::vr::SettingsMenuView settingsMenuView{};
    int settingsLaserTargetRow{-1};
    phonecast::vr::SettingsMenuInput settingsLaserInput{};
    bool settingsAxisLatched{false};
    phonecast::vr::SettingsMenuInput pendingSettingsMenuInput{
        phonecast::vr::SettingsMenuCommand::Back, 0.0F};
    bool hasPendingSettingsMenuCommand{false};
    vr::HmdMatrix34_t settingsWorldTransform{};
    bool settingsWorldTransformValid{false};
    vr::TrackedDeviceIndex_t settingsGrabbedDevice{vr::k_unTrackedDeviceIndexInvalid};
    vr::HmdMatrix34_t settingsGrabRelative{};
    bool settingsResizing{false};
    float settingsResizeStartX{0.0F};
    float settingsResizeStartWidthMeters{0.34F};
    bool radialMenuVisible{false};
    bool radialMenuLeft{true};
    bool radialAwaitRelease{false};
    int radialMenuSelection{-1};
    phonecast::vr::RadialMenuSelection pendingRadialMenuSelection{};
    bool hasPendingRadialMenuSelection{false};
    HoldState leftHold{};
    HoldState rightHold{};
    phonecast::vr::WristMenuGesture wristMenuGesture{};
    vr::VRActionSetHandle_t shortcutSet{vr::k_ulInvalidActionSetHandle};
    phonecast::vr::PanelDepthScroll phoneDepthScroll, settingsDepthScroll;
    vr::VRActionHandle_t shortcutToggleLeft{vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t shortcutToggleRight{vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t shortcutDockLeft{vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t shortcutDockRight{vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t shortcutHaptic{vr::k_ulInvalidActionHandle};
    int hoveredDashboardCell{-1};
    FooterHoverTarget currentFooterHover{FooterHoverTarget::None};
    bool wasDashboardVisible{false};
    bool shortcutReady{false};
    bool shortcutActive{false};
    bool shown{false};
    bool desiredVisible{true};
    bool remoteStatusKnown{false};
    bool remoteAppEnabled{false};
    bool remoteAccessibilityEnabled{false};
    std::string pairingCode;
    std::chrono::steady_clock::time_point lastDashboardHealthCheck{};
    bool hasFrame{false};
    phonecast::vr::OverlaySettings currentSettings{};
    phonecast::vr::OverlaySettings pendingSettings{};
    bool hasPendingSettings{false};
    phonecast::vr::GlanceInput pendingGlanceInput{phonecast::vr::GlanceInput::LeftController};
    bool hasPendingGlanceInput{false};
    bool calibrationActive{false};
    bool calibrationDirty{false};
    std::uint64_t previousLeftControllerButtons{0};
    std::uint64_t previousRightControllerButtons{0};
    std::chrono::steady_clock::time_point lastCalibrationAdjustment{};
    vr::TrackedDeviceIndex_t grabbedDevice{vr::k_unTrackedDeviceIndexInvalid};
    vr::HmdMatrix34_t grabRelative{};
    vr::HmdMatrix34_t grabLastAbsolute{};
    phonecast::vr::OverlayInteractionController interaction{};
    phonecast::core::VideoFrame compositeFrame{};
    std::deque<core::PointerEvent> pendingPointerEvents;
    bool pointerDown{false};
    bool backButtonDown{false};
    bool resizing{false};
    float resizeStartX{};
    float resizeStartWidthMeters{};
    float lastPointerX{};
    float lastPointerY{};
#ifdef _WIN32
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* context{nullptr};
    ID3D11Texture2D* texture{nullptr};
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
#else
    std::unique_ptr<SteamFrameVulkanTexture> vulkanTexture;
#endif
    std::unique_ptr<TrackedCameraWorker> cameraWorker;
};

OpenVrOverlayRenderer::OpenVrOverlayRenderer(core::ILogger& logger, std::string executablePath)
    : impl_(std::make_unique<Impl>(logger, std::move(executablePath))) {}

OpenVrOverlayRenderer::~OpenVrOverlayRenderer() { Stop(); }

bool OpenVrOverlayRenderer::Start(const phonecast::vr::OverlaySettings& settings, std::string& error) {
    if (impl_->system != nullptr) {
        error = "OpenVR renderer is already started.";
        return false;
    }
    vr::EVRInitError initError = vr::VRInitError_None;
    impl_->system = vr::VR_Init(&initError, vr::VRApplication_Overlay);
    if (initError != vr::VRInitError_None || impl_->system == nullptr) {
        error = std::string("VR_Init failed: ") + vr::VR_GetVRInitErrorAsEnglishDescription(initError) +
                " (" + std::to_string(static_cast<int>(initError)) + ")";
        if (impl_->system != nullptr) vr::VR_Shutdown();
        impl_->system = nullptr;
        return false;
    }

    impl_->RegisterManifest();
    if constexpr (kEnableExperimentalOverlayGlobalInput) {
        impl_->explicitInputReady = impl_->InitializeControllerInput();
    } else {
        impl_->explicitInputReady = false;
        impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                          "Overlay-global controller actions are disabled; use the PhoneCast dashboard.");
    }
    impl_->overlayApi = vr::VROverlay();
    if (impl_->overlayApi == nullptr) {
        error = "OpenVR did not provide IVROverlay.";
        Stop();
        return false;
    }
    impl_->cameraWorker = std::make_unique<TrackedCameraWorker>(
        impl_->logger, impl_->system, vr::VRTrackedCamera());
    impl_->cameraWorker->Start();
#ifndef _WIN32
    impl_->vulkanTexture = std::make_unique<SteamFrameVulkanTexture>(impl_->logger);
    if (!impl_->vulkanTexture->Initialize(impl_->system, vr::VRCompositor(), error)) {
        Stop();
        return false;
    }
#endif
    if (!impl_->OverlayCall(impl_->overlayApi->CreateOverlay(kOverlayKey, kOverlayName, &impl_->overlay),
                            "CreateOverlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->CreateOverlay(
                                kMenuOverlayKey, kMenuOverlayName, &impl_->menuOverlay),
                            "Create control-grid overlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayWidthInMeters(impl_->menuOverlay, 0.34F),
                            "Set control-grid width", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlaySortOrder(impl_->menuOverlay, 100U),
                            "Set control-grid sort order", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayInputMethod(
                                impl_->menuOverlay, vr::VROverlayInputMethod_Mouse),
                            "Set control-grid input method", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->menuOverlay,
                                vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true),
                            "Set control-grid interactive flag", error) ||
        !impl_->OverlayCall(impl_->overlayApi->CreateOverlay(
                                kGestureOverlayKey, kGestureOverlayName, &impl_->gestureOverlay),
                            "Create gesture-progress overlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayWidthInMeters(
                                impl_->gestureOverlay, 0.08F),
                            "Set gesture-progress width", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlaySortOrder(
                                impl_->gestureOverlay, 102U),
                            "Set gesture-progress sort order", error) ||
        !impl_->OverlayCall(impl_->overlayApi->CreateOverlay(
                                kSettingsOverlayKey, kSettingsOverlayName, &impl_->settingsOverlay),
                            "Create settings overlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayWidthInMeters(impl_->settingsOverlay, 0.34F),
                            "Set settings width", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlaySortOrder(impl_->settingsOverlay, 101U),
                            "Set settings sort order", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayInputMethod(
                                impl_->settingsOverlay, vr::VROverlayInputMethod_Mouse),
                            "Set settings input method", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->settingsOverlay, vr::VROverlayFlags_SendVRSmoothScrollEvents, true),
                            "Enable settings smooth scroll events", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->settingsOverlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true),
                            "Enable settings discrete scroll diagnostics", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->settingsOverlay,
                                vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true),
                            "Set settings interactive flag", error) ||
        !impl_->OverlayCall(impl_->overlayApi->CreateOverlay(
                                kNotificationOverlayKey, kNotificationOverlayName,
                                &impl_->notificationOverlay),
                            "Create notification overlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayWidthInMeters(
                                impl_->notificationOverlay, 0.48F),
                            "Set notification width", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlaySortOrder(
                                impl_->notificationOverlay, 110U),
                            "Set notification sort order", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayInputMethod(
                                impl_->notificationOverlay, vr::VROverlayInputMethod_Mouse),
                            "Set notification input method", error) ||
        !impl_->OverlayCall(impl_->overlayApi->CreateOverlay(
                                kPassthroughOverlayKey, kPassthroughOverlayName,
                                &impl_->passthroughOverlay),
                            "Create passthrough overlay", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayAlpha(
                                impl_->passthroughOverlay, 1.0F),
                            "Set passthrough alpha", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlaySortOrder(
                                impl_->passthroughOverlay, 90U),
                            "Set passthrough sort order", error)) {
        Stop();
        return false;
    }

    const vr::HmdVector2_t menuMouseScale{{static_cast<float>(kMenuTextureWidth),
                                           static_cast<float>(kMenuTextureHeight)}};
    if (!impl_->OverlayCall(impl_->overlayApi->SetOverlayMouseScale(
                                impl_->menuOverlay, &menuMouseScale),
                            "Set control-grid mouse scale", error)) {
        Stop();
        return false;
    }

    const vr::HmdVector2_t settingsMouseScale{{static_cast<float>(kSettingsTextureWidth),
                                               static_cast<float>(kSettingsTextureHeight)}};
    const vr::HmdVector2_t notificationMouseScale{{
        static_cast<float>(kNotificationTextureWidth),
        static_cast<float>(kNotificationTextureHeight)}};
    auto settingsTransform = vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                                 {0.0F, 1.0F, 0.0F, -0.04F},
                                                 {0.0F, 0.0F, 1.0F, -0.85F}}};
    auto gestureTransform = vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.0F},
                                                {0.0F, 1.0F, 0.0F, -0.18F},
                                                {0.0F, 0.0F, 1.0F, -0.60F}}};
    auto notificationTransform = vr::HmdMatrix34_t{{{1.0F, 0.0F, 0.0F, 0.18F},
                                                     {0.0F, 1.0F, 0.0F, 0.12F},
                                                     {0.0F, 0.0F, 1.0F, -0.75F}}};
    if (!impl_->OverlayCall(impl_->overlayApi->SetOverlayTransformTrackedDeviceRelative(
                                impl_->settingsOverlay, vr::k_unTrackedDeviceIndex_Hmd,
                                &settingsTransform),
                            "Set settings transform", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayMouseScale(
                                impl_->settingsOverlay, &settingsMouseScale),
                            "Set settings mouse scale", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayTransformTrackedDeviceRelative(
                                impl_->gestureOverlay, vr::k_unTrackedDeviceIndex_Hmd,
                                &gestureTransform),
                            "Set gesture-progress transform", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayTransformTrackedDeviceRelative(
                                impl_->notificationOverlay, vr::k_unTrackedDeviceIndex_Hmd,
                                &notificationTransform),
                            "Set notification transform", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayMouseScale(
                                impl_->notificationOverlay, &notificationMouseScale),
                            "Set notification mouse scale", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayInputMethod(
                                impl_->overlay, vr::VROverlayInputMethod_Mouse),
                            "SetOverlayInputMethod", error) ||
        // Do not set MakeOverlaysInteractiveIfVisible on the persistent phone
        // surface. OpenVR documents that flag as activating system-wide laser
        // mouse mode, which suppresses controller poses/input in the running
        // game for as long as the phone remains visible. The phone receives
        // mouse events while the user has intentionally opened the dashboard;
        // with the dashboard closed it is view-only and leaves game input alone.
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true),
                            "Enable discrete overlay scroll events", error) ||
        !impl_->OverlayCall(impl_->overlayApi->SetOverlayFlag(
                                impl_->overlay, vr::VROverlayFlags_SendVRSmoothScrollEvents, true),
                            "Enable smooth overlay scroll events", error) ||
        !impl_->ApplySettings(settings, error)) {
        Stop();
        return false;
    }
    impl_->InitializeDashboard();
    impl_->InitializeShortcut();
    impl_->logger.Log(core::LogLevel::Info, "openvr",
                      "Overlay created; dashboard laser interacts with the phone and bottom handle without claiming game input while the dashboard is closed.");
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::SubmitFrame(const core::VideoFrame& frame, std::string& error) {
    if (impl_->overlayApi == nullptr || impl_->overlay == vr::k_ulOverlayHandleInvalid) {
        error = "OpenVR overlay is not started.";
        return false;
    }
    if (!frame.IsValid()) {
        error = "Cannot submit an invalid RGBA frame.";
        return false;
    }
    AddGrabHandle(frame, impl_->compositeFrame, impl_->currentFooterHover);
    impl_->hasFrame = true;
    if (!impl_->UploadCompositeTexture(error)) return false;
    const auto& composite = impl_->compositeFrame;
    const vr::HmdVector2_t mouseScale{{static_cast<float>(composite.width),
                                      static_cast<float>(composite.height)}};
    if (!impl_->OverlayCall(impl_->overlayApi->SetOverlayMouseScale(impl_->overlay, &mouseScale),
                            "SetOverlayMouseScale", error)) return false;
    impl_->interaction.SetSurfaceSize(frame.width, frame.height,
                                      kGrabHandleHeightPixels);
    impl_->hasFrame = true;
    if (impl_->desiredVisible && !impl_->shown) {
        if (!SetVisible(true, error)) return false;
        impl_->logger.Log(core::LogLevel::Info, "openvr", "Overlay is visible.");
    }
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::ApplySettings(const phonecast::vr::OverlaySettings& settings,
                                          std::string& error) {
    if (impl_->overlayApi == nullptr || impl_->overlay == vr::k_ulOverlayHandleInvalid) {
        error = "OpenVR overlay is not started.";
        return false;
    }
    if (!impl_->ApplySettings(settings, error)) return false;
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::SetVisible(bool visible, std::string& error) {
    const bool visibilityChanged = impl_->desiredVisible != visible;
    impl_->desiredVisible = visible;
    if (visibilityChanged) impl_->RenderDashboard();
    if (impl_->overlayApi == nullptr || impl_->overlay == vr::k_ulOverlayHandleInvalid) {
        error = "OpenVR overlay is not started.";
        return false;
    }
    if (visible && impl_->hasFrame) {
        if (!impl_->OverlayCall(impl_->overlayApi->ShowOverlay(impl_->overlay),
                                "ShowOverlay", error)) return false;
        impl_->shown = true;
    } else if (!visible) {
        if (impl_->shown) {
            if (!impl_->OverlayCall(impl_->overlayApi->HideOverlay(impl_->overlay),
                                    "HideOverlay", error)) return false;
            impl_->shown = false;
        }
        if (impl_->passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
            impl_->overlayApi->HideOverlay(impl_->passthroughOverlay);
        }
    }
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::PlaceBesideDashboard(
        phonecast::vr::OverlaySettings& settings, std::string& error) {
    vr::HmdMatrix34_t hmd{};
    if (!impl_->DevicePose(vr::k_unTrackedDeviceIndex_Hmd, hmd)) {
        error = "No HMD pose is available for the world reset.";
        return false;
    }
    float phoneWidth = settings.widthMeters;
    if (impl_->overlayApi->GetOverlayWidthInMeters(impl_->overlay, &phoneWidth) ==
        vr::VROverlayError_None) {
        const auto phoneHeight = impl_->compositeFrame.height > kGrabHandleHeightPixels
            ? impl_->compositeFrame.height - kGrabHandleHeightPixels : 0U;
        const float presentationScale = phoneHeight > 0 && impl_->compositeFrame.width > phoneHeight
            ? static_cast<float>(impl_->compositeFrame.width) / phoneHeight : 1.0F;
        settings.widthMeters = phoneWidth / presentationScale;
    }
    const auto local = FromArray(phonecast::vr::StartLocation(
        settings.startOffsetXMeters, settings.startOffsetYMeters, settings.startDistanceMeters));
    const auto panel = Multiply(hmd, local);
    settings.placementMode = phonecast::vr::PlacementMode::WorldLocked;
    settings.worldTransform = ToArray(panel);
    settings.worldTransformValid = true;
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::PinCurrentPosition(
        phonecast::vr::OverlaySettings& settings, std::string& error) {
    vr::HmdMatrix34_t absolute{};
    if (!impl_->AbsoluteForSettings(impl_->currentSettings, absolute)) {
        error = "The current phone pose is unavailable for world pinning.";
        return false;
    }
    float physicalWidth = settings.widthMeters;
    if (impl_->overlayApi->GetOverlayWidthInMeters(impl_->overlay, &physicalWidth) ==
        vr::VROverlayError_None) {
        const auto phoneHeight = impl_->compositeFrame.height > kGrabHandleHeightPixels
            ? impl_->compositeFrame.height - kGrabHandleHeightPixels : 0U;
        const float presentationScale = phoneHeight > 0 && impl_->compositeFrame.width > phoneHeight
            ? static_cast<float>(impl_->compositeFrame.width) / phoneHeight : 1.0F;
        settings.widthMeters = physicalWidth / presentationScale;
    }
    settings.placementMode = phonecast::vr::PlacementMode::WorldLocked;
    settings.worldTransform = ToArray(absolute);
    settings.worldTransformValid = true;
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::PumpEvents() {
    if (impl_->overlayApi == nullptr || impl_->system == nullptr) return false;
    impl_->EnsureDashboardHealthy();
    impl_->PollShortcut();
    const bool dashboardVisible = impl_->overlayApi->IsDashboardVisible();
    if (impl_->wasDashboardVisible && !dashboardVisible) {
        if (impl_->resizing) impl_->EndResize();
        if (impl_->grabbedDevice != vr::k_unTrackedDeviceIndexInvalid)
            impl_->EndGrab(impl_->grabbedDevice);
        if (impl_->settingsGrabbedDevice != vr::k_unTrackedDeviceIndexInvalid)
            impl_->EndSettingsGrab(impl_->settingsGrabbedDevice);
        if (impl_->pointerDown) {
            impl_->QueuePointer(impl_->interaction.PointerUp(
                impl_->lastPointerX, impl_->lastPointerY));
            impl_->pointerDown = false;
        }
        impl_->backButtonDown = false;
    }
    impl_->wasDashboardVisible = dashboardVisible;
    impl_->UpdateControllerPlacement();
    impl_->PollControllerCalibration();
    vr::VREvent_t event{};
    while (impl_->dashboardOverlay != vr::k_ulOverlayHandleInvalid &&
           impl_->overlayApi->PollNextOverlayEvent(
               impl_->dashboardOverlay, &event, sizeof(event))) {
        if (event.eventType == vr::VREvent_MouseMove) {
            const int cell = DashboardCell(event.data.mouse.x, event.data.mouse.y);
            if (cell != impl_->hoveredDashboardCell) {
                impl_->hoveredDashboardCell = cell;
                if (cell >= 0) {
                    impl_->TriggerHaptic(impl_->dashboardOverlay, 0.015F, 50.0F, 0.25F, event.trackedDeviceIndex);
                }
                impl_->RenderDashboard();
            }
        } else if (event.eventType == vr::VREvent_MouseButtonDown &&
            (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            impl_->TriggerHaptic(impl_->dashboardOverlay, 0.03F, 80.0F, 0.6F, event.trackedDeviceIndex);
            impl_->QueueDashboardCell(
                DashboardCell(event.data.mouse.x, event.data.mouse.y));
        } else if (event.eventType == vr::VREvent_FocusLeave ||
                   event.eventType == vr::VREvent_OverlayHidden) {
            if (impl_->hoveredDashboardCell != -1) {
                impl_->hoveredDashboardCell = -1;
                impl_->RenderDashboard();
            }
        }
        if (IsQuitEvent(event.eventType)) {
            impl_->logger.Log(core::LogLevel::Info, "openvr",
                              "Runtime requested overlay shutdown.");
            return false;
        }
    }
    while (impl_->notificationOverlay != vr::k_ulOverlayHandleInvalid &&
           impl_->overlayApi->PollNextOverlayEvent(
               impl_->notificationOverlay, &event, sizeof(event))) {
        if (event.eventType == vr::VREvent_MouseButtonDown &&
            (event.data.mouse.button & vr::VRMouseButton_Left) != 0 &&
            !impl_->notificationPreviewVisible) {
            impl_->notificationOpenRequested = true;
            impl_->pendingNotificationActionToken = impl_->currentNotificationActionToken;
            impl_->overlayApi->HideOverlay(impl_->notificationOverlay);
            impl_->notificationVisible = false;
            impl_->logger.Log(core::LogLevel::Info, "openvr-notification",
                              "Notification card requested the full phone view.");
        }
        if (IsQuitEvent(event.eventType)) return false;
    }
    while (impl_->overlayApi->PollNextOverlayEvent(
               impl_->settingsOverlay, &event, sizeof(event))) {
        if (event.eventType == vr::VREvent_FocusLeave || event.eventType == vr::VREvent_OverlayHidden) {
            impl_->EndSettingsGrab(impl_->settingsGrabbedDevice);
        } else if (event.eventType == vr::VREvent_MouseButtonDown &&
            (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            impl_->TriggerHaptic(impl_->settingsOverlay, 0.025F, 70.0F, 0.5F, event.trackedDeviceIndex);
            if (impl_->IsSettingsResizeHandle(event.data.mouse.x, event.data.mouse.y))
                impl_->BeginSettingsResize(event.data.mouse.x);
            else if (impl_->IsSettingsGrabHandle(event.data.mouse.y))
                impl_->BeginSettingsGrab(event.trackedDeviceIndex);
            else
                impl_->QueueSettingsLaserClick(event.data.mouse.x, event.data.mouse.y);
        } else if (event.eventType == vr::VREvent_MouseButtonUp &&
                   (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            if (impl_->settingsResizing)
                impl_->EndSettingsResize();
            if (impl_->settingsGrabbedDevice != vr::k_unTrackedDeviceIndexInvalid)
                impl_->EndSettingsGrab(event.trackedDeviceIndex);
        } else if (event.eventType == vr::VREvent_MouseMove) {
            if (impl_->settingsResizing)
                impl_->UpdateSettingsResize(event.data.mouse.x);
        } else if (event.eventType == vr::VREvent_ScrollSmooth ||
                   event.eventType == vr::VREvent_ScrollDiscrete) {
            impl_->QueueDragScroll(event, true);
        }
        if (IsQuitEvent(event.eventType)) {
            impl_->logger.Log(core::LogLevel::Info, "openvr", "Runtime requested overlay shutdown.");
            return false;
        }
    }
    while (impl_->overlayApi->PollNextOverlayEvent(
               impl_->menuOverlay, &event, sizeof(event))) {
        if (event.eventType == vr::VREvent_MouseMove) {
            // Keep the static grid texture stable; the runtime laser supplies
            // hover feedback without repeated SetOverlayRaw submissions.
            impl_->radialMenuSelection = impl_->MenuGridCell(
                event.data.mouse.x, event.data.mouse.y);
        } else if (event.eventType == vr::VREvent_MouseButtonDown &&
                   (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            impl_->AcceptMenuCell(
                impl_->MenuGridCell(event.data.mouse.x, event.data.mouse.y));
        }
        if (IsQuitEvent(event.eventType)) {
            impl_->logger.Log(core::LogLevel::Info, "openvr", "Runtime requested overlay shutdown.");
            return false;
        }
    }
    while (impl_->overlayApi->PollNextOverlayEvent(impl_->overlay, &event, sizeof(event))) {
        if (event.eventType == vr::VREvent_FocusLeave || event.eventType == vr::VREvent_OverlayHidden) {
            impl_->EndGrab(impl_->grabbedDevice);
            if (impl_->resizing) impl_->EndResize();
            if (impl_->pointerDown) {
                impl_->QueuePointer(impl_->interaction.PointerUp(impl_->lastPointerX, impl_->lastPointerY));
                impl_->pointerDown = false;
            }
            impl_->backButtonDown = false;
            if (impl_->currentFooterHover != FooterHoverTarget::None) {
                impl_->currentFooterHover = FooterHoverTarget::None;
                impl_->UpdateFooterHover(FooterHoverTarget::None);
            }
        } else if (!impl_->calibrationActive && event.eventType == vr::VREvent_MouseButtonDown &&
            (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            impl_->TriggerHaptic(impl_->overlay, 0.025F, 70.0F, 0.5F, event.trackedDeviceIndex);
            if (impl_->interaction.IsBackButton(event.data.mouse.x, event.data.mouse.y)) {
                impl_->backButtonDown = true;
                impl_->QueuePointer(impl_->interaction.Back());
                impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                                  "Android Back requested from the overlay button.");
            } else if (impl_->interaction.IsHomeButton(event.data.mouse.x, event.data.mouse.y)) {
                impl_->backButtonDown = true;
                impl_->QueuePointer(impl_->interaction.Home());
                impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                                  "Android Home requested from the overlay button.");
            } else if (impl_->interaction.IsRecentsButton(event.data.mouse.x, event.data.mouse.y)) {
                impl_->backButtonDown = true;
                impl_->QueuePointer(impl_->interaction.Recents());
                impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                                  "Android Recents requested from the overlay button.");
            } else if (impl_->interaction.IsKeyboardButton(event.data.mouse.x, event.data.mouse.y)) {
                impl_->backButtonDown = true;
                if (impl_->overlayApi) {
                    impl_->overlayApi->ShowKeyboardForOverlay(
                        impl_->overlay,
                        vr::k_EGamepadTextInputModeNormal,
                        vr::k_EGamepadTextInputLineModeSingleLine,
                        vr::KeyboardFlag_Modal,
                        "PhoneCast Keyboard",
                        1024,
                        "",
                        0);
                    impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                                      "SteamVR virtual keyboard requested.");
                }
            } else if (impl_->interaction.IsCloseButton(
                           event.data.mouse.x, event.data.mouse.y)) {
                impl_->backButtonDown = true;
                impl_->hideRequested = true;
                impl_->logger.Log(core::LogLevel::Info, "openvr-input",
                                  "Phone hide requested from the overlay close button.");
            } else if (event.data.mouse.y < static_cast<float>(kGrabBarHeightPixels)) {
                if (impl_->interaction.IsResizeButton(event.data.mouse.x, event.data.mouse.y)) {
                    impl_->BeginResize(event.data.mouse.x);
                } else {
                    impl_->BeginGrab(event.trackedDeviceIndex);
                }
            } else if (impl_->interaction.IsGrabHandle(event.data.mouse.y)) {
                // Click on blank background area of the nav bar; consume without sending touch to phone.
            } else {
                impl_->pointerDown = true;
                impl_->lastPointerX = event.data.mouse.x;
                impl_->lastPointerY = event.data.mouse.y;
                impl_->QueuePointer(impl_->interaction.PointerDown(
                    event.data.mouse.x, event.data.mouse.y));
            }
        } else if (!impl_->calibrationActive && event.eventType == vr::VREvent_MouseMove) {
            impl_->lastPointerX = event.data.mouse.x;
            impl_->lastPointerY = event.data.mouse.y;
            if (impl_->resizing) {
                impl_->UpdateResize(event.data.mouse.x);
            } else if (impl_->pointerDown) {
                impl_->QueuePointer(impl_->interaction.PointerMove(
                    event.data.mouse.x, event.data.mouse.y));
            } else if (impl_->interaction.IsGrabHandle(event.data.mouse.y)) {
                FooterHoverTarget target = FooterHoverTarget::Grab;
                if (event.data.mouse.y < static_cast<float>(kGrabBarHeightPixels)) {
                    if (impl_->interaction.IsResizeButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Resize;
                    }
                } else {
                    if (impl_->interaction.IsBackButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Back;
                    } else if (impl_->interaction.IsHomeButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Home;
                    } else if (impl_->interaction.IsRecentsButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Recents;
                    } else if (impl_->interaction.IsKeyboardButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Keyboard;
                    } else if (impl_->interaction.IsCloseButton(event.data.mouse.x, event.data.mouse.y)) {
                        target = FooterHoverTarget::Close;
                    } else {
                        target = FooterHoverTarget::None;
                    }
                }
                if (target != impl_->currentFooterHover) {
                    impl_->currentFooterHover = target;
                    if (target != FooterHoverTarget::None) {
                        impl_->TriggerHaptic(impl_->overlay, 0.015F, 50.0F, 0.25F, event.trackedDeviceIndex);
                    }
                    impl_->UpdateFooterHover(target);
                }
            } else {
                if (impl_->currentFooterHover != FooterHoverTarget::None) {
                    impl_->currentFooterHover = FooterHoverTarget::None;
                    impl_->UpdateFooterHover(FooterHoverTarget::None);
                }
            }
        } else if (event.eventType == vr::VREvent_KeyboardDone) {
            char textBuffer[4096] = {0};
            const uint32_t len = impl_->overlayApi->GetKeyboardText(textBuffer, sizeof(textBuffer));
            if (len > 0) {
                impl_->QueueTextEntered(std::string(textBuffer));
                impl_->logger.Log(core::LogLevel::Info, "openvr-keyboard",
                                  "SteamVR keyboard text forwarded.");
            }
        } else if (!impl_->calibrationActive && event.eventType == vr::VREvent_MouseButtonUp &&
                   (event.data.mouse.button & vr::VRMouseButton_Left) != 0) {
            if (impl_->backButtonDown) {
                impl_->backButtonDown = false;
            } else if (impl_->resizing) {
                impl_->EndResize();
            } else if (impl_->grabbedDevice != vr::k_unTrackedDeviceIndexInvalid) {
                impl_->EndGrab(event.trackedDeviceIndex);
            } else if (impl_->pointerDown) {
                impl_->QueuePointer(impl_->interaction.PointerUp(
                    event.data.mouse.x, event.data.mouse.y));
            }
            impl_->pointerDown = false;
        } else if (!impl_->calibrationActive &&
                   (event.eventType == vr::VREvent_ScrollDiscrete ||
                    event.eventType == vr::VREvent_ScrollSmooth)) {
            if (impl_->grabbedDevice != vr::k_unTrackedDeviceIndexInvalid) {
                impl_->QueueDragScroll(event, false);
            } else if (!impl_->resizing && !impl_->interaction.IsGrabHandle(impl_->lastPointerY)) {
                impl_->QueuePointer(impl_->interaction.Scroll(
                    impl_->lastPointerX, impl_->lastPointerY, event.data.scroll.ydelta));
            }
        }
        if (IsQuitEvent(event.eventType)) {
            impl_->logger.Log(core::LogLevel::Info, "openvr", "Runtime requested overlay shutdown.");
            return false;
        }
    }
    impl_->UpdateDragDepth();
    while (impl_->system->PollNextEvent(&event, sizeof(event))) {
        if (IsQuitEvent(event.eventType)) {
            impl_->system->AcknowledgeQuit_Exiting();
            impl_->logger.Log(core::LogLevel::Info, "openvr", "Runtime requested process shutdown.");
            return false;
        }
    }
    return true;
}

bool OpenVrOverlayRenderer::TakeSettingsUpdate(phonecast::vr::OverlaySettings& settings) {
    if (!impl_->hasPendingSettings) return false;
    settings = impl_->pendingSettings;
    impl_->hasPendingSettings = false;
    return true;
}

bool OpenVrOverlayRenderer::TakeGlanceInput(phonecast::vr::GlanceInput& input) {
    if (!impl_->hasPendingGlanceInput) return false;
    input = impl_->pendingGlanceInput;
    impl_->hasPendingGlanceInput = false;
    return true;
}

bool OpenVrOverlayRenderer::TakeRadialMenuSelection(
        phonecast::vr::RadialMenuSelection& selection) {
    if (!impl_->hasPendingRadialMenuSelection) return false;
    selection = impl_->pendingRadialMenuSelection;
    impl_->hasPendingRadialMenuSelection = false;
    return true;
}

bool OpenVrOverlayRenderer::ShowSettingsMenu(
        const phonecast::vr::SettingsMenuView& view, std::string& error) {
    if (impl_->overlayApi == nullptr ||
        impl_->settingsOverlay == vr::k_ulOverlayHandleInvalid) {
        error = "The OpenVR settings overlay is unavailable.";
        return false;
    }
    impl_->settingsMenuView = view;
    auto image = MakeSettingsTexture(view);
    if (!impl_->CaptureSettingsTransform()) {
        error = "Could not place the settings panel in world space.";
        return false;
    }
    if (!impl_->OverlayCall(impl_->overlayApi->SetOverlayRaw(
                                impl_->settingsOverlay, image.data(),
                                kSettingsTextureWidth, kSettingsTextureHeight, 4),
                            "Set settings texture", error) ||
        !impl_->OverlayCall(impl_->overlayApi->ShowOverlay(impl_->settingsOverlay),
                            "Show settings overlay", error)) return false;
    impl_->settingsMenuLeft = impl_->radialMenuLeft;
    impl_->settingsMenuVisible = true;
    impl_->UpdateNotificationPreview(view.showNotificationPreview);
    impl_->UpdateStartLocationPreview(view.showStartLocationPreview);
    impl_->logger.Log(core::LogLevel::Info, "openvr-settings", "In-headset settings menu shown.");
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::HideSettingsMenu(std::string& error) {
    if (impl_->overlayApi == nullptr ||
        impl_->settingsOverlay == vr::k_ulOverlayHandleInvalid) {
        error = "The OpenVR settings overlay is unavailable.";
        return false;
    }
    if (!impl_->OverlayCall(impl_->overlayApi->HideOverlay(impl_->settingsOverlay),
                            "Hide settings overlay", error)) return false;
    impl_->settingsMenuVisible = false;
    impl_->UpdateNotificationPreview(false);
    impl_->UpdateStartLocationPreview(false);
    impl_->settingsAxisLatched = false;
    impl_->hasPendingSettingsMenuCommand = false;
    impl_->settingsLaserTargetRow = -1;
    impl_->logger.Log(core::LogLevel::Info, "openvr-settings", "In-headset settings menu hidden.");
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::TakeSettingsMenuInput(
        phonecast::vr::SettingsMenuInput& input) {
    if (impl_->hasPendingSettingsMenuCommand) {
        input = impl_->pendingSettingsMenuInput;
        impl_->hasPendingSettingsMenuCommand = false;
        return true;
    }
    if (impl_->settingsLaserTargetRow < 0) return false;
    if (impl_->settingsMenuView.selectedIndex !=
        static_cast<std::size_t>(impl_->settingsLaserTargetRow)) {
        const auto count = impl_->settingsMenuView.labels.size();
        const auto current = impl_->settingsMenuView.selectedIndex;
        const auto target = static_cast<std::size_t>(impl_->settingsLaserTargetRow);
        const auto forward = (target + count - current) % count;
        const auto backward = (current + count - target) % count;
        input = {forward <= backward
            ? phonecast::vr::SettingsMenuCommand::NextItem
            : phonecast::vr::SettingsMenuCommand::PreviousItem, 0.0F};
        return true;
    }
    input = impl_->settingsLaserInput;
    impl_->settingsLaserTargetRow = -1;
    return true;
}

void OpenVrOverlayRenderer::SetRemoteControlStatus(bool known, bool appEnabled,
                                                    bool accessibilityEnabled) {
    const bool changed = impl_->remoteStatusKnown != known ||
        impl_->remoteAppEnabled != appEnabled ||
        impl_->remoteAccessibilityEnabled != accessibilityEnabled;
    impl_->remoteStatusKnown = known;
    impl_->remoteAppEnabled = appEnabled;
    impl_->remoteAccessibilityEnabled = accessibilityEnabled;
    if (changed) impl_->RenderDashboard();
}

std::string OpenVrOverlayRenderer::DefaultAudioDeviceId() const {
    auto* system = vr::VRSystem();
    if (!system) return {};
    char device[1024]{};
    vr::ETrackedPropertyError propertyError = vr::TrackedProp_Success;
    const auto size = system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_Audio_DefaultPlaybackDeviceId_String,device,sizeof(device),&propertyError);
    return propertyError == vr::TrackedProp_Success && size > 1 && size <= sizeof(device)
        ? std::string(device) : std::string{};
}

void OpenVrOverlayRenderer::SetPairingCode(const std::string& pairCode) {
    impl_->pairingCode = pairCode;
    impl_->RenderDashboard();
}

bool OpenVrOverlayRenderer::FocusDashboard(std::string& error) {
    if (impl_->overlayApi == nullptr) {
        error = "OpenVR is not running; the PhoneCast dashboard cannot be focused.";
        return false;
    }
    impl_->EnsureDashboardHealthy();
    if (impl_->dashboardOverlay == vr::k_ulOverlayHandleInvalid) {
        error = "The PhoneCast dashboard overlay is unavailable.";
        return false;
    }
    impl_->overlayApi->ShowDashboard(kDashboardOverlayKey);
    impl_->logger.Log(core::LogLevel::Info, "openvr-dashboard",
                      "Opened the SteamVR dashboard with PhoneCast selected.");
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::TakePointerEvent(core::PointerEvent& event) {
    if (impl_->pendingPointerEvents.empty()) return false;
    event = impl_->pendingPointerEvents.front();
    impl_->pendingPointerEvents.pop_front();
    return true;
}

bool OpenVrOverlayRenderer::TakeTextEntered(std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->textQueueMutex);
    if (impl_->pendingTextQueue.empty()) return false;
    text = std::move(impl_->pendingTextQueue.front());
    impl_->pendingTextQueue.pop_front();
    return true;
}

bool OpenVrOverlayRenderer::ShowNotification(
        const core::protocol::NotificationEvent& notification, std::string& error) {
    if (impl_->overlayApi == nullptr ||
        impl_->notificationOverlay == vr::k_ulOverlayHandleInvalid) {
        error = "The OpenVR notification overlay is unavailable.";
        return false;
    }
    auto image = MakeNotificationTexture(notification);
    if (!impl_->OverlayCall(impl_->overlayApi->SetOverlayRaw(
                                impl_->notificationOverlay, image.data(),
                                kNotificationTextureWidth, kNotificationTextureHeight, 4),
                            "Set notification texture", error) ||
        !impl_->OverlayCall(impl_->overlayApi->ShowOverlay(impl_->notificationOverlay),
                            "Show notification overlay", error)) return false;
    impl_->notificationPreviewVisible = false;
    impl_->currentNotificationActionToken = notification.actionToken;
    impl_->notificationVisible = true;
    impl_->logger.Log(core::LogLevel::Info, "openvr-notification",
                      notification.actionToken == 0U
                          ? "Displayed a notification card without an Android open action."
                          : "Displayed a notification card with an Android open action.");
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::HideNotification(std::string& error) {
    if (impl_->overlayApi == nullptr ||
        impl_->notificationOverlay == vr::k_ulOverlayHandleInvalid) {
        error = "The OpenVR notification overlay is unavailable.";
        return false;
    }
    if (impl_->notificationVisible &&
        !impl_->OverlayCall(impl_->overlayApi->HideOverlay(impl_->notificationOverlay),
                            "Hide notification overlay", error)) return false;
    impl_->notificationVisible = false;
    error.clear();
    return true;
}

bool OpenVrOverlayRenderer::TakeNotificationOpenRequest(std::uint64_t& actionToken) {
    if (!impl_->notificationOpenRequested) return false;
    actionToken = impl_->pendingNotificationActionToken;
    impl_->notificationOpenRequested = false;
    impl_->pendingNotificationActionToken = 0;
    return true;
}

bool OpenVrOverlayRenderer::TakeHideRequest() {
    if (!impl_->hideRequested) return false;
    impl_->hideRequested = false;
    return true;
}

bool OpenVrOverlayRenderer::TakeQuitRequest() {
    if (!impl_->quitRequested) return false;
    impl_->quitRequested = false;
    return true;
}

bool OpenVrOverlayRenderer::GetPerformanceStats(phonecast::vr::VrPerformanceStats& stats) {
    stats = {};
    vr::IVRCompositor* compositor = vr::VRCompositor();
    if (impl_->system == nullptr || compositor == nullptr) return false;

    vr::Compositor_FrameTiming timing{};
    timing.m_nSize = sizeof(timing);
    if (!compositor->GetFrameTiming(&timing, 0)) return false;

    stats.available = true;
    stats.frameIndex = timing.m_nFrameIndex;
    stats.framePresents = timing.m_nNumFramePresents;
    stats.misPresentedFrames = timing.m_nNumMisPresented;
    stats.droppedFrames = timing.m_nNumDroppedFrames;
    stats.reprojectionFlags = timing.m_nReprojectionFlags;
    stats.totalRenderGpuMilliseconds = timing.m_flTotalRenderGpuMs;
    stats.compositorGpuMilliseconds = timing.m_flCompositorRenderGpuMs;
    stats.compositorCpuMilliseconds = timing.m_flCompositorRenderCpuMs;
    stats.clientFrameIntervalMilliseconds = timing.m_flClientFrameIntervalMs;
    return true;
}

void OpenVrOverlayRenderer::Stop() noexcept {
#ifndef _WIN32
    // Detach compositor ownership before destroying the overlay. Vulkan images
    // remain alive until after VR_Shutdown below.
    if (impl_->vulkanTexture != nullptr && impl_->overlayApi != nullptr &&
        impl_->overlay != vr::k_ulOverlayHandleInvalid)
        impl_->overlayApi->ClearOverlayTexture(impl_->overlay);
#endif
    impl_->DestroyDashboard();
    if (impl_->overlayApi != nullptr && impl_->gestureOverlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->gestureOverlay);
        impl_->overlayApi->DestroyOverlay(impl_->gestureOverlay);
    }
    if (impl_->overlayApi != nullptr && impl_->notificationOverlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->notificationOverlay);
        impl_->overlayApi->DestroyOverlay(impl_->notificationOverlay);
    }
    if (impl_->overlayApi != nullptr && impl_->passthroughOverlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->passthroughOverlay);
        impl_->overlayApi->DestroyOverlay(impl_->passthroughOverlay);
    }
    if (impl_->overlayApi != nullptr && impl_->settingsOverlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->settingsOverlay);
        impl_->overlayApi->DestroyOverlay(impl_->settingsOverlay);
    }
    if (impl_->overlayApi != nullptr && impl_->menuOverlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->menuOverlay);
        impl_->overlayApi->DestroyOverlay(impl_->menuOverlay);
    }
    if (impl_->overlayApi != nullptr && impl_->overlay != vr::k_ulOverlayHandleInvalid) {
        impl_->overlayApi->HideOverlay(impl_->overlay);
        impl_->overlayApi->DestroyOverlay(impl_->overlay);
    }
    impl_->gestureOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->settingsOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->notificationOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->passthroughOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->dashboardOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->dashboardThumbnail = vr::k_ulOverlayHandleInvalid;
    impl_->menuOverlay = vr::k_ulOverlayHandleInvalid;
    impl_->overlay = vr::k_ulOverlayHandleInvalid;
    impl_->overlayApi = nullptr;
    impl_->shown = false;
    impl_->desiredVisible = true;
    impl_->remoteStatusKnown = false;
    impl_->remoteAppEnabled = false;
    impl_->remoteAccessibilityEnabled = false;
    impl_->pairingCode.clear();
    impl_->lastDashboardHealthCheck = {};
    impl_->hasFrame = false;
    impl_->hasPendingSettings = false;
    impl_->hasPendingGlanceInput = false;
    impl_->hasPendingRadialMenuSelection = false;
    impl_->resizing = false;
    impl_->shortcutReady = false;
    impl_->shortcutActive = false;
    impl_->phoneDepthScroll = {};
    impl_->settingsDepthScroll = {};
    impl_->wasDashboardVisible = false;
    impl_->hasPendingSettingsMenuCommand = false;
    impl_->gestureProgressVisible = false;
    impl_->startLocationPreviewVisible = false;
    impl_->gestureProgressStep = -1;
    impl_->settingsMenuVisible = false;
    impl_->settingsWorldTransformValid = false;
    impl_->settingsGrabbedDevice = vr::k_unTrackedDeviceIndexInvalid;
    impl_->notificationVisible = false;
    impl_->notificationPreviewVisible = false;
    impl_->notificationOpenRequested = false;
    impl_->currentNotificationActionToken = 0;
    impl_->pendingNotificationActionToken = 0;
    impl_->hideRequested = false;
    impl_->quitRequested = false;
    impl_->settingsAxisLatched = false;
    impl_->settingsLaserTargetRow = -1;
    impl_->radialMenuVisible = false;
    impl_->radialAwaitRelease = false;
    impl_->radialMenuSelection = -1;
    impl_->leftHold = {};
    impl_->rightHold = {};
    impl_->wristMenuGesture.Reset();
    impl_->explicitInputReady = false;
    impl_->actionUpdateErrorReported = false;
    impl_->inputApi = nullptr;
    impl_->actionSet = vr::k_ulInvalidActionSetHandle;
    impl_->leftActions = {};
    impl_->rightActions = {};
    impl_->calibrationActive = false;
    impl_->calibrationDirty = false;
    impl_->previousLeftControllerButtons = 0;
    impl_->previousRightControllerButtons = 0;
    impl_->grabbedDevice = vr::k_unTrackedDeviceIndexInvalid;
    impl_->pendingPointerEvents.clear();
    impl_->pointerDown = false;
    impl_->backButtonDown = false;
#ifdef _WIN32
    if (impl_->texture != nullptr) impl_->texture->Release();
    if (impl_->context != nullptr) impl_->context->Release();
    if (impl_->device != nullptr) impl_->device->Release();
    impl_->texture = nullptr;
    impl_->context = nullptr;
    impl_->device = nullptr;
    impl_->textureWidth = 0;
    impl_->textureHeight = 0;
#endif
    if (impl_->cameraWorker) {
        impl_->cameraWorker->Stop();
        impl_->cameraWorker.reset();
    }
    if (impl_->system != nullptr) vr::VR_Shutdown();
    impl_->system = nullptr;
#ifndef _WIN32
    if (impl_->vulkanTexture != nullptr) impl_->vulkanTexture->Shutdown();
    impl_->vulkanTexture.reset();
#endif
}

void OpenVrOverlayRenderer::SetScreenMetrics(const core::protocol::StreamScreenMetrics& metrics) {
    if (impl_ && impl_->cameraWorker) {
        impl_->cameraWorker->SetScreenMetrics(metrics);
    }
}

void OpenVrOverlayRenderer::IngestPhoneImu(const core::protocol::StreamPhoneImu& imu) {
    if (impl_ && impl_->cameraWorker) {
        impl_->cameraWorker->IngestPhoneImu(imu);
    }
}

}  // namespace phonecast::platform::openvr
