#pragma once

#include <vector>
#include <cmath>
#include "image_loader.h"

namespace seer_ocr {

struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct QuadBox {
    PointF points[4]; // 0: TL, 1: TR, 2: BR, 3: BL
    float score = 0.0f;
};

struct OcrLineResult {
    int index       = 0;
    std::string text;
    float score     = 0.0f;
    int orientation = 0; // 0 or 180
    int box[4][2]   = {{0,0},{0,0},{0,0},{0,0}}; // [[x1,y1],[x2,y2],[x3,y3],[x4,y4]] in screenshot px
};

// Computes polygon area
float polygonArea(const std::vector<PointF> &polygon);

// Computes polygon perimeter
float polygonPerimeter(const std::vector<PointF> &polygon);

// Unclips (dilates) polygon by unclip_ratio (default 1.5) and fits minimum oriented bounding box
bool unclipPolygon(
    const std::vector<PointF> &contour,
    float unclip_ratio,
    QuadBox &out_box);

// Crops text line image from source image using quadbox perspective mapping
bool cropTextLine(
    const ImageRgb &src,
    const QuadBox &box,
    ImageRgb &out_crop);

// Rotates an image in-place by 180 degrees
void rotateImage180(ImageRgb &img);

// Sorts detected quad boxes into natural reading order (XY-cut with column awareness)
std::vector<QuadBox> sortReadingOrder(const std::vector<QuadBox> &boxes);

}  // namespace seer_ocr
