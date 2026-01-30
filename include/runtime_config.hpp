#pragma once

#include <string>

struct RuntimeConfig {
    bool thermalEnabled = false;
    bool debug = false;
    int thermalHigh = 75000;
    int thermalCrit = 80000;
    int thermalSleepMs = 100;

    bool useDmabuf = false;
    bool useInt8 = false;
    std::string int8Param = "models/nanodet_m-int8.param";
    std::string int8Bin = "models/nanodet_m-int8.bin";

    bool showLabels = true;
    float personMinScore = 0.55f;
    int personMax = 2;
    float personMinAreaRatio = 0.6f;
    float personArMin = 0.6f;
    float personArMax = 2.5f;

    std::string rtspHost;

    // Detector overrides (optional)
    int detInputWidth = 0;
    int detInputHeight = 0;
    int detTopK = 0;
    int detMaxDetections = 0;
    float detBaseScore = 0.0f;
    float detMinScoreSmallArea = 0.0f;
    float detMinScoreMediumArea = 0.0f;
    float detMinScoreSmallAreaThreshold = 0.0f;
    float detMinScoreMediumAreaThreshold = 0.0f;
    float detCapSmallAreaThreshold = 0.0f;
    float detCapMediumAreaThreshold = 0.0f;
    float detIouThreshold = 0.0f;
    float detEmaAlpha = 0.0f;
    int detMinBoxArea = 0;
    std::string detHeads;
};

RuntimeConfig loadRuntimeConfig();
const RuntimeConfig& getRuntimeConfig();
std::string formatRuntimeConfig(const RuntimeConfig& cfg);
std::string resolveRtspHost(const RuntimeConfig& cfg);
