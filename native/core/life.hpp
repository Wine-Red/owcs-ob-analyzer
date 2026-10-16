#pragma once
#include <opencv2/core.hpp>

namespace owcs {
// Geometric evidence only: no hero ID, OCR result, team hue or previous state.
struct LifeEvidence {
  double ring = 0;
  double cross = 0;
  bool deathOverlay() const { return ring >= .80 || cross >= .76; }
  bool unobscured() const { return ring < .68 && cross < .65; }
};
LifeEvidence lifeEvidence(const cv::Mat &portrait);
} // namespace owcs
