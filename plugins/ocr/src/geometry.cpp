#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seer_ocr {

namespace {

float distance(const PointF &p1, const PointF &p2)
{
    float dx = p1.x - p2.x;
    float dy = p1.y - p2.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Orders 4 points clockwise starting from top-left
void orderPointsClockwise(PointF pts[4])
{
    // Sort by y to separate top 2 and bottom 2
    PointF sorted[4];
    for (int i = 0; i < 4; ++i) sorted[i] = pts[i];
    std::sort(sorted, sorted + 4, [](const PointF &a, const PointF &b) {
        return a.y < b.y;
    });

    PointF tl, tr, bl, br;
    if (sorted[0].x < sorted[1].x) {
        tl = sorted[0];
        tr = sorted[1];
    }
    else {
        tl = sorted[1];
        tr = sorted[0];
    }

    if (sorted[2].x < sorted[3].x) {
        bl = sorted[2];
        br = sorted[3];
    }
    else {
        bl = sorted[3];
        br = sorted[2];
    }

    pts[0] = tl;
    pts[1] = tr;
    pts[2] = br;
    pts[3] = bl;
}

float boxCenterX(const QuadBox &box)
{
    return (box.points[0].x + box.points[1].x + box.points[2].x + box.points[3].x) * 0.25f;
}

float boxCenterY(const QuadBox &box)
{
    return (box.points[0].y + box.points[1].y + box.points[2].y + box.points[3].y) * 0.25f;
}

float boxSpanY(const QuadBox &box)
{
    float min_y = box.points[0].y;
    float max_y = box.points[0].y;
    for (int i = 1; i < 4; ++i) {
        min_y = std::min(min_y, box.points[i].y);
        max_y = std::max(max_y, box.points[i].y);
    }
    return max_y - min_y;
}

// Groups boxes into text lines first, then orders each line left-to-right.
// Sorting on a single comparator that mixes "same line" with "by y" is not a
// strict weak ordering and is undefined behaviour for std::sort.
std::vector<QuadBox> sortBoxesIntoLines(const std::vector<QuadBox> &boxes)
{
    std::vector<QuadBox> sorted = boxes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const QuadBox &a, const QuadBox &b) {
        return boxCenterY(a) < boxCenterY(b);
    });

    const auto by_x = [](const QuadBox &a, const QuadBox &b) {
        return boxCenterX(a) < boxCenterX(b);
    };

    size_t band_start = 0;
    for (size_t i = 1; i < sorted.size(); ++i) {
        const float band_cy = boxCenterY(sorted[band_start]);
        const float tolerance = std::max(boxSpanY(sorted[band_start]), boxSpanY(sorted[i])) * 0.5f;
        if (std::abs(boxCenterY(sorted[i]) - band_cy) > tolerance) {
            std::stable_sort(sorted.begin() + band_start, sorted.begin() + i, by_x);
            band_start = i;
        }
    }
    std::stable_sort(sorted.begin() + band_start, sorted.end(), by_x);

    return sorted;
}

}  // namespace

float polygonArea(const std::vector<PointF> &polygon)
{
    if (polygon.size() < 3) return 0.0f;
    float area = 0.0f;
    size_t n = polygon.size();
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        area += polygon[i].x * polygon[j].y;
        area -= polygon[j].x * polygon[i].y;
    }
    return std::abs(area) * 0.5f;
}

float polygonPerimeter(const std::vector<PointF> &polygon)
{
    if (polygon.size() < 2) return 0.0f;
    float perimeter = 0.0f;
    size_t n = polygon.size();
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        perimeter += distance(polygon[i], polygon[j]);
    }
    return perimeter;
}

