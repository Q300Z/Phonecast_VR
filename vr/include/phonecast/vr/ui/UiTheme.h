#pragma once

#include <array>
#include <cstdint>

namespace phonecast::vr::ui {

using ColorRgba = std::array<std::uint8_t, 4>;

namespace theme {
constexpr ColorRgba kBgDark{7, 13, 23, 252};          // #070D17 Deep Navy container
constexpr ColorRgba kBgSurface{11, 21, 34, 250};       // #0B1522 Dark Navy surface / card
constexpr ColorRgba kBgElevated{24, 40, 59, 220};      // #18283B Elevated button / item
constexpr ColorRgba kBgHover{42, 74, 112, 245};         // #2A4A70 Surface hover
constexpr ColorRgba kAccentCyan{42, 188, 251, 255};     // #2ABCFB Vibrant Cyan
constexpr ColorRgba kAccentGlow{42, 188, 251, 220};     // #2ABCFB Accent line / glow
constexpr ColorRgba kBorderSubtle{24, 40, 59, 180};     // #18283B Surface High divider
constexpr ColorRgba kTextPrimary{245, 249, 255, 255};   // #F5F9FF Crisp off-white
constexpr ColorRgba kTextSecondary{167, 187, 205, 220}; // #A7BDCD Muted slate
constexpr ColorRgba kCloseHover{180, 48, 60, 245};      // #B4303C Crimson close hover
constexpr ColorRgba kDanger{255, 92, 105, 255};        // #FF5C69 Soft red
constexpr ColorRgba kWarning{255, 187, 92, 255};       // #FFBB5C Warning orange
constexpr ColorRgba kHandleNormal{167, 187, 205, 220}; // #A7BDCD Normal grab pill
constexpr ColorRgba kHandleHover{42, 188, 251, 255};    // #2ABCFB Active grab pill
}  // namespace theme

}  // namespace phonecast::vr::ui
