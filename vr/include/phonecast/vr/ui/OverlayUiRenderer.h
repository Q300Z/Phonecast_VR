#pragma once

#include "UiCanvas.h"
#include "UiTheme.h"
#include "phonecast/core/protocol/StreamProtocol.h"
#include "phonecast/core/streaming/VideoFrame.h"
#include "phonecast/vr/overlay/IOverlayRenderer.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace phonecast::vr::ui {

constexpr std::uint32_t kNotificationTextureWidth = 768;
constexpr std::uint32_t kNotificationTextureHeight = 256;
constexpr std::uint32_t kMenuTextureWidth = 512;
constexpr std::uint32_t kMenuTextureHeight = 896;
constexpr int kMenuColumns = 2;
constexpr int kMenuRows = 5;
constexpr int kCloseMenuCell = 8;
constexpr std::uint32_t kGestureTextureSize = 128;
constexpr int kGestureProgressSteps = 24;
constexpr std::uint32_t kSettingsTextureWidth = 512;
constexpr std::uint32_t kSettingsTextureHeight = 978;
constexpr std::uint32_t kDashboardTextureWidth = 1024;
constexpr std::uint32_t kDashboardTextureHeight = 600;
constexpr std::uint32_t kDashboardThumbnailSize = 256;
constexpr std::uint32_t kNavBarHeightPixels = 48;
constexpr std::uint32_t kGrabBarHeightPixels = 40;
constexpr std::uint32_t kGrabHandleHeightPixels = kNavBarHeightPixels + kGrabBarHeightPixels;

enum class DashboardControl {
    ToggleVisible,
    HeadLocked,
    WorldLocked,
    PhoneTracked,
    AndroidBack,
    LeftControllerLocked,
    RightControllerLocked,
    OpenSettings,
    Quit
};

enum class FooterHoverTarget {
    None,
    Back,
    Home,
    Recents,
    Keyboard,
    Grab,
    Close,
    Resize
};

struct DashboardTarget {
    int left;
    int top;
    int width;
    int height;
    DashboardControl control;
};

// 3+2 Dashboard layout grid shared between rendering and hit-testing
constexpr std::array<DashboardTarget, 9> kDashboardTargets{{
    {24, 136, 360, 174, DashboardControl::ToggleVisible},
    {416, 156, 181, 72, DashboardControl::HeadLocked},
    {609, 156, 181, 72, DashboardControl::WorldLocked},
    {802, 156, 182, 72, DashboardControl::PhoneTracked},
    {24, 352, 0, 0, DashboardControl::AndroidBack},
    {416, 244, 278, 72, DashboardControl::LeftControllerLocked},
    {706, 244, 278, 72, DashboardControl::RightControllerLocked},
    {416, 408, 376, 104, DashboardControl::OpenSettings},
    {808, 408, 176, 104, DashboardControl::Quit}
}};

// Renders the Settings Menu into an RGBA texture buffer
std::vector<std::uint8_t> RenderSettingsMenu(const SettingsMenuView& view);

// Renders the SteamVR Dashboard main panel
std::vector<std::uint8_t> RenderDashboard(
    bool currentlyVisible, PlacementMode placementMode,
    bool remoteStatusKnown, bool remoteAppEnabled,
    bool remoteAccessibilityEnabled, const std::string& pairingCode,
    int hoveredCell = -1);

// Renders the 256x256 Dashboard icon thumbnail
std::vector<std::uint8_t> RenderDashboardThumbnail();

// Hit tests dashboard coordinates (OpenGL / OpenVR overlay coordinates where Y is inverted)
int HitTestDashboard(float mouseX, float mouseY);

// Renders the in-world radial / thumbstick menu
std::vector<std::uint8_t> RenderRadialMenu(int selected, bool currentlyVisible);

// Hit tests radial menu coordinates
int HitTestRadialMenu(float mouseX, float mouseY);

// Renders wrist gesture progress ring
std::vector<std::uint8_t> RenderGestureProgress(int completedSteps);

// Sanitizes text for notification rendering with the 5x7 bitmap font
std::string SanitizeNotificationLine(const std::string& value, std::size_t maximumCharacters);

// Renders floating privacy-filtered notification card
std::vector<std::uint8_t> RenderNotification(const core::protocol::NotificationEvent& notification);

// Renders the two-row navigation and grab/resize footer onto a composite frame
void RenderGrabFooter(core::VideoFrame& composite, std::uint32_t phoneHeight,
                      FooterHoverTarget hover = FooterHoverTarget::None);

// Copies the incoming phone frame and appends the 88px grab footer
void AddGrabFooter(const core::VideoFrame& frame, core::VideoFrame& composite,
                   FooterHoverTarget hover = FooterHoverTarget::None);

}  // namespace phonecast::vr::ui
