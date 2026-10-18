#include <algorithm>
#include <cmath>

#include "polygon_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

namespace {

using Polygon = std::vector<lv_fpoint_t>;
constexpr double GEOMETRY_EPSILON = 0.00001;

// The polygon math runs in double; LVGL's points are float.
constexpr double ToDouble(float value) {
    return static_cast<double>(value);
}

double Cross(lv_fpoint_t a, lv_fpoint_t b, lv_fpoint_t point) {
    return (ToDouble(b.x) - ToDouble(a.x)) * (ToDouble(point.y) - ToDouble(a.y))
        - (ToDouble(b.y) - ToDouble(a.y)) * (ToDouble(point.x) - ToDouble(a.x));
}

double Distance(lv_fpoint_t a, lv_fpoint_t b) {
    return std::hypot(ToDouble(a.x) - ToDouble(b.x), ToDouble(a.y) - ToDouble(b.y));
}

bool NormalizeConvexPolygon(Polygon& vertices) {
    if(vertices.size() < 3)
        return false;

    double area = 0;
    for(size_t i = 0; i < vertices.size(); ++i) {
        if(!std::isfinite(vertices[i].x) || !std::isfinite(vertices[i].y))
            return false;
        area += Cross(vertices[0], vertices[i], vertices[(i + 1) % vertices.size()]);
    }
    if(std::abs(area) <= GEOMETRY_EPSILON)
        return false;
    // Positive winding is clockwise in screen coordinates; arcs use this winding too.
    if(area < 0)
        std::ranges::reverse(vertices);

    for(size_t i = 0; i < vertices.size(); ++i) {
        const size_t next = (i + 1) % vertices.size();
        for(size_t j = 0; j < vertices.size(); ++j) {
            if(j != i && j != next && Cross(vertices[i], vertices[next], vertices[j]) <= GEOMETRY_EPSILON)
                return false;
        }
    }
    return true;
}

float AppendPolygon(lv_vector_path_t* path, const Polygon& vertices, float radius) {
    std::vector<double> tangents;
    if(radius > 0) {
        tangents.reserve(vertices.size());
        for(size_t i = 0; i < vertices.size(); ++i) {
            const auto vertex = vertices[i];
            const auto previous = vertices[(i + vertices.size() - 1) % vertices.size()];
            const auto next = vertices[(i + 1) % vertices.size()];
            const double dot = (ToDouble(previous.x) - ToDouble(vertex.x)) * ToDouble(next.x - vertex.x)
                + (ToDouble(previous.y) - ToDouble(vertex.y)) * ToDouble(next.y - vertex.y);
            // atan2 retains precision at the acute corners of very narrow polygons.
            const double angle = std::atan2(std::abs(Cross(vertex, previous, next)), dot);
            tangents.push_back(1 / std::tan(angle / 2));
        }
        for(size_t i = 0; i < vertices.size(); ++i) {
            const size_t next = (i + 1) % vertices.size();
            // Adjacent corner arcs must not consume more than their shared edge.
            radius = static_cast<float>(
                std::min(ToDouble(radius), Distance(vertices[i], vertices[next]) / (tangents[i] + tangents[next]))
            );
        }
    }

    for(size_t i = 0; i < vertices.size(); ++i) {
        const auto vertex = vertices[i];
        if(radius <= 0) {
            if(i == 0)
                lv_vector_path_move_to(path, &vertex);
            else
                lv_vector_path_line_to(path, &vertex);
            continue;
        }

        const auto previous = vertices[(i + vertices.size() - 1) % vertices.size()];
        const auto next = vertices[(i + 1) % vertices.size()];
        const double previous_scale = ToDouble(radius) * tangents[i] / Distance(vertex, previous);
        const double next_scale = ToDouble(radius) * tangents[i] / Distance(vertex, next);
        const lv_fpoint_t start{
            static_cast<float>(ToDouble(vertex.x) + ToDouble(previous.x - vertex.x) * previous_scale),
            static_cast<float>(ToDouble(vertex.y) + ToDouble(previous.y - vertex.y) * previous_scale)
        };
        const lv_fpoint_t end{ static_cast<float>(ToDouble(vertex.x) + ToDouble(next.x - vertex.x) * next_scale),
            static_cast<float>(ToDouble(vertex.y) + ToDouble(next.y - vertex.y) * next_scale) };
        if(i == 0)
            lv_vector_path_move_to(path, &start);
        else
            lv_vector_path_line_to(path, &start);
        lv_vector_path_arc_to(path, radius, radius, 0, false, true, &end);
    }
    lv_vector_path_close(path);
    return radius;
}

Polygon InsetPolygon(const Polygon& vertices, float stroke) {
    Polygon inset = vertices;
    Polygon clipped;

    // Clip against each edge shifted inward by the stroke width. Unlike scaling
    // around a center, this keeps the stroke uniform for any convex polygon and
    // also handles edges disappearing as the outline gets thicker.
    for(size_t i = 0; i < vertices.size() && inset.size() >= 3; ++i) {
        const auto a = vertices[i];
        const auto b = vertices[(i + 1) % vertices.size()];
        const double offset = ToDouble(stroke) * Distance(a, b);
        clipped.clear();

        const auto append = [&](lv_fpoint_t point) {
            if(clipped.empty() || Distance(clipped.back(), point) > GEOMETRY_EPSILON)
                clipped.push_back(point);
        };

        auto previous = inset.back();
        double previous_distance = Cross(a, b, previous) - offset;

        for(const auto point : inset) {
            const double distance = Cross(a, b, point) - offset;
            if((distance >= 0) != (previous_distance >= 0)) {
                const double ratio = previous_distance / (previous_distance - distance);
                append(
                    { static_cast<float>(ToDouble(previous.x) + ToDouble(point.x - previous.x) * ratio),
                        static_cast<float>(ToDouble(previous.y) + ToDouble(point.y - previous.y) * ratio) }
                );
            }

            if(distance >= 0)
                append(point);

            previous = point;
            previous_distance = distance;
        }
        if(clipped.size() > 1 && Distance(clipped.front(), clipped.back()) <= GEOMETRY_EPSILON)
            clipped.pop_back();
        inset.swap(clipped);
    }

    if(!NormalizeConvexPolygon(inset))
        inset.clear();

    return inset;
}

} // namespace

void PolygonIconBase::RegisterProperties(WidgetPropertyStore& store) {
    ClosedShapeIconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::CORNER_RAD_PX, ConfigValue{ 0 }, PropertyChangeEffect::Repaint);
}

bool PolygonIconBase::ReadGeometry() {
    bool changed = ClosedShapeIconBase::ReadGeometry();
    changed |= UpdateValue(corner_radius_px_, ReadPixels(*properties_, WidgetPropertyType::CORNER_RAD_PX, 0, false));
    return changed;
}

float PolygonIconBase::BuildPath(lv_vector_path_t* path, float width, float height) const {
    auto vertices = GetVertices(width, height);

    if(!NormalizeConvexPolygon(vertices))
        return 0;

    const float radius = AppendPolygon(path, vertices, static_cast<float>(corner_radius_px_));

    if(fill_mode_ == WidgetFillMode::Outline) {
        const auto stroke = static_cast<float>(stroke_px_);
        const auto inset = InsetPolygon(vertices, stroke);
        if(!inset.empty())
            AppendPolygon(path, inset, std::max(0.0F, radius - stroke));
    }

    return 0;
}

} // namespace eerie_leap::views::widgets::basic::icons
