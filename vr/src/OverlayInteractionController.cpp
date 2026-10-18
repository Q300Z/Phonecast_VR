#include "phonecast/vr/interaction/OverlayInteractionController.h"

#include <algorithm>

#include "phonecast/vr/ui/OverlayUiRenderer.h"

namespace phonecast::vr {
namespace {
float ClampUnit(float value) noexcept {
    return std::max(0.0F, std::min(1.0F, value));
}

bool InNavRow(float overlayY, std::uint32_t bottomInset) noexcept {
    if (bottomInset == 0 || overlayY < 0.0F || overlayY >= static_cast<float>(bottomInset)) return false;
    if (bottomInset == ui::kGrabHandleHeightPixels) {
        return overlayY >= static_cast<float>(ui::kGrabBarHeightPixels);
    }
    // Lower 40% is reserved for grab/resize bar; upper 60% is for navigation buttons.
    return overlayY >= static_cast<float>(bottomInset) * 0.40F;
}
}  // namespace

void OverlayInteractionController::SetSurfaceSize(std::uint32_t width,
                                                  std::uint32_t height,
                                                  std::uint32_t bottomInset) noexcept {
    width_ = std::max<std::uint32_t>(1, width);
    height_ = std::max<std::uint32_t>(1, height);
    bottomInset_ = bottomInset;
}

bool OverlayInteractionController::IsGrabHandle(float overlayY) const noexcept {
    return bottomInset_ > 0 && overlayY >= 0.0F &&
           overlayY < static_cast<float>(bottomInset_);
}

bool OverlayInteractionController::IsBackButton(float overlayX, float overlayY) const noexcept {
    if (!InNavRow(overlayY, bottomInset_)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        constexpr float backRight = static_cast<float>(ui::kNavButtonMargin + ui::kNavButtonWidth) + 3.0F;
        return overlayX >= 0.0F && overlayX < backRight;
    }
    return overlayX >= 0.0F && overlayX < static_cast<float>(bottomInset_);
}

bool OverlayInteractionController::IsHomeButton(float overlayX, float overlayY) const noexcept {
    if (!InNavRow(overlayY, bottomInset_)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        constexpr float homeLeft = static_cast<float>(ui::kNavButtonMargin + ui::kNavButtonWidth + ui::kNavButtonGap) - 3.0F;
        constexpr float homeRight = homeLeft + static_cast<float>(ui::kNavButtonWidth) + 6.0F;
        return overlayX >= homeLeft && overlayX < homeRight && homeRight <= static_cast<float>(width_);
    }
    const float left = static_cast<float>(bottomInset_);
    const float right = static_cast<float>(bottomInset_ * 2U);
    const float maxSafe = static_cast<float>(width_ > bottomInset_ * 2U ? width_ - bottomInset_ * 2U : 0U);
    return overlayX >= left && overlayX < right && right <= maxSafe;
}

bool OverlayInteractionController::IsRecentsButton(float overlayX, float overlayY) const noexcept {
    if (!InNavRow(overlayY, bottomInset_)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        constexpr float recentsLeft = static_cast<float>(ui::kNavButtonMargin + (ui::kNavButtonWidth + ui::kNavButtonGap) * 2) - 3.0F;
        constexpr float recentsRight = recentsLeft + static_cast<float>(ui::kNavButtonWidth) + 6.0F;
        return overlayX >= recentsLeft && overlayX < recentsRight && recentsRight <= static_cast<float>(width_);
    }
    const float left = static_cast<float>(bottomInset_ * 2U);
    const float right = static_cast<float>(bottomInset_ * 3U);
    const float maxSafe = static_cast<float>(width_ > bottomInset_ * 2U ? width_ - bottomInset_ * 2U : 0U);
    return overlayX >= left && overlayX < right && right <= maxSafe;
}

bool OverlayInteractionController::IsKeyboardButton(float overlayX, float overlayY) const noexcept {
    if (!InNavRow(overlayY, bottomInset_)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        constexpr float kbLeft = static_cast<float>(ui::kNavButtonMargin + (ui::kNavButtonWidth + ui::kNavButtonGap) * 3) - 3.0F;
        constexpr float kbRight = kbLeft + static_cast<float>(ui::kNavButtonWidth) + 6.0F;
        return overlayX >= kbLeft && overlayX < kbRight && kbRight <= static_cast<float>(width_);
    }
    const float left = static_cast<float>(bottomInset_ * 3U);
    const float right = static_cast<float>(bottomInset_ * 4U);
    const float maxSafe = static_cast<float>(width_ > bottomInset_ * 2U ? width_ - bottomInset_ * 2U : 0U);
    return overlayX >= left && overlayX < right && right <= maxSafe;
}

bool OverlayInteractionController::IsCloseButton(float overlayX, float overlayY) const noexcept {
    if (!InNavRow(overlayY, bottomInset_)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        const float closeLeft = static_cast<float>(width_ > 56U ? width_ - 56U : 0U);
        return overlayX >= closeLeft && overlayX <= static_cast<float>(width_);
    }
    return overlayX >= static_cast<float>(width_ > bottomInset_ * 2U ? width_ - bottomInset_ * 2U : 0U) &&
           overlayX < static_cast<float>(width_ > bottomInset_ ? width_ - bottomInset_ : 0U);
}

bool OverlayInteractionController::IsResizeButton(float overlayX, float overlayY) const noexcept {
    if (!IsGrabHandle(overlayY)) return false;
    if (bottomInset_ == ui::kGrabHandleHeightPixels) {
        const bool inBottomRow = overlayY < static_cast<float>(ui::kGrabBarHeightPixels);
        const float resizeLeft = static_cast<float>(width_ > 44U ? width_ - 44U : 0U);
        return inBottomRow && overlayX >= resizeLeft && overlayX <= static_cast<float>(width_);
    }
    return overlayX >= static_cast<float>(width_ > bottomInset_ ? width_ - bottomInset_ : 0U) &&
           overlayX < static_cast<float>(width_);
}

core::PointerEvent OverlayInteractionController::Make(core::PointerEvent::Type type,
                                                       float overlayX, float overlayY,
                                                       float scrollDelta) noexcept {
    core::PointerEvent event;
    event.type = type;
    event.normalizedX = ClampUnit(overlayX / static_cast<float>(width_));
    // OpenVR overlay events have a bottom-left origin. The renderer may place a
    // grab handle below the phone image, so remove that inset before converting
    // the phone region to Android's top-left coordinate system.
    const float phoneY = overlayY - static_cast<float>(bottomInset_);
    event.normalizedY = ClampUnit(1.0F - phoneY / static_cast<float>(height_));
    event.scrollDelta = std::max(-1.0F, std::min(1.0F, scrollDelta));
    event.sequence = nextSequence_++;
    return event;
}

core::PointerEvent OverlayInteractionController::PointerDown(float x, float y) noexcept {
    return Make(core::PointerEvent::Type::Down, x, y);
}
core::PointerEvent OverlayInteractionController::PointerMove(float x, float y) noexcept {
    return Make(core::PointerEvent::Type::Move, x, y);
}
core::PointerEvent OverlayInteractionController::PointerUp(float x, float y) noexcept {
    return Make(core::PointerEvent::Type::Up, x, y);
}
core::PointerEvent OverlayInteractionController::Scroll(float x, float y, float delta) noexcept {
    return Make(core::PointerEvent::Type::Scroll, x, y, delta);
}
core::PointerEvent OverlayInteractionController::Back() noexcept {
    return Make(core::PointerEvent::Type::Back, 0.5F * width_, 0.5F * height_);
}
core::PointerEvent OverlayInteractionController::Home() noexcept {
    return Make(core::PointerEvent::Type::Home, 0.5F * width_, 0.5F * height_);
}
core::PointerEvent OverlayInteractionController::Recents() noexcept {
    return Make(core::PointerEvent::Type::Recents, 0.5F * width_, 0.5F * height_);
}

}  // namespace phonecast::vr