bool unclipPolygon(
    const std::vector<PointF> &contour,
    float unclip_ratio,
    QuadBox &out_box)
{
    if (contour.size() < 3) {
        return false;
    }

    // Callers feed boundary pixels in BFS discovery order, not perimeter order,
    // so the polygon is not a walkable contour. Fit the oriented rectangle over
    // the point cloud first (order-independent) and inflate that rectangle.
    float best_area = std::numeric_limits<float>::max();
    float best_cos = 1.0f;
    float best_sin = 0.0f;
    float best_umin = 0.0f;
    float best_umax = 0.0f;
    float best_vmin = 0.0f;
    float best_vmax = 0.0f;

    for (int deg = 0; deg < 90; ++deg) {
        const float rad = deg * 3.14159265f / 180.0f;
        const float cos_a = std::cos(rad);
        const float sin_a = std::sin(rad);

        float min_u = std::numeric_limits<float>::max();
        float max_u = -std::numeric_limits<float>::max();
        float min_v = std::numeric_limits<float>::max();
        float max_v = -std::numeric_limits<float>::max();

        for (const auto &pt : contour) {
            const float u = pt.x * cos_a + pt.y * sin_a;
            const float v = -pt.x * sin_a + pt.y * cos_a;
            if (u < min_u) min_u = u;
            if (u > max_u) max_u = u;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }

        const float cur_area = (max_u - min_u) * (max_v - min_v);
        if (cur_area < best_area) {
            best_area = cur_area;
            best_cos  = cos_a;
            best_sin  = sin_a;
            best_umin = min_u;
            best_umax = max_u;
            best_vmin = min_v;
            best_vmax = max_v;
        }
    }

    if (best_area <= 1e-4f) {
        return false;
    }

    const float rect_w = best_umax - best_umin;
    const float rect_h = best_vmax - best_vmin;
    const float perimeter = 2.0f * (rect_w + rect_h);
    const float d = (perimeter > 1e-4f) ? (best_area * unclip_ratio / perimeter) : 0.0f;

    // Four corners in rectangle space, inflated by d on every side.
    const PointF corners_uv[4] = {
        {best_umin - d, best_vmin - d},
        {best_umax + d, best_vmin - d},
        {best_umax + d, best_vmax + d},
        {best_umin - d, best_vmax + d}
    };

    PointF corners[4];
    for (int k = 0; k < 4; ++k) {
        corners[k].x = corners_uv[k].x * best_cos - corners_uv[k].y * best_sin;
        corners[k].y = corners_uv[k].x * best_sin + corners_uv[k].y * best_cos;
    }

    orderPointsClockwise(corners);
    for (int k = 0; k < 4; ++k) {
        out_box.points[k] = corners[k];
    }
    return true;
}

bool cropTextLine(
    const ImageRgb &src,
    const QuadBox &box,
    ImageRgb &out_crop)
{
    if (src.empty()) {
        return false;
    }

    const auto &p0 = box.points[0];
    const auto &p1 = box.points[1];
    const auto &p2 = box.points[2];
    const auto &p3 = box.points[3];

    float w_top = distance(p0, p1);
    float w_bot = distance(p3, p2);
    float h_left = distance(p0, p3);
    float h_right = distance(p1, p2);

    int dst_w = static_cast<int>(std::max(w_top, w_bot) + 0.5f);
    int dst_h = static_cast<int>(std::max(h_left, h_right) + 0.5f);

    if (dst_w < 4) dst_w = 4;
    if (dst_h < 4) dst_h = 4;

    out_crop.width = dst_w;
    out_crop.height = dst_h;
    out_crop.data.resize(static_cast<size_t>(dst_w) * dst_h * 3);

    for (int y = 0; y < dst_h; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(dst_h);
        for (int x = 0; x < dst_w; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(dst_w);

            // Bilinear interpolation between corners
            float src_x = (1.0f - v) * ((1.0f - u) * p0.x + u * p1.x)
                        + v * ((1.0f - u) * p3.x + u * p2.x);
            float src_y = (1.0f - v) * ((1.0f - u) * p0.y + u * p1.y)
                        + v * ((1.0f - u) * p3.y + u * p2.y);

            int x0 = static_cast<int>(std::floor(src_x));
            int y0 = static_cast<int>(std::floor(src_y));
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            float fx = src_x - x0;
            float fy = src_y - y0;

            auto clamp_x = [&](int px) { return std::max(0, std::min(src.width - 1, px)); };
            auto clamp_y = [&](int py) { return std::max(0, std::min(src.height - 1, py)); };

            int cx0 = clamp_x(x0);
            int cx1 = clamp_x(x1);
            int cy0 = clamp_y(y0);
            int cy1 = clamp_y(y1);

            size_t idx00 = (static_cast<size_t>(cy0) * src.width + cx0) * 3;
            size_t idx10 = (static_cast<size_t>(cy0) * src.width + cx1) * 3;
            size_t idx01 = (static_cast<size_t>(cy1) * src.width + cx0) * 3;
            size_t idx11 = (static_cast<size_t>(cy1) * src.width + cx1) * 3;

            size_t dst_idx = (static_cast<size_t>(y) * dst_w + x) * 3;

            for (int c = 0; c < 3; ++c) {
                float v00 = src.data[idx00 + c];
                float v10 = src.data[idx10 + c];
                float v01 = src.data[idx01 + c];
                float v11 = src.data[idx11 + c];

                float val = (1.0f - fx) * (1.0f - fy) * v00
                          + fx * (1.0f - fy) * v10
                          + (1.0f - fx) * fy * v01
                          + fx * fy * v11;

                int ival = static_cast<int>(val + 0.5f);
                out_crop.data[dst_idx + c] = static_cast<uint8_t>(std::max(0, std::min(255, ival)));
            }
        }
    }

    return true;
}

