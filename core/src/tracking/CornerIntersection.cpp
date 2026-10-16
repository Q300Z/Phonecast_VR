#include "phonecast/core/tracking/CornerIntersection.h"

#include <cmath>

namespace phonecast::core::tracking {

std::optional<Vector2> CornerIntersection::Intersect(
    const Line2D& l1,
    const Line2D& l2,
    float minSinAngle) noexcept {
    if (!l1.valid || !l2.valid) {
        return std::nullopt;
    }

    if (!std::isfinite(l1.a) || !std::isfinite(l1.b) || !std::isfinite(l1.c) ||
        !std::isfinite(l2.a) || !std::isfinite(l2.b) || !std::isfinite(l2.c) ||
        !std::isfinite(minSinAngle) || minSinAngle < 0.0F) {
        return std::nullopt;
    }

    const double a1 = static_cast<double>(l1.a);
    const double b1 = static_cast<double>(l1.b);
    const double c1 = static_cast<double>(l1.c);

    const double a2 = static_cast<double>(l2.a);
    const double b2 = static_cast<double>(l2.b);
    const double c2 = static_cast<double>(l2.c);

    const double n1 = std::sqrt(a1 * a1 + b1 * b1);
    const double n2 = std::sqrt(a2 * a2 + b2 * b2);
    if (!std::isfinite(n1) || !std::isfinite(n2) || n1 < 1e-7 || n2 < 1e-7) {
        return std::nullopt;
    }

    const double na1 = a1 / n1;
    const double nb1 = b1 / n1;
    const double nc1 = c1 / n1;

    const double na2 = a2 / n2;
    const double nb2 = b2 / n2;
    const double nc2 = c2 / n2;

    const double det = na1 * nb2 - nb1 * na2;
    if (!std::isfinite(det) || std::abs(det) < static_cast<double>(minSinAngle)) {
        return std::nullopt; // Parallel or nearly parallel lines
    }

    const double x = (nb1 * nc2 - nc1 * nb2) / det;
    const double y = (nc1 * na2 - na1 * nc2) / det;

    if (!std::isfinite(x) || !std::isfinite(y)) {
        return std::nullopt;
    }

    return Vector2{static_cast<float>(x), static_cast<float>(y)};
}

bool CornerIntersection::IsConvexQuad(
    const std::array<Vector2, 4>& corners,
    float minAreaPixels) noexcept {
    for (const auto& c : corners) {
        if (!std::isfinite(c.x) || !std::isfinite(c.y)) {
            return false;
        }
    }
    if (!std::isfinite(minAreaPixels) || minAreaPixels < 0.0F) {
        return false;
    }

    bool hasPositive = false;
    bool hasNegative = false;

    for (size_t i = 0; i < 4; ++i) {
        const size_t next = (i + 1) % 4;
        const size_t next2 = (i + 2) % 4;

        const double dx1 = static_cast<double>(corners[next].x - corners[i].x);
        const double dy1 = static_cast<double>(corners[next].y - corners[i].y);
        const double dx2 = static_cast<double>(corners[next2].x - corners[next].x);
        const double dy2 = static_cast<double>(corners[next2].y - corners[next].y);

        const double cross = dx1 * dy2 - dy1 * dx2;
        if (!std::isfinite(cross)) {
            return false;
        }
        if (cross > 1e-5) {
            hasPositive = true;
        } else if (cross < -1e-5) {
            hasNegative = true;
        } else {
            // Three collinear vertices or sharp degenerate fold
            return false;
        }
    }

    // In screen coordinates (+Y down), clockwise order requires strictly positive cross products.
    // If hasNegative is true, the quad has reflex angles or inverted (counter-clockwise) winding.
    // If hasPositive is false, all cross products were non-positive.
    if (!hasPositive || hasNegative) {
        return false; // Non-convex, self-intersecting, or inverted/counter-clockwise
    }

    // Shoelace formula for area
    double area2 = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        const size_t next = (i + 1) % 4;
        area2 += static_cast<double>(corners[i].x * corners[next].y - corners[next].x * corners[i].y);
    }
    const double area = 0.5 * std::abs(area2);

    return std::isfinite(area) && area >= static_cast<double>(minAreaPixels);
}

ScreenCorners2D CornerIntersection::ComputeCorners(const ScreenBorders& borders) noexcept {
    ScreenCorners2D result{};

    if (!borders.IsValid()) {
        return result;
    }

    // Clockwise order:
    // [0] = Top-Left     = Top ∩ Left
    // [1] = Top-Right    = Top ∩ Right
    // [2] = Bottom-Right = Bottom ∩ Right
    // [3] = Bottom-Left  = Bottom ∩ Left
    const auto tl = Intersect(borders.top, borders.left);
    const auto tr = Intersect(borders.top, borders.right);
    const auto br = Intersect(borders.bottom, borders.right);
    const auto bl = Intersect(borders.bottom, borders.left);

    if (!tl.has_value() || !tr.has_value() || !br.has_value() || !bl.has_value()) {
        return result;
    }

    result.corners[0] = *tl;
    result.corners[1] = *tr;
    result.corners[2] = *br;
    result.corners[3] = *bl;

    result.valid = IsConvexQuad(result.corners);
    return result;
}

} // namespace phonecast::core::tracking
