#include <algorithm>
#include <vector>
#include <cstring>

#include "ncnn_detector.hpp"

float NCNNDetector::calculateIoU(const Detection& a, const Detection& b) const {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.w, b.x + b.w);
    int y2 = std::min(a.y + a.h, b.y + b.h);
    int inter_w = std::max(0, x2 - x1);
    int inter_h = std::max(0, y2 - y1);
    int inter = inter_w * inter_h;
    int area_a = a.w * a.h;
    int area_b = b.w * b.h;
    int uni = area_a + area_b - inter;
    return uni > 0 ? (float)inter / (float)uni : 0.0f;
}

void NCNNDetector::applyPostFilter(const RuntimeConfig& runtime,
                                  const std::vector<Detection>& raw_dets,
                                  std::vector<Detection>& final_dets,
                                  float frame_area) const {
    auto sorted = raw_dets;
    std::sort(sorted.begin(), sorted.end(), [](const Detection& a, const Detection& b){ return a.score > b.score; });

    int per_class_count[80];
    std::memset(per_class_count, 0, sizeof(per_class_count));
    int person_max = runtime.personMax;

    auto apply_person_filters = [&](int max_person_area) {
        if (max_person_area <= 0) return;
        float ratio = runtime.personMinAreaRatio;
        float min_score = runtime.personMinScore;
        float ar_min = runtime.personArMin;
        float ar_max = runtime.personArMax;
        std::vector<Detection> filtered;
        filtered.reserve(final_dets.size());
        for (const auto& d : final_dets) {
            if (d.class_id == 0) {
                float ar = d.h > 0 ? (float)d.h / (float)d.w : 0.0f;
                if (d.w * d.h < (int)(max_person_area * ratio)) continue;
                if (d.score < min_score) continue;
                if (ar < ar_min || ar > ar_max) continue;
            }
            filtered.push_back(d);
        }
        final_dets.swap(filtered);
    };

    auto append_with_caps = [&]() {
        for (const auto& d : sorted) {
            if (final_dets.size() >= static_cast<size_t>(config.maxDetections)) break;
            if (d.class_id >= 0 && d.class_id < 80) {
                float area_norm = (d.w * d.h) / frame_area;
                int cap = (area_norm < config.capSmallAreaThreshold) ? 1
                         : (area_norm < config.capMediumAreaThreshold ? 2 : 3);
                if (d.class_id == 0 && person_max < cap) cap = person_max;
                if (per_class_count[d.class_id] >= cap) continue;
            }
            bool skip = false;
            for (const auto& f : final_dets) {
                if (d.class_id >= 0 && f.class_id >= 0 && d.class_id != f.class_id) continue;
                if (calculateIoU(d, f) > config.iouThreshold) { skip = true; break; }
            }
            if (!skip) {
                if (d.class_id >= 0 && d.class_id < 80) per_class_count[d.class_id] += 1;
                final_dets.push_back(d);
            }
        }
    };

    int max_person_area = 0;
    for (const auto& d : final_dets) {
        if (d.class_id == 0) {
            int area = d.w * d.h;
            if (area > max_person_area) max_person_area = area;
        }
    }
    apply_person_filters(max_person_area);
    append_with_caps();
}

void NCNNDetector::smoothDetections(std::vector<Detection>& final_dets) {
    std::vector<Detection> smoothed;
    smoothed.reserve(final_dets.size());
    for (const auto& cur : final_dets) {
        const Detection* best = nullptr;
        float best_iou = 0.0f;
        for (const auto& prev : prev_detections) {
            if (cur.class_id >= 0 && prev.class_id >= 0 && cur.class_id != prev.class_id) continue;
            float v = calculateIoU(cur, prev);
            if (v > best_iou) { best_iou = v; best = &prev; }
        }
        Detection d = cur;
        if (best && best_iou >= config.iouThreshold) {
            d.x = (int)(config.emaAlpha * cur.x + (1.0f - config.emaAlpha) * best->x);
            d.y = (int)(config.emaAlpha * cur.y + (1.0f - config.emaAlpha) * best->y);
            d.w = (int)(config.emaAlpha * cur.w + (1.0f - config.emaAlpha) * best->w);
            d.h = (int)(config.emaAlpha * cur.h + (1.0f - config.emaAlpha) * best->h);
        }
        smoothed.push_back(d);
    }
    final_dets.swap(smoothed);
}
