#include "runtime_config.hpp"

#include <cstdlib>
#include <string>
#include <sstream>

#include "net_util.hpp"

namespace {

bool envEnabled(const char* name) {
    const char* v = std::getenv(name);
    return v && std::string(v) == "1";
}

int envInt(const char* name, int default_value) {
    if (const char* v = std::getenv(name)) {
        return std::atoi(v);
    }
    return default_value;
}

float envFloat(const char* name, float default_value) {
    if (const char* v = std::getenv(name)) {
        return std::atof(v);
    }
    return default_value;
}

}

RuntimeConfig loadRuntimeConfig() {
    RuntimeConfig cfg;

    cfg.thermalEnabled = envEnabled("NANOSTREAM_THERMAL");
    cfg.debug = envEnabled("NANOSTREAM_DEBUG");
    cfg.thermalHigh = envInt("NANOSTREAM_THERMAL_HIGH", cfg.thermalHigh);
    cfg.thermalCrit = envInt("NANOSTREAM_THERMAL_CRIT", cfg.thermalCrit);
    cfg.thermalSleepMs = envInt("NANOSTREAM_THERMAL_SLEEP", cfg.thermalSleepMs);

    cfg.useDmabuf = envEnabled("NANOSTREAM_DMABUF");
    cfg.useInt8 = envEnabled("NANOSTREAM_INT8");
    if (const char* v = std::getenv("NANOSTREAM_INT8_PARAM")) cfg.int8Param = v;
    if (const char* v = std::getenv("NANOSTREAM_INT8_BIN")) cfg.int8Bin = v;

    const char* label_env = std::getenv("NANOSTREAM_LABELS");
    cfg.showLabels = !(label_env && std::string(label_env) == "0");

    cfg.personMinScore = envFloat("NANOSTREAM_PERSON_MIN_SCORE", cfg.personMinScore);
    cfg.personMax = envInt("NANOSTREAM_PERSON_MAX", cfg.personMax);
    cfg.personMinAreaRatio = envFloat("NANOSTREAM_PERSON_MIN_AREA_RATIO", cfg.personMinAreaRatio);
    cfg.personArMin = envFloat("NANOSTREAM_PERSON_AR_MIN", cfg.personArMin);
    cfg.personArMax = envFloat("NANOSTREAM_PERSON_AR_MAX", cfg.personArMax);

    if (const char* v = std::getenv("NANOSTREAM_RTSP_HOST")) cfg.rtspHost = v;

    cfg.detInputWidth = envInt("NANOSTREAM_DET_INPUT_W", cfg.detInputWidth);
    cfg.detInputHeight = envInt("NANOSTREAM_DET_INPUT_H", cfg.detInputHeight);
    cfg.detTopK = envInt("NANOSTREAM_DET_TOPK", cfg.detTopK);
    cfg.detMaxDetections = envInt("NANOSTREAM_DET_MAX_DET", cfg.detMaxDetections);
    cfg.detMinBoxArea = envInt("NANOSTREAM_DET_MIN_BOX_AREA", cfg.detMinBoxArea);
    cfg.detBaseScore = envFloat("NANOSTREAM_DET_BASE_SCORE", cfg.detBaseScore);
    cfg.detMinScoreSmallArea = envFloat("NANOSTREAM_DET_MIN_SCORE_SMALL", cfg.detMinScoreSmallArea);
    cfg.detMinScoreMediumArea = envFloat("NANOSTREAM_DET_MIN_SCORE_MED", cfg.detMinScoreMediumArea);
    cfg.detMinScoreSmallAreaThreshold = envFloat("NANOSTREAM_DET_AREA_SMALL", cfg.detMinScoreSmallAreaThreshold);
    cfg.detMinScoreMediumAreaThreshold = envFloat("NANOSTREAM_DET_AREA_MED", cfg.detMinScoreMediumAreaThreshold);
    cfg.detCapSmallAreaThreshold = envFloat("NANOSTREAM_DET_CAP_AREA_SMALL", cfg.detCapSmallAreaThreshold);
    cfg.detCapMediumAreaThreshold = envFloat("NANOSTREAM_DET_CAP_AREA_MED", cfg.detCapMediumAreaThreshold);
    cfg.detIouThreshold = envFloat("NANOSTREAM_DET_IOU", cfg.detIouThreshold);
    cfg.detEmaAlpha = envFloat("NANOSTREAM_DET_EMA", cfg.detEmaAlpha);
    if (const char* v = std::getenv("NANOSTREAM_DET_HEADS")) cfg.detHeads = v;

    return cfg;
}

const RuntimeConfig& getRuntimeConfig() {
    static RuntimeConfig cfg = loadRuntimeConfig();
    return cfg;
}

std::string formatRuntimeConfig(const RuntimeConfig& cfg) {
    std::ostringstream out;
    out << "thermal_enabled=" << (cfg.thermalEnabled ? "1" : "0")
        << " thermal_high=" << cfg.thermalHigh
        << " thermal_crit=" << cfg.thermalCrit
        << " thermal_sleep_ms=" << cfg.thermalSleepMs
        << " debug=" << (cfg.debug ? "1" : "0")
        << " dmabuf=" << (cfg.useDmabuf ? "1" : "0")
        << " int8=" << (cfg.useInt8 ? "1" : "0")
        << " int8_param=" << cfg.int8Param
        << " int8_bin=" << cfg.int8Bin
        << " labels=" << (cfg.showLabels ? "1" : "0")
        << " person_min_score=" << cfg.personMinScore
        << " person_max=" << cfg.personMax
        << " person_min_area_ratio=" << cfg.personMinAreaRatio
        << " person_ar_min=" << cfg.personArMin
        << " person_ar_max=" << cfg.personArMax
        << " rtsp_host=" << (cfg.rtspHost.empty() ? "<device-ip>" : cfg.rtspHost)
        << " det_input_w=" << cfg.detInputWidth
        << " det_input_h=" << cfg.detInputHeight
        << " det_topk=" << cfg.detTopK
        << " det_max_det=" << cfg.detMaxDetections
        << " det_min_box_area=" << cfg.detMinBoxArea
        << " det_base_score=" << cfg.detBaseScore
        << " det_min_score_small=" << cfg.detMinScoreSmallArea
        << " det_min_score_med=" << cfg.detMinScoreMediumArea
        << " det_area_small=" << cfg.detMinScoreSmallAreaThreshold
        << " det_area_med=" << cfg.detMinScoreMediumAreaThreshold
        << " det_cap_area_small=" << cfg.detCapSmallAreaThreshold
        << " det_cap_area_med=" << cfg.detCapMediumAreaThreshold
        << " det_iou=" << cfg.detIouThreshold
        << " det_ema=" << cfg.detEmaAlpha
        << " det_heads=" << (cfg.detHeads.empty() ? "<default>" : cfg.detHeads);
    return out.str();
}

std::string resolveRtspHost(const RuntimeConfig& cfg) {
    if (!cfg.rtspHost.empty()) {
        return cfg.rtspHost;
    }
    std::string local = getLocalIPv4();
    if (!local.empty()) {
        return local;
    }
    return "<device-ip>";
}
