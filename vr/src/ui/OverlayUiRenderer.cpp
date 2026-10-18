#include "phonecast/vr/ui/OverlayUiRenderer.h"

#include <algorithm>
#include <cmath>

namespace phonecast::vr::ui {

namespace {

constexpr float kPi = 3.14159265358979323846F;

constexpr std::array<const char*, 9> kMenuLabels{
    "SHOW", "GLANCE", "PIN", "SETTINGS", "HEAD", "WORLD", "LEFT", "RIGHT", "CLOSE"
};

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

}  // namespace

std::vector<std::uint8_t> RenderSettingsMenu(const SettingsMenuView& view) {
    UiCanvas canvas(kSettingsTextureWidth, kSettingsTextureHeight);

    // Main window container
    canvas.FillRoundedRect(4, 4, static_cast<int>(kSettingsTextureWidth) - 8,
                          static_cast<int>(kSettingsTextureHeight) - 8, 22, theme::kBgDark);

    // Top Header surface
    canvas.FillRoundedRect(10, 10, static_cast<int>(kSettingsTextureWidth) - 20, 84, 16, theme::kBgSurface);
    canvas.FillRect(10, 10, static_cast<int>(kSettingsTextureWidth) - 20, 2, theme::kAccentGlow);

    if (view.title != "PHONECAST SETTINGS") {
        // Back button (<) on the left
        canvas.FillRoundedRect(20, 32, 44, 36, 8, theme::kBgElevated);
        canvas.DrawIconBack(20 + 22, 32 + 18, theme::kTextPrimary);
    }
    canvas.DrawLabel(view.title, static_cast<int>(kSettingsTextureWidth) / 2, 52,
                     view.title.size() > 18 ? 2 : 3, theme::kTextPrimary);

    // Close button (×) on the far right
    canvas.FillRoundedRect(static_cast<int>(kSettingsTextureWidth) - 64, 32, 44, 36, 8, theme::kBgElevated);
    canvas.DrawIconClose(static_cast<int>(kSettingsTextureWidth) - 64 + 22, 32 + 18, theme::kTextPrimary);

    const bool root = view.title == "PHONECAST SETTINGS";
    if (root) {
        canvas.DrawLabel("DISPLAY", 58, 114, 1, theme::kTextSecondary);
        constexpr int firstCenter = 152;
        constexpr int rowStep = 76;
        for (std::size_t index = 0; index < 7 && index < view.labels.size(); ++index) {
            const int centerY = firstCenter + static_cast<int>(index) * rowStep;
            const bool isSelected = index == view.selectedIndex;
            canvas.FillRoundedRect(16, centerY - 32, static_cast<int>(kSettingsTextureWidth) - 32, 64, 12,
                                  isSelected ? theme::kBgHover : theme::kBgElevated);
            if (isSelected) {
                canvas.FillRoundedRect(16, centerY - 20, 4, 40, 2, theme::kAccentCyan);
            }
            canvas.DrawLabel(view.labels[index], 158, centerY, view.labels[index].size() > 15 ? 1 : 2, theme::kTextPrimary);
            if (index < view.values.size()) {
                canvas.DrawLabel(view.values[index], 390, centerY, view.values[index].size() > 12 ? 1 : 2, theme::kTextSecondary);
            }
            canvas.DrawIconChevronRight(474, centerY, theme::kTextSecondary);
        }
        const bool aboutSelected = view.selectedIndex == 10;
        canvas.FillRoundedRect(16, 672, 480, 46, 12,
                              aboutSelected ? theme::kBgHover : theme::kBgElevated);
        if (aboutSelected) {
            canvas.FillRoundedRect(16, 675, 4, 40, 2, theme::kAccentCyan);
        }
        canvas.DrawLabel("ABOUT / EXPORT LOGS", 256, 695, 2, theme::kTextPrimary);
        constexpr std::array<int, 3> actionLeft{16, 174, 332};
        constexpr std::array<int, 3> actionWidth{142, 142, 164};
        for (std::size_t action = 0; action < 3; ++action) {
            const std::size_t index = action + 7;
            const bool isActionSelected = index == view.selectedIndex;
            canvas.FillRoundedRect(actionLeft[action], 734, actionWidth[action], 78, 14,
                                  isActionSelected
                                      ? (index == 9 ? theme::kAccentCyan : theme::kBgHover)
                                      : theme::kBgElevated);
            const auto color = index == 7 ? theme::kDanger : (index == 9 && isActionSelected ? theme::kBgDark : theme::kTextPrimary);
            canvas.DrawLabel(view.labels[index], actionLeft[action] + actionWidth[action] / 2, 773,
                            view.labels[index].size() > 7 ? 1 : 2, color);
            if (index == 7 && !view.values[index].empty()) {
                canvas.DrawLabel(view.values[index], actionLeft[action] + actionWidth[action] / 2, 796, 1, theme::kDanger);
            }
        }
    } else {
        constexpr int rowHeight = 82;
        constexpr int firstRow = 126;
        for (std::size_t index = 0; index < view.labels.size(); ++index) {
            const int centerY = firstRow + static_cast<int>(index) * rowHeight;
            const bool isSelected = index == view.selectedIndex;
            canvas.FillRoundedRect(16, centerY - 34, static_cast<int>(kSettingsTextureWidth) - 32, 68, 12,
                                  isSelected ? theme::kBgHover : theme::kBgElevated);
            if (isSelected) {
                canvas.FillRoundedRect(16, centerY - 22, 4, 44, 2, theme::kAccentCyan);
            }
            canvas.DrawLabel(view.labels[index], 152, centerY - 12, view.labels[index].size() > 15 ? 1 : 2, theme::kTextPrimary);
            const bool adjustable = view.title != "ABOUT PHONECAST" && index < view.values.size() &&
                                    !view.values[index].empty() && view.values[index] != ">";
            if (index < view.values.size()) {
                if (view.values[index] == ">") {
                    canvas.DrawIconChevronRight(464, centerY - 12, theme::kTextSecondary);
                } else {
                    canvas.DrawLabel(view.values[index], 372, centerY - 12, view.values[index].size() > 13 ? 1 : 2, theme::kAccentCyan);
                }
            }
            if (adjustable) {
                canvas.FillRoundedRect(18, centerY + 2, 54, 30, 8, theme::kBgSurface);
                canvas.FillRoundedRect(440, centerY + 2, 54, 30, 8, theme::kBgSurface);
                canvas.DrawIconMinus(45, centerY + 17, theme::kTextPrimary);
                canvas.DrawIconPlus(467, centerY + 17, theme::kTextPrimary);
                if (index < view.normalizedValues.size() && view.normalizedValues[index] >= 0.0F) {
                    constexpr int trackLeft = 88;
                    constexpr int trackWidth = 336;
                    canvas.FillRoundedRect(trackLeft, centerY + 13, trackWidth, 8, 4, theme::kBgSurface);
                    const int knobX = trackLeft + static_cast<int>(
                        Clamp(view.normalizedValues[index], 0.0F, 1.0F) * trackWidth);
                    canvas.FillRoundedRect(knobX - 8, centerY + 5, 16, 24, 7, theme::kAccentCyan);
                }
            }
        }
    }

    // ROW 2: Dedicated Grab & Resize Bar matching the Phone Window
    constexpr int grabRowY = static_cast<int>(kSettingsTextureHeight) - 70;
    constexpr int grabCenterY = grabRowY + 35;
    canvas.FillRect(0, grabRowY, static_cast<int>(kSettingsTextureWidth), 70, theme::kBgDark);
    canvas.FillRect(0, grabRowY, static_cast<int>(kSettingsTextureWidth), 1, theme::kBorderSubtle);

    constexpr int handleWidth = 180;
    constexpr int handleHeight = 10;
    canvas.DrawIconGrabHandle(static_cast<int>(kSettingsTextureWidth) / 2, grabCenterY,
                             handleWidth, handleHeight, 5, theme::kHandleNormal);

    // Resize button in bottom-right corner of Row 2
    constexpr int resizeLeft = static_cast<int>(kSettingsTextureWidth) - 44;
    constexpr int resizeY = grabRowY + (70 - 32) / 2;
    canvas.FillRoundedRect(resizeLeft, resizeY, 32, 32, 6, theme::kBgElevated);
    canvas.DrawIconResize(resizeLeft + 24, resizeY + 24, theme::kTextPrimary);

    return canvas.Pixels();
}

std::vector<std::uint8_t> RenderDashboard(
        bool currentlyVisible, PlacementMode placementMode,
        bool remoteStatusKnown, bool remoteAppEnabled,
        bool remoteAccessibilityEnabled, const std::string& pairingCode,
        int hoveredCell) {
    UiCanvas canvas(kDashboardTextureWidth, kDashboardTextureHeight);

    // Main window container (#070D17 Deep Navy container)
    canvas.FillRoundedRect(4, 4, 1016, 592, 24, theme::kBgDark);

    // Top Header surface (#0B1522 Dark Navy surface)
    canvas.FillRoundedRect(8, 8, 1008, 96, 20, theme::kBgSurface);
    // Top cyan accent line (#2ABCFB)
    canvas.FillRect(8, 8, 1008, 2, theme::kAccentGlow);

    canvas.DrawLabel("PHONECAST", 150, 52, 4, theme::kTextPrimary);

    const std::string pairStatus = pairingCode.empty()
        ? "PAIRING CODE PENDING" : "PAIRED  CODE " + pairingCode;
    canvas.DrawLabel(pairStatus, 482, 52, 2, theme::kTextSecondary);

    // Visibility status badge
    canvas.FillRoundedRect(790, 24, 202, 54, 25, theme::kBgElevated);
    canvas.DrawLabel(currentlyVisible ? "PHONE VISIBLE" : "PHONE HIDDEN",
                     891, 51, 2, currentlyVisible ? theme::kAccentCyan : theme::kWarning);

    // Large SHOW/HIDE card (target 0)
    const auto& show = kDashboardTargets[0];
    const bool showHovered = hoveredCell == 0;
    canvas.FillRoundedRect(show.left, show.top, show.width, show.height, 22,
                          showHovered ? theme::kBgHover : theme::kBgElevated);
    if (currentlyVisible) {
        canvas.FillRoundedRect(show.left, show.top + 24, 4, show.height - 48, 2, theme::kAccentCyan);
    }
    canvas.DrawLabel(currentlyVisible ? "HIDE PHONE" : "SHOW PHONE",
                     show.left + show.width / 2, show.top + 70, 4, theme::kTextPrimary);
    canvas.DrawLabel(currentlyVisible ? "KEEP PLACEMENT" : "OPEN BESIDE MENU",
                     show.left + show.width / 2, show.top + 120, 2, theme::kTextSecondary);

    // Section header
    canvas.DrawLabel("PLACEMENT", 490, 126, 2, theme::kTextSecondary);

    // Placement Mode buttons (targets 1, 2, 3, 5, 6)
    constexpr std::array<std::size_t, 5> placementTargets{1, 2, 3, 5, 6};
    for (std::size_t index = 0; index < placementTargets.size(); ++index) {
        const auto& target = kDashboardTargets[placementTargets[index]];
        const bool selected =
            (index == 0 && placementMode == PlacementMode::HeadLocked) ||
            (index == 1 && placementMode == PlacementMode::WorldLocked) ||
            (index == 2 && placementMode == PlacementMode::PhoneTracked) ||
            (index == 3 && placementMode == PlacementMode::LeftControllerLocked) ||
            (index == 4 && placementMode == PlacementMode::RightControllerLocked);
        const bool hovered = hoveredCell >= 0 && static_cast<std::size_t>(hoveredCell) == placementTargets[index];

        const auto cardColor = selected ? theme::kAccentCyan : (hovered ? theme::kBgHover : theme::kBgElevated);
        const auto textColor = selected ? theme::kBgDark : theme::kTextPrimary;
        const auto iconColor = selected ? theme::kBgDark : (hovered ? theme::kAccentCyan : theme::kTextPrimary);

        canvas.FillRoundedRect(target.left, target.top, target.width, target.height, 16, cardColor);

        const int centerY = target.top + target.height / 2;
        if (index == 0) { // HEAD
            canvas.DrawIconHeadset(target.left + 36, centerY, iconColor);
            canvas.DrawLabel("HEAD", target.left + 110, centerY, 3, textColor);
        } else if (index == 1) { // WORLD
            canvas.DrawIconWorld(target.left + 36, centerY, iconColor);
            canvas.DrawLabel("WORLD", target.left + 110, centerY, 3, textColor);
        } else if (index == 2) { // PHONE TRACK
            canvas.DrawIconPhoneTrack(target.left + 34, centerY, iconColor);
            canvas.DrawLabel("TRACKED", target.left + 114, centerY, 2, textColor);
        } else if (index == 3) { // LEFT DOCK
            canvas.DrawIconController(target.left + 44, centerY, true, iconColor);
            canvas.DrawLabel("LEFT DOCK", target.left + 160, centerY, 2, textColor);
        } else if (index == 4) { // RIGHT DOCK
            canvas.DrawIconController(target.left + 44, centerY, false, iconColor);
            canvas.DrawLabel("RIGHT DOCK", target.left + 160, centerY, 2, textColor);
        }
    }

    canvas.DrawLabel("CLOSE DASHBOARD FOR HOTKEYS", 204, 350, 2, theme::kAccentCyan);
    canvas.DrawLabel("HOLD EITHER STICK", 204, 382, 2, theme::kTextSecondary);
    canvas.DrawLabel("TOGGLE PHONE", 204, 414, 2, theme::kTextSecondary);
    canvas.DrawLabel("DOUBLE CLICK DOCK", 204, 446, 2, theme::kTextSecondary);

    const auto& settings = kDashboardTargets[7];
    const auto& quit = kDashboardTargets[8];
    const bool settingsHovered = hoveredCell == 7;
    const bool quitHovered = hoveredCell == 8;

    canvas.FillRoundedRect(settings.left, settings.top, settings.width, settings.height, 18,
                          settingsHovered ? theme::kBgHover : theme::kBgElevated);
    canvas.DrawIconSettings(settings.left + 54, settings.top + settings.height / 2,
                            settingsHovered ? theme::kAccentCyan : theme::kTextPrimary);
    canvas.DrawLabel("SETTINGS", settings.left + 190, settings.top + settings.height / 2, 3, theme::kTextPrimary);

    canvas.FillRoundedRect(quit.left, quit.top, quit.width, quit.height, 18,
                          quitHovered ? theme::kCloseHover : theme::kBgElevated);
    canvas.DrawIconClose(quit.left + 44, quit.top + quit.height / 2, quitHovered ? theme::kTextPrimary : theme::kDanger);
    canvas.DrawLabel("QUIT", quit.left + 114, quit.top + quit.height / 2, 3, quitHovered ? theme::kTextPrimary : theme::kDanger);

    std::string controlStatus = "CONTROL STATUS PENDING";
    if (remoteStatusKnown) {
        if (remoteAppEnabled && remoteAccessibilityEnabled) {
            controlStatus = "REMOTE CONTROL READY";
        } else if (!remoteAppEnabled && !remoteAccessibilityEnabled) {
            controlStatus = "ENABLE BOTH CONTROL GATES";
        } else if (!remoteAppEnabled) {
            controlStatus = "ENABLE APP CONTROL";
        } else {
            controlStatus = "ENABLE ACCESSIBILITY";
        }
    }
    canvas.DrawLabel(controlStatus, 512, 552, 2,
                     remoteStatusKnown && remoteAppEnabled && remoteAccessibilityEnabled
                         ? theme::kAccentCyan : theme::kWarning);

    // Decorative Signature Grab Handle at bottom
    canvas.DrawIconGrabHandle(kDashboardTextureWidth / 2, 578, 200, 8, 4, theme::kBorderSubtle);

    return canvas.Pixels();
}

std::vector<std::uint8_t> RenderDashboardThumbnail() {
    constexpr ColorRgba background{8, 24, 40, 255};
    constexpr ColorRgba phone{31, 147, 220, 255};
    constexpr ColorRgba screen{11, 18, 28, 255};
    constexpr ColorRgba text{245, 249, 255, 255};

    UiCanvas canvas(kDashboardThumbnailSize, kDashboardThumbnailSize);
    canvas.FillRect(0, 0, static_cast<int>(kDashboardThumbnailSize), static_cast<int>(kDashboardThumbnailSize), background);
    canvas.FillRect(63, 20, 130, 176, phone);
    canvas.FillRect(73, 32, 110, 144, screen);
    canvas.DrawLabel("PC", 128, 104, 8, text);
    canvas.DrawLabel("PHONECAST", 128, 224, 3, text);
    return canvas.Pixels();
}

int HitTestDashboard(float mouseX, float mouseY) {
    const int x = static_cast<int>(mouseX);
    const int y = static_cast<int>(kDashboardTextureHeight - mouseY);
    for (std::size_t index = 0; index < kDashboardTargets.size(); ++index) {
        const auto& target = kDashboardTargets[index];
        if (x >= target.left && x < target.left + target.width &&
            y >= target.top && y < target.top + target.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::vector<std::uint8_t> RenderRadialMenu(int selected, bool currentlyVisible) {
    constexpr int margin = 12;
    constexpr int gap = 8;
    const int cellWidth = (static_cast<int>(kMenuTextureWidth) - margin * 2 - gap) / kMenuColumns;
    const int cellHeight = (static_cast<int>(kMenuTextureHeight) - margin * 2 - gap * (kMenuRows - 1)) / kMenuRows;

    UiCanvas canvas(kMenuTextureWidth, kMenuTextureHeight);
    canvas.FillRoundedRect(4, 4, static_cast<int>(kMenuTextureWidth) - 8,
                          static_cast<int>(kMenuTextureHeight) - 8, 20, theme::kBgDark);
    canvas.FillRect(8, 8, static_cast<int>(kMenuTextureWidth) - 16, 2, theme::kAccentGlow);

    for (int index = 0; index < kMenuColumns * kMenuRows; ++index) {
        const int column = index % kMenuColumns;
        const int row = index / kMenuColumns;
        const int left = margin + column * (cellWidth + gap);
        const int top = margin + row * (cellHeight + gap);
        const bool populated = index < static_cast<int>(kMenuLabels.size());
        const bool isSelected = index == selected;
        const auto color = !populated
            ? theme::kBgDark
            : isSelected
                ? theme::kAccentCyan
                : theme::kBgElevated;
        canvas.FillRoundedRect(left, top, cellWidth, cellHeight, 14, color);
        if (!populated) continue;
        const std::string label = index == 0 && currentlyVisible ? "HIDE" : kMenuLabels[index];
        canvas.DrawLabel(label, left + cellWidth / 2, top + cellHeight / 2,
                         label.size() > 6 ? 2 : 3, isSelected ? theme::kBgDark : theme::kTextPrimary);
    }
    return canvas.Pixels();
}

int HitTestRadialMenu(float mouseX, float mouseY) {
    constexpr int margin = 12;
    constexpr int gap = 8;
    const int cellWidth = (static_cast<int>(kMenuTextureWidth) - margin * 2 - gap) / kMenuColumns;
    const int cellHeight = (static_cast<int>(kMenuTextureHeight) - margin * 2 - gap * (kMenuRows - 1)) / kMenuRows;
    const int x = static_cast<int>(mouseX);
    const int y = static_cast<int>(kMenuTextureHeight - mouseY);
    for (int row = 0; row < kMenuRows; ++row) {
        for (int column = 0; column < kMenuColumns; ++column) {
            const int left = margin + column * (cellWidth + gap);
            const int top = margin + row * (cellHeight + gap);
            if (x >= left && x < left + cellWidth && y >= top && y < top + cellHeight) {
                const int cell = row * kMenuColumns + column;
                return cell < static_cast<int>(kMenuLabels.size()) ? cell : -1;
            }
        }
    }
    return -1;
}

std::vector<std::uint8_t> RenderGestureProgress(int completedSteps) {
    UiCanvas canvas(kGestureTextureSize, kGestureTextureSize);
    const float center = static_cast<float>(kGestureTextureSize) * 0.5F;
    for (std::uint32_t y = 0; y < kGestureTextureSize; ++y) {
        for (std::uint32_t x = 0; x < kGestureTextureSize; ++x) {
            const float dx = static_cast<float>(x) - center;
            const float dy = center - static_cast<float>(y);
            const float radius = std::sqrt(dx * dx + dy * dy);
            if (radius < 43.0F || radius > 57.0F) continue;
            float angle = std::atan2(dx, dy);
            if (angle < 0.0F) angle += 2.0F * kPi;
            const int step = static_cast<int>(angle / (2.0F * kPi) * kGestureProgressSteps);
            const ColorRgba color = step < completedSteps
                ? ColorRgba{35, 175, 245, 255}
                : ColorRgba{70, 82, 98, 220};
            canvas.SetPixel(static_cast<int>(x), static_cast<int>(y), color);
        }
    }
    return canvas.Pixels();
}

std::string SanitizeNotificationLine(const std::string& value, std::size_t maximumCharacters) {
    std::string result;
    result.reserve(std::min(value.size(), maximumCharacters));
    bool previousSpace = false;
    for (const unsigned char byte : value) {
        if (result.size() >= maximumCharacters) break;
        char character = byte < 128U ? static_cast<char>(byte) : '?';
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
        if (character == ' ' && previousSpace) continue;
        result.push_back(character);
        previousSpace = character == ' ';
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::vector<std::uint8_t> RenderNotification(const core::protocol::NotificationEvent& notification) {
    constexpr ColorRgba shadow{0, 0, 0, 105};
    constexpr ColorRgba card{12, 22, 34, 250};
    constexpr ColorRgba badge{24, 112, 170, 255};
    constexpr ColorRgba accent{65, 188, 245, 255};
    constexpr ColorRgba text{245, 249, 255, 255};
    constexpr ColorRgba secondary{170, 205, 225, 255};
    constexpr ColorRgba action{19, 54, 78, 255};

    UiCanvas canvas(kNotificationTextureWidth, kNotificationTextureHeight);
    canvas.FillRoundedRect(18, 18, 738, 226, 24, shadow);
    canvas.FillRoundedRect(10, 10, 738, 226, 24, card);
    canvas.FillRoundedRect(28, 28, 70, 70, 18, badge);

    const std::string application = SanitizeNotificationLine(notification.applicationName, 28);
    const std::string badgeLabel = application.empty() ? "N" : application.substr(0, 1);
    canvas.DrawLabel(badgeLabel, 63, 63, 5, text);

    canvas.FillRoundedRect(112, 28, 610, 4, 2, accent);
    canvas.DrawLabel(application, 400, 56, 2, accent);

    const std::string title = SanitizeNotificationLine(notification.title, 38);
    const std::string body = notification.contentRedacted
        ? "CONTENT HIDDEN FOR PRIVACY"
        : SanitizeNotificationLine(notification.body, 54);
    canvas.DrawLabel(title.empty() ? "NEW NOTIFICATION" : title, 400, 111,
                     title.size() > 30 ? 2 : 3, text);
    if (!body.empty()) {
        canvas.DrawLabel(body, 400, 157, body.size() > 38 ? 1 : 2, secondary);
    }
    canvas.FillRoundedRect(230, 188, 340, 34, 13, action);
    canvas.DrawLabel(notification.actionToken == 0U ? "OPEN PHONE" : "OPEN APP",
                     400, 205, 2, accent);
    return canvas.Pixels();
}

void RenderGrabFooter(core::VideoFrame& composite, std::uint32_t phoneHeight, FooterHoverTarget hover) {
    const auto navBackground = theme::kBgSurface;
    const auto grabBackground = theme::kBgDark;
    const auto topAccent = theme::kAccentGlow;
    const auto rowDivider = theme::kBorderSubtle;
    const auto buttonNormal = theme::kBgElevated;
    const auto buttonHover = theme::kBgHover;
    const auto buttonCloseHover = theme::kCloseHover;
    const auto iconColor = theme::kTextPrimary;
    const auto handleColor = hover == FooterHoverTarget::Grab ? theme::kHandleHover : theme::kHandleNormal;

    const int phoneH = static_cast<int>(phoneHeight);
    const int navH = static_cast<int>(kNavBarHeightPixels);
    const int grabH = static_cast<int>(kGrabBarHeightPixels);
    const int compW = static_cast<int>(composite.width);

    UiCanvas canvas(composite.pixels, composite.width, composite.height);

    // 1. ROW 1: Android System Navigation Bar
    canvas.FillRect(0, phoneH, compW, navH, navBackground);
    canvas.FillRect(0, phoneH, compW, 2, topAccent);

    // 2. ROW 2: Dedicated Grab & Resize Bar
    canvas.FillRect(0, phoneH + navH, compW, grabH, grabBackground);
    canvas.FillRect(0, phoneH + navH, compW, 1, rowDivider);

    // Navigation buttons geometry
    const int btnW = 44;
    const int btnH = 36;
    const int btnRadius = 8;
    const int btnY = phoneH + (navH - btnH) / 2;
    const int centerY = btnY + btnH / 2;

    // Button 1: Android Back (<)
    const int backLeft = 8;
    const auto backBtnColor = hover == FooterHoverTarget::Back ? buttonHover : buttonNormal;
    canvas.FillRoundedRect(backLeft, btnY, btnW, btnH, btnRadius, backBtnColor);
    canvas.DrawIconBack(backLeft + btnW / 2, centerY, iconColor);

    // Button 2: Android Home (◯)
    const int homeLeft = backLeft + btnW + 6;
    if (homeLeft + btnW <= compW - 60) {
        const auto homeBtnColor = hover == FooterHoverTarget::Home ? buttonHover : buttonNormal;
        canvas.FillRoundedRect(homeLeft, btnY, btnW, btnH, btnRadius, homeBtnColor);
        canvas.DrawIconHome(homeLeft + btnW / 2, centerY, iconColor);
    }

    // Button 3: Android Recents (▢)
    const int recentsLeft = homeLeft + btnW + 6;
    if (recentsLeft + btnW <= compW - 60) {
        const auto recentsBtnColor = hover == FooterHoverTarget::Recents ? buttonHover : buttonNormal;
        canvas.FillRoundedRect(recentsLeft, btnY, btnW, btnH, btnRadius, recentsBtnColor);
        canvas.DrawIconRecents(recentsLeft + btnW / 2, centerY, iconColor);
    }

    // Button 4: SteamVR Keyboard (⌨)
    const int kbLeft = recentsLeft + btnW + 6;
    if (kbLeft + btnW <= compW - 60) {
        const auto kbBtnColor = hover == FooterHoverTarget::Keyboard ? buttonHover : buttonNormal;
        canvas.FillRoundedRect(kbLeft, btnY, btnW, btnH, btnRadius, kbBtnColor);
        canvas.DrawIconKeyboard(kbLeft + btnW / 2, centerY, iconColor);
    }

    // Button 5: Close (×) placed on the far right of the Nav Row
    const int closeLeft = std::max(0, compW - btnW - 8);
    const auto closeBtnColor = hover == FooterHoverTarget::Close ? buttonCloseHover : buttonNormal;
    canvas.FillRoundedRect(closeLeft, btnY, btnW, btnH, btnRadius, closeBtnColor);
    canvas.DrawIconClose(closeLeft + btnW / 2, centerY, iconColor);

    // ROW 2: Dedicated Grab & Resize Bar
    const int grabRowY = phoneH + navH;
    const int grabCenterY = grabRowY + grabH / 2;
    const int handleWidth = std::max(120, std::min(240, compW * 4 / 10));
    constexpr int handleHeight = 10;
    canvas.DrawIconGrabHandle(compW / 2, grabCenterY, handleWidth, handleHeight, 5, handleColor);

    // Resize button in bottom-right corner of Row 2
    const int resizeLeft = std::max(0, compW - 36);
    const int resizeY = grabRowY + (grabH - 30) / 2;
    const auto resizeBtnColor = hover == FooterHoverTarget::Resize ? buttonHover : buttonNormal;
    canvas.FillRoundedRect(resizeLeft, resizeY, 30, 30, 6, resizeBtnColor);
    canvas.DrawIconResize(resizeLeft + 22, resizeY + 22, iconColor);
}

void AddGrabFooter(const core::VideoFrame& frame, core::VideoFrame& composite, FooterHoverTarget hover) {
    composite.width = frame.width;
    composite.height = frame.height + kGrabHandleHeightPixels;
    composite.format = frame.format;
    composite.sequence = frame.sequence;
    composite.pixels.resize(static_cast<std::size_t>(composite.width) * composite.height * 4U);
    std::copy(frame.pixels.begin(), frame.pixels.end(), composite.pixels.begin());
    RenderGrabFooter(composite, frame.height, hover);
}

}  // namespace phonecast::vr::ui
