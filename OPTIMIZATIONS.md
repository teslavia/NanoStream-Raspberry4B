# Detection Stability Tuning (P1)

Changes applied to improve box stability and reduce jitter/duplicates:

1) Confidence threshold raised to 0.35
   - Filters out low-confidence clutter before decode.

2) De-duplication tightened
   - Near-duplicate distance from 30 → 20 px; still cap max boxes at 5.

3) Candidate cap before NMS on distribution regression path
   - Keep top 200 candidates per head before spatial filtering; reduces redundant boxes.

Context
- Model: NanoDet-m (ncnn-assets) with distribution regression layout (reg_max=7 → 8 bins per side).
- Preprocess: BGR input, mean {103.53,116.28,123.675}, norm {0.017429,0.017507,0.017125}.
- Extract heads: cls/reg pairs 792/795, 814/817, 836/839; packing layout disabled.

Expected effect
- Fewer noisy boxes, less jitter, better visual stability without changing the model weights.
