#include <algorithm>
#include <cmath>

#include "polygon_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

namespace {

using Polygon = std::vector<lv_fpoint_t>;
constexpr double geometry_epsilon = 0.00001;

double Cross(lv_fpoint_t a, lv_fpoint_t b, lv_fpoint_t point) {
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(point.y) - a.y)
        - (static_cast<double>(b.y) - a.y) * (static_cast<double>(point.x) - a.x);
}

double Distance(lv_fpoint_t a, lv_fpoint_t b) {
    return std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y);
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
    if(std::abs(area) <= geometry_epsilon)
        return false;
    // Positive winding is clockwise in screen coordinates; arcs use this winding too.
    if(area < 0)
        std::reverse(vertices.begin(), vertices.end());

    for(size_t i = 0; i < vertices.size(); ++i) {
        size_t next = (i + 1) % vertices.size();
        for(size_t j = 0; j < vertices.size(); ++j) {
            if(j != i && j != next && Cross(vertices[i], vertices[next], vertices[j]) <= geometry_epsilon)
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
            auto vertex = vertices[i];
            auto previous = vertices[(i + vertices.size() - 1) % vertices.size()];
            auto next = vertices[(i + 1) % vertices.size()];
            double dot = (static_cast<double>(previous.x) - vertex.x) * (next.x - vertex.x)
                + (static_cast<double>(previous.y) - vertex.y) * (next.y - vertex.y);
            // atan2 retains precision at the acute corners of very narrow polygons.
            double angle = std::atan2(std::abs(Cross(vertex, previous, next)), dot);
            tangents.push_back(1 / std::tan(angle / 2));
        }
        for(size_t i = 0; i < vertices.size(); ++i) {
            size_t next = (i + 1) % vertices.size();
            // Adjacent corner arcs must not consume more than their shared edge.
            radius = std::min<double>(radius, Distance(vertices[i], vertices[next])
                / (tangents[i] + tangents[next]));
        }
    }

    for(size_t i = 0; i < vertices.size(); ++i) {
        auto vertex = vertices[i];
        if(radius <= 0) {
            if(i == 0)
                lv_vector_path_move_to(path, &vertex);
            else
                lv_vector_path_line_to(path, &vertex);
            continue;
        }

        auto previous = vertices[(i + vertices.size() - 1) % vertices.size()];
        auto next = vertices[(i + 1) % vertices.size()];
        double previous_scale = radius * tangents[i] / Distance(vertex, previous);
        double next_scale = radius * tangents[i] / Distance(vertex, next);
        lv_fpoint_t start {
            static_cast<float>(vertex.x + (previous.x - vertex.x) * previous_scale),
            static_cast<float>(vertex.y + (previous.y - vertex.y) * previous_scale)
        };
        lv_fpoint_t end {
            static_cast<float>(vertex.x + (next.x - vertex.x) * next_scale),
            static_cast<float>(vertex.y + (next.y - vertex.y) * next_scale)
        };
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
        auto a = vertices[i];
        auto b = vertices[(i + 1) % vertices.size()];
        double offset = stroke * Distance(a, b);
        clipped.clear();
        auto append = [&](lv_fpoint_t point) {
            if(clipped.empty() || Distance(clipped.back(), point) > geometry_epsilon)
                clipped.push_back(point);
        };
        auto previous = inset.back();
        double previous_distance = Cross(a, b, previous) - offset;
        for(auto point : inset) {
            double distance = Cross(a, b, point) - offset;
            if((distance >= 0) != (previous_distance >= 0)) {
                double ratio = previous_distance / (previous_distance - distance);
                append({
                    static_cast<float>(previous.x + (point.x - previous.x) * ratio),
                    static_cast<float>(previous.y + (point.y - previous.y) * ratio)
                });
            }
            if(distance >= 0)
                append(point);
            previous = point;
            previous_distance = distance;
        }
        if(clipped.size() > 1 && Distance(clipped.front(), clipped.back()) <= geometry_epsilon)
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
    store.Register(WidgetPropertyType::CORNER_RAD_PX, ConfigValue { 0 }, PropertyChangeEffect::Repaint);
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
    float radius = AppendPolygon(path, vertices, corner_radius_px_);
    if(fill_mode_ == WidgetFillMode::Outline) {
        auto inset = InsetPolygon(vertices, stroke_px_);
        if(!inset.empty())
            AppendPolygon(path, inset, std::max(0.0F, radius - stroke_px_));
    }
    return 0;
}

} // namespace eerie_leap::views::widgets::basic::icons
