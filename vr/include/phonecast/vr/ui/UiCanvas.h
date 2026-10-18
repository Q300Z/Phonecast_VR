#pragma once

#include "UiTheme.h"
#include <cstdint>
#include <string>
#include <vector>

namespace phonecast::vr::ui {

class UiCanvas {
public:
    UiCanvas(std::uint32_t width, std::uint32_t height);
    UiCanvas(std::vector<std::uint8_t>& buffer, std::uint32_t width, std::uint32_t height);

    std::uint32_t Width() const noexcept { return width_; }
    std::uint32_t Height() const noexcept { return height_; }
    std::vector<std::uint8_t>& Pixels() noexcept { return pixels_; }
    const std::vector<std::uint8_t>& Pixels() const noexcept { return pixels_; }

    void Clear(const ColorRgba& color);
    void SetPixel(int x, int y, const ColorRgba& color);
    void BlendPixel(int x, int y, const ColorRgba& color, float coverage);

    void FillRect(int x, int y, int width, int height, const ColorRgba& color);
    void FillRoundedRect(int x, int y, int width, int height, int radius, const ColorRgba& color);

    void DrawLabel(const std::string& label, int centerX, int centerY, int scale, const ColorRgba& color);
    void DrawTextLeft(const std::string& label, int startX, int startY, int scale, const ColorRgba& color);

    // Vector Geometric Icons matching Phone Window navigation & grab footer
    void DrawIconBack(int centerX, int centerY, const ColorRgba& color);
    void DrawIconHome(int centerX, int centerY, const ColorRgba& color);
    void DrawIconRecents(int centerX, int centerY, const ColorRgba& color);
    void DrawIconKeyboard(int centerX, int centerY, const ColorRgba& color);
    void DrawIconClose(int centerX, int centerY, const ColorRgba& color);
    void DrawIconGrabHandle(int centerX, int centerY, int handleWidth, int handleHeight, int radius, const ColorRgba& color);
    void DrawIconResize(int right, int bottom, const ColorRgba& color);
    void DrawIconChevronRight(int centerX, int centerY, const ColorRgba& color);
    void DrawIconMinus(int centerX, int centerY, const ColorRgba& color);
    void DrawIconPlus(int centerX, int centerY, const ColorRgba& color);
    void DrawIconHeadset(int centerX, int centerY, const ColorRgba& color);
    void DrawIconWorld(int centerX, int centerY, const ColorRgba& color);
    void DrawIconPhoneTrack(int centerX, int centerY, const ColorRgba& color);
    void DrawIconController(int centerX, int centerY, bool isLeft, const ColorRgba& color);
    void DrawIconSettings(int centerX, int centerY, const ColorRgba& color);

private:
    std::vector<std::uint8_t> ownedPixels_;
    std::vector<std::uint8_t>& pixels_;
    std::uint32_t width_;
    std::uint32_t height_;
};

}  // namespace phonecast::vr::ui
