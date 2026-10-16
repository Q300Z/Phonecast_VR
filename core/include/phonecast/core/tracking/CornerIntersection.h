#pragma once

#include "phonecast/core/tracking/TrackingTypes.h"
#include <array>
#include <optional>

namespace phonecast::core::tracking {

class CornerIntersection {
public:
    // Computes the 4 virtual corners analytically by intersecting the 4 fitted border lines.
    // Returns ScreenCorners2D with valid=true if all 4 corners form a convex quadrilateral.
    [[nodiscard]] static ScreenCorners2D ComputeCorners(const ScreenBorders& borders) noexcept;

    // Computes subpixel intersection of two 2D lines: a1*x + b1*y + c1 = 0 and a2*x + b2*y + c2 = 0.
    // Returns std::nullopt if lines are degenerate or nearly parallel (|sin(theta)| < minSinAngle).
    [[nodiscard]] static std::optional<Vector2> Intersect(
        const Line2D& l1,
        const Line2D& l2,
        float minSinAngle = 0.15F) noexcept;

    // Checks whether the 4 corners form a strictly convex quadrilateral with clockwise winding
    // in screen coordinates (+Y down) and area >= minAreaPixels.
    [[nodiscard]] static bool IsConvexQuad(
        const std::array<Vector2, 4>& corners,
        float minAreaPixels = 400.0F) noexcept;
};

} // namespace phonecast::core::tracking