void rotateImage180(ImageRgb &img)
{
    if (img.empty()) return;
    int w = img.width;
    int h = img.height;
    size_t total = static_cast<size_t>(w) * h;
    uint8_t *data = img.data.data();

    for (size_t i = 0; i < total / 2; ++i) {
        size_t j = total - 1 - i;
        std::swap(data[i * 3 + 0], data[j * 3 + 0]);
        std::swap(data[i * 3 + 1], data[j * 3 + 1]);
        std::swap(data[i * 3 + 2], data[j * 3 + 2]);
    }
}

std::vector<QuadBox> sortReadingOrder(const std::vector<QuadBox> &boxes)
{
    if (boxes.size() <= 1) {
        return boxes;
    }

    // Check if boxes span multiple columns (XY-cut heuristic)
    // Find min and max X
    float min_x = std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    for (const auto &box : boxes) {
        for (int i = 0; i < 4; ++i) {
            if (box.points[i].x < min_x) min_x = box.points[i].x;
            if (box.points[i].x > max_x) max_x = box.points[i].x;
        }
    }

    float total_width = max_x - min_x;
    if (total_width <= 50.0f) {
        // Single narrow line / column: top-to-bottom, then left-to-right
        return sortBoxesIntoLines(boxes);
    }

    // Detect column gaps using histogram
    const int kNumBins = 100;
    std::vector<int> x_hist(kNumBins, 0);
    for (const auto &box : boxes) {
        float b_min_x = std::min({box.points[0].x, box.points[1].x, box.points[2].x, box.points[3].x});
        float b_max_x = std::max({box.points[0].x, box.points[1].x, box.points[2].x, box.points[3].x});
        int bin_start = std::max(0, std::min(kNumBins - 1, static_cast<int>((b_min_x - min_x) / total_width * kNumBins)));
        int bin_end   = std::max(0, std::min(kNumBins - 1, static_cast<int>((b_max_x - min_x) / total_width * kNumBins)));
        for (int b = bin_start; b <= bin_end; ++b) {
            x_hist[b]++;
        }
    }

    // Look for a valley in middle bins [25..75]
    int best_split_bin = -1;
    int max_valley_width = 0;
    int cur_valley_start = -1;

    for (int b = 20; b <= 80; ++b) {
        if (x_hist[b] == 0) {
            if (cur_valley_start < 0) cur_valley_start = b;
            int width = b - cur_valley_start + 1;
            if (width > max_valley_width) {
                max_valley_width = width;
                best_split_bin = cur_valley_start + width / 2;
            }
        }
        else {
            cur_valley_start = -1;
        }
    }

    if (max_valley_width >= 3) {
        // Significant column separation found! Split into left and right columns
        float split_x = min_x + (static_cast<float>(best_split_bin) / kNumBins) * total_width;
        std::vector<QuadBox> left_col;
        std::vector<QuadBox> right_col;

        for (const auto &box : boxes) {
            float cx = (box.points[0].x + box.points[1].x + box.points[2].x + box.points[3].x) * 0.25f;
            if (cx < split_x) {
                left_col.push_back(box);
            }
            else {
                right_col.push_back(box);
            }
        }

        if (left_col.empty() || right_col.empty()) {
            // A split that leaves one side empty recurses on the unchanged box
            // set and never terminates; fall back to the line grouping.
            return sortBoxesIntoLines(boxes);
        }

        auto sorted_left = sortReadingOrder(left_col);
        auto sorted_right = sortReadingOrder(right_col);

        std::vector<QuadBox> result;
        result.reserve(boxes.size());
        result.insert(result.end(), sorted_left.begin(), sorted_left.end());
        result.insert(result.end(), sorted_right.begin(), sorted_right.end());
        return result;
    }

    return sortBoxesIntoLines(boxes);
}

}  // namespace seer_ocr
