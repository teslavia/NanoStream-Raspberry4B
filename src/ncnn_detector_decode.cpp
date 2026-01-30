#include <iostream>
#include <cmath>

#include "ncnn_detector.hpp"

float NCNNDetector::distExpect(const float* p, int bins) const {
    float sum = 0.f;
    if (bins <= 0 || bins > 16) return 0.0f;
    float maxv = p[0];
    for (int i = 1; i < bins; ++i) maxv = std::max(maxv, p[i]);
    float expbuf[16];
    for (int i = 0; i < bins; ++i) {
        float expv = std::exp(p[i] - maxv);
        expbuf[i] = expv;
        sum += expv;
    }
    float v = 0.f;
    for (int i = 0; i < bins; ++i) v += (expbuf[i] / sum) * i;
    return v;
}

bool NCNNDetector::extractHeadOutputs(ncnn::Extractor& ex,
                                      const std::string& cls,
                                      const std::string& reg,
                                      uint64_t frame_id,
                                      bool debug,
                                      ncnn::Mat& out_cls,
                                      ncnn::Mat& out_reg) const {
    int cls_ret = ex.extract(cls.c_str(), out_cls);
    int reg_ret = ex.extract(reg.c_str(), out_reg);
    bool extracted = (cls_ret == 0 && reg_ret == 0 && !out_cls.empty() && !out_reg.empty());
    if (debug && frame_id < 5) {
        std::cout << "\n[Diag] Head " << cls << "/" << reg
                  << " cls_ret=" << cls_ret << " reg_ret=" << reg_ret
                  << " cls_shape=" << out_cls.w << "x" << out_cls.h << "x" << out_cls.c
                  << " reg_shape=" << out_reg.w << "x" << out_reg.h << "x" << out_reg.c
                  << " cls_empty=" << out_cls.empty() << " reg_empty=" << out_reg.empty()
                  << std::endl;
    }
    return extracted;
}
