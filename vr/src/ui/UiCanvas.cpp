#include "phonecast/vr/ui/UiCanvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace phonecast::vr::ui {

namespace {

using Glyph = std::array<const char*, 7>;
const std::unordered_map<char, Glyph> kGlyphs{
    {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
    {'C', {"01111", "10000", "10000", "10000", "10000", "10000", "01111"}},
    {'D', {"11110", "10001", "10001", "10001", "10001", "10001", "11110"}},
    {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
    {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
    {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01110"}},
    {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'I', {"11111", "00100", "00100", "00100", "00100", "00100", "11111"}},
    {'J', {"00111", "00010", "00010", "00010", "10010", "10010", "01100"}},
    {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
    {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
    {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
    {'N', {"10001", "11001", "10101", "10011", "10001", "10001", "10001"}},
    {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
    {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
    {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
    {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
    {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
    {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
    {'W', {"10001", "10001", "10001", "10101", "10101", "10101", "01010"}},
    {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
    {'Y', {"10001", "10001", "01010", "00100", "00100", "00100", "00100"}},
    {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
    {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
    {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
    {'2', {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}},
    {'3', {"11110", "00001", "00001", "01110", "00001", "00001", "11110"}},
    {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
    {'5', {"11111", "10000", "10000", "11110", "00001", "00001", "11110"}},
    {'6', {"01110", "10000", "10000", "11110", "10001", "10001", "01110"}},
    {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
    {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
    {'9', {"01110", "10001", "10001", "01111", "00001", "00001", "01110"}},
    {'.', {"00000", "00000", "00000", "00000", "00000", "00110", "00110"}},
    {'-', {"00000", "00000", "00000", "11111", "00000", "00000", "00000"}},
    {'+', {"00000", "00100", "00100", "11111", "00100", "00100", "00000"}},
    {':', {"00000", "00110", "00110", "00000", "00110", "00110", "00000"}},
    {'?', {"01110", "10001", "00001", "00010", "00100", "00000", "00100"}},
    {'>', {"10000", "01000", "00100", "00010", "00100", "01000", "10000"}},
    {'/', {"00001", "00010", "00100", "00100", "01000", "10000", "00000"}},
    {'[', {"01110", "01000", "01000", "01000", "01000", "01000", "01110"}},
    {']', {"01110", "00010", "00010", "00010", "00010", "00010", "01110"}},
    {'(', {"00100", "01000", "10000", "10000", "10000", "01000", "00100"}},
    {')', {"00100", "00010", "00001", "00001", "00001", "00010", "00100"}},
    {'=', {"00000", "11111", "00000", "00000", "11111", "00000", "00000"}},
    {'%', {"11001", "11010", "00100", "01000", "01011", "10011", "00000"}},
    {',', {"00000", "00000", "00000", "00000", "00110", "00100", "01000"}},
    {'_', {"00000", "00000", "00000", "00000", "00000", "00000", "11111"}},
    {' ', {"00000", "00000", "00000", "00000", "00000", "00000", "00000"}}
};

}  // namespace

UiCanvas::UiCanvas(std::uint32_t width, std::uint32_t height)
    : ownedPixels_(static_cast<std::size_t>(width) * height * 4U, 0),
      pixels_(ownedPixels_),
      width_(width),
      height_(height) {}

UiCanvas::UiCanvas(std::vector<std::uint8_t>& buffer, std::uint32_t width, std::uint32_t height)
    : pixels_(buffer),
      width_(width),
      height_(height) {
    if (pixels_.size() < static_cast<std::size_t>(width_) * height_ * 4U) {
        pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4U, 0);
    }
}

void UiCanvas::Clear(const ColorRgba& color) {
    for (std::size_t i = 0; i < pixels_.size(); i += 4) {
        pixels_[i + 0] = color[0];
        pixels_[i + 1] = color[1];
        pixels_[i + 2] = color[2];
        pixels_[i + 3] = color[3];
    }
}

void UiCanvas::SetPixel(int x, int y, const ColorRgba& color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) return;
    const auto offset = (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U;
    if (color[3] == 255) {
        pixels_[offset + 0] = color[0];
        pixels_[offset + 1] = color[1];
        pixels_[offset + 2] = color[2];
        pixels_[offset + 3] = 255;
    } else {
        const float alpha = static_cast<float>(color[3]) / 255.0F;
        const float invAlpha = 1.0F - alpha;
        pixels_[offset + 0] = static_cast<std::uint8_t>(color[0] * alpha + pixels_[offset + 0] * invAlpha);
        pixels_[offset + 1] = static_cast<std::uint8_t>(color[1] * alpha + pixels_[offset + 1] * invAlpha);
        pixels_[offset + 2] = static_cast<std::uint8_t>(color[2] * alpha + pixels_[offset + 2] * invAlpha);
        pixels_[offset + 3] = static_cast<std::uint8_t>(std::min(255.0F, color[3] + pixels_[offset + 3] * invAlpha));
    }
}

void UiCanvas::BlendPixel(int x, int y, const ColorRgba& color, float coverage) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_) || coverage <= 0.005F) return;
    const auto offset = (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U;
    const float alpha = (static_cast<float>(color[3]) / 255.0F) * std::min(1.0F, coverage);
    const float invAlpha = 1.0F - alpha;
    pixels_[offset + 0] = static_cast<std::uint8_t>(color[0] * alpha + pixels_[offset + 0] * invAlpha);
    pixels_[offset + 1] = static_cast<std::uint8_t>(color[1] * alpha + pixels_[offset + 1] * invAlpha);
    pixels_[offset + 2] = static_cast<std::uint8_t>(color[2] * alpha + pixels_[offset + 2] * invAlpha);
    pixels_[offset + 3] = static_cast<std::uint8_t>(std::min(255.0F, color[3] * coverage + pixels_[offset + 3] * invAlpha));
}

void UiCanvas::FillRect(int x, int y, int width, int height, const ColorRgba& color) {
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            SetPixel(column, row, color);
        }
    }
}

void UiCanvas::FillRoundedRect(int x, int y, int width, int height, int radius, const ColorRgba& color) {
    const float r = static_cast<float>(radius);
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const int dx = column < radius ? radius - column - 1
                : column >= width - radius ? column - (width - radius) : 0;
            const int dy = row < radius ? radius - row - 1
                : row >= height - radius ? row - (height - radius) : 0;
            if (dx == 0 && dy == 0) {
                SetPixel(x + column, y + row, color);
            } else {
                const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (dist <= r - 0.75F) {
                    SetPixel(x + column, y + row, color);
                } else if (dist < r + 0.75F) {
                    const float coverage = (r + 0.75F - dist) / 1.5F;
                    BlendPixel(x + column, y + row, color, coverage);
                }
            }
        }
    }
}

void UiCanvas::DrawLabel(const std::string& label, int centerX, int centerY, int scale, const ColorRgba& color) {
    const int advance = 6 * scale;
    const int labelWidth = static_cast<int>(label.size()) * advance - scale;
    int cursorX = centerX - labelWidth / 2;
    const int originY = centerY - 7 * scale / 2;
    for (const char character : label) {
        const char upperChar = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        const auto glyph = kGlyphs.find(upperChar);
        if (glyph != kGlyphs.end()) {
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if (glyph->second[static_cast<std::size_t>(row)][column] != '1') continue;
                    for (int yy = 0; yy < scale; ++yy) {
                        for (int xx = 0; xx < scale; ++xx) {
                            SetPixel(cursorX + column * scale + xx, originY + row * scale + yy, color);
                        }
                    }
                }
            }
        }
        cursorX += advance;
    }
}

void UiCanvas::DrawTextLeft(const std::string& label, int startX, int startY, int scale, const ColorRgba& color) {
    const int advance = 6 * scale;
    int cursorX = startX;
    for (const char character : label) {
        const char upperChar = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        const auto glyph = kGlyphs.find(upperChar);
        if (glyph != kGlyphs.end()) {
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if (glyph->second[static_cast<std::size_t>(row)][column] != '1') continue;
                    for (int yy = 0; yy < scale; ++yy) {
                        for (int xx = 0; xx < scale; ++xx) {
                            SetPixel(cursorX + column * scale + xx, startY + row * scale + yy, color);
                        }
                    }
                }
            }
        }
        cursorX += advance;
    }
}

void UiCanvas::DrawIconBack(int centerX, int centerY, const ColorRgba& color) {
    for (int step = 0; step < 8; ++step) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            SetPixel(centerX - 4 + step + thickness, centerY - step, color);
            SetPixel(centerX - 4 + step + thickness, centerY + step, color);
        }
    }
}

void UiCanvas::DrawIconHome(int centerX, int centerY, const ColorRgba& color) {
    for (int dy = -8; dy <= 8; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if (r2 >= 32 && r2 <= 56) {
                SetPixel(centerX + dx, centerY + dy, color);
            }
        }
    }
}

void UiCanvas::DrawIconRecents(int centerX, int centerY, const ColorRgba& color) {
    for (int step = -6; step <= 6; ++step) {
        for (int thickness = -1; thickness <= 0; ++thickness) {
            SetPixel(centerX + step, centerY - 6 + thickness, color);
            SetPixel(centerX + step, centerY + 6 + thickness, color);
            SetPixel(centerX - 6 + thickness, centerY + step, color);
            SetPixel(centerX + 6 + thickness, centerY + step, color);
        }
    }
}

void UiCanvas::DrawIconKeyboard(int centerX, int centerY, const ColorRgba& color) {
    for (int step = -9; step <= 9; ++step) {
        SetPixel(centerX + step, centerY - 6, color);
        SetPixel(centerX + step, centerY + 6, color);
    }
    for (int step = -6; step <= 6; ++step) {
        SetPixel(centerX - 9, centerY + step, color);
        SetPixel(centerX + 9, centerY + step, color);
    }
    for (int kx : {-6, -2, 2, 6}) {
        SetPixel(centerX + kx, centerY - 2, color);
        SetPixel(centerX + kx + 1, centerY - 2, color);
    }
    for (int kx = -4; kx <= 4; ++kx) {
        SetPixel(centerX + kx, centerY + 2, color);
    }
}

void UiCanvas::DrawIconClose(int centerX, int centerY, const ColorRgba& color) {
    for (int step = -6; step <= 6; ++step) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            SetPixel(centerX + step, centerY + step + thickness, color);
            SetPixel(centerX + step, centerY - step + thickness, color);
        }
    }
}

void UiCanvas::DrawIconGrabHandle(int centerX, int centerY, int handleWidth, int handleHeight, int radius, const ColorRgba& color) {
    FillRoundedRect(centerX - handleWidth / 2, centerY - handleHeight / 2, handleWidth, handleHeight, radius, color);
}

void UiCanvas::DrawIconResize(int right, int bottom, const ColorRgba& color) {
    for (int step = 0; step < 14; ++step) {
        for (int thickness = 0; thickness < 2; ++thickness) {
            SetPixel(right - step, bottom - thickness, color);
            SetPixel(right - thickness, bottom - step, color);
        }
    }
}

void UiCanvas::DrawIconChevronRight(int centerX, int centerY, const ColorRgba& color) {
    for (int step = -6; step <= 6; ++step) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            SetPixel(centerX - 3 + std::abs(step) + thickness, centerY + step, color);
        }
    }
}

void UiCanvas::DrawIconMinus(int centerX, int centerY, const ColorRgba& color) {
    for (int dx = -7; dx <= 7; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            SetPixel(centerX + dx, centerY + dy, color);
        }
    }
}

void UiCanvas::DrawIconPlus(int centerX, int centerY, const ColorRgba& color) {
    for (int dx = -7; dx <= 7; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            SetPixel(centerX + dx, centerY + dy, color);
        }
    }
    for (int dy = -7; dy <= 7; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            SetPixel(centerX + dx, centerY + dy, color);
        }
    }
}

