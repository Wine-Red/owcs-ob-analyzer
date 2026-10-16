#pragma once
#include <opencv2/core.hpp>

namespace owcs {
// Geometric evidence only: no hero ID, OCR result, team hue or previous state.
struct LifeEvidence {
  double ring = 0;
  double cross = 0;
};
enum class LifeKind { Unknown, Alive, Dead };
struct LifeDecision {
  LifeKind kind = LifeKind::Unknown;
  double confidence = 0;
};
LifeEvidence lifeEvidence(const cv::Mat &portrait);
// Compare the normal-portrait explanation with explicit overlay evidence.
// normal requires both the full face and badge area to fit ONE template/pose.
LifeDecision classifyLife(const LifeEvidence &, double normal);
} // namespace owcs