void UiCanvas::DrawIconHeadset(int centerX, int centerY, const ColorRgba& color) {
    FillRoundedRect(centerX - 14, centerY - 7, 28, 14, 4, color);
    for (int t = -1; t <= 1; ++t) {
        for (int dx = -20; dx < -14; ++dx) SetPixel(centerX + dx, centerY + t, color);
        for (int dx = 14; dx <= 20; ++dx) SetPixel(centerX + dx, centerY + t, color);
    }
}

void UiCanvas::DrawIconWorld(int centerX, int centerY, const ColorRgba& color) {
    for (int dy = -9; dy <= 9; ++dy) {
        for (int dx = -9; dx <= 9; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if (r2 >= 64 && r2 <= 95) SetPixel(centerX + dx, centerY + dy, color);
        }
    }
    for (int dx = -8; dx <= 8; ++dx) {
        SetPixel(centerX + dx, centerY, color);
    }
    for (int dy = -8; dy <= 8; ++dy) {
        const int mx = static_cast<int>(std::round(4.0F * std::sqrt(std::max(0.0F, 1.0F - (dy * dy) / 64.0F))));
        SetPixel(centerX - mx, centerY + dy, color);
        SetPixel(centerX + mx, centerY + dy, color);
    }
}

void UiCanvas::DrawIconPhoneTrack(int centerX, int centerY, const ColorRgba& color) {
    for (int y = -9; y <= 9; ++y) {
        SetPixel(centerX - 7, centerY + y, color);
        SetPixel(centerX + 7, centerY + y, color);
    }
    for (int x = -6; x <= 6; ++x) {
        SetPixel(centerX + x, centerY - 9, color);
        SetPixel(centerX + x, centerY + 9, color);
    }
    SetPixel(centerX, centerY + 6, color);
    SetPixel(centerX, centerY - 7, color);
    for (int d = 0; d < 4; ++d) {
        SetPixel(centerX - 11 + d, centerY - 11, color);
        SetPixel(centerX - 11, centerY - 11 + d, color);
        SetPixel(centerX + 11 - d, centerY - 11, color);
        SetPixel(centerX + 11, centerY - 11 + d, color);
        SetPixel(centerX - 11 + d, centerY + 11, color);
        SetPixel(centerX - 11, centerY + 11 - d, color);
        SetPixel(centerX + 11 - d, centerY + 11, color);
        SetPixel(centerX + 11, centerY + 11 - d, color);
    }
}

void UiCanvas::DrawIconController(int centerX, int centerY, bool isLeft, const ColorRgba& color) {
    for (int dy = -2; dy <= 9; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            SetPixel(centerX + dx, centerY + dy, color);
        }
    }
    const int ringX = isLeft ? centerX - 2 : centerX + 2;
    for (int dy = -10; dy <= -2; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
            const int r2 = dx * dx + (dy + 5) * (dy + 5);
            if (r2 >= 20 && r2 <= 36) {
                SetPixel(ringX + dx, centerY + dy, color);
            }
        }
    }
}

void UiCanvas::DrawIconSettings(int centerX, int centerY, const ColorRgba& color) {
    for (int dy = -8; dy <= 8; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if ((r2 >= 18 && r2 <= 42) || ((std::abs(dx) <= 2 || std::abs(dy) <= 2) && r2 <= 64)) {
                SetPixel(centerX + dx, centerY + dy, color);
            }
        }
    }
}

}  // namespace phonecast::vr::ui
