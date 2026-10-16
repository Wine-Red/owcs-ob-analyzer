#include "life.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace owcs {
LifeDecision classifyLife(const LifeEvidence &overlay, double normal) {
  // A circle/X explained by the unchanged portrait is not an overlay.
  // The same acceptance floor as foreground portrait matching applies to
  // every independently checked region, without a hero-specific exception.
  if (normal >= .72)
    return {LifeKind::Alive, normal};
  if (overlay.ring >= .80 || overlay.cross >= .76)
    return {LifeKind::Dead, std::max(overlay.ring, overlay.cross)};
  return {};
}
namespace {
cv::Vec3f sample(const cv::Mat &im, float x, float y) {
  int ix = int(x), iy = int(y);
  float fx = x - ix, fy = y - iy;
  return im.at<cv::Vec3f>(iy, ix) * ((1 - fx) * (1 - fy)) +
         im.at<cv::Vec3f>(iy, ix + 1) * (fx * (1 - fy)) +
         im.at<cv::Vec3f>(iy + 1, ix) * ((1 - fx) * fy) +
         im.at<cv::Vec3f>(iy + 1, ix + 1) * (fx * fy);
}
} // namespace
LifeEvidence lifeEvidence(const cv::Mat &portrait) {
  LifeEvidence out;
  if (portrait.empty())
    return out;
  cv::Mat face, color;
  cv::resize(portrait, face, {54, 54});
  face.convertTo(color, CV_32F);
  // A real overlay has radially oriented contrast at one shared radius.
  // Searching arbitrary edge pixels throughout an annulus also accepts faces.
  for (int cy : {26, 27, 28})
    for (int cx : {26, 27, 28}) {
      bool hits[18][64]{};
      for (int r = 0; r < 18; ++r)
        for (int k = 0; k < 64; ++k) {
          float angle = float(k * 2 * CV_PI / 64), co = std::cos(angle), si = std::sin(angle);
          float radius = 10 + r * .5f, x = cx + radius * co, y = cy + radius * si;
          auto radial = sample(color, x + co, y + si) - sample(color, x - co, y - si);
          auto tangent = sample(color, x - si, y + co) - sample(color, x + si, y - co);
          double strength = radial.dot(radial), side = tangent.dot(tangent);
          hits[r][k] = strength > 45 * 45 && strength > 4 * side;
        }
      for (int r = 1; r < 17; ++r) {
        int count = 0;
        for (int k = 0; k < 64; ++k)
          count += hits[r - 1][k] || hits[r][k] || hits[r + 1][k];
        out.ring = std::max(out.ring, count / 64.);
      }
    }
  // The initial death animation is a dark X on a light skull, before a timer.
  // Use only its central X so the changing skull silhouette and team colors
  // do not enter the comparison. Sizes cover the brief expansion animation.
  cv::Mat gray, pattern(32, 32, CV_32F), dark(32, 32, CV_8U);
  cv::cvtColor(face, gray, cv::COLOR_BGR2GRAY);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      float nx = (x - 15.5f) / 16, ny = (y - 15.5f) / 16;
      bool arm = std::abs(nx - ny) < .38 || std::abs(nx + ny) < .38;
      pattern.at<float>(y, x) = arm ? 0.f : 1.f;
      dark.at<uchar>(y, x) = arm ? 255 : 0;
    }
  cv::Mat light = 255 - dark;
  for (int size = 12; size <= 42; size += 2) {
    // Render the glyph through the same sampling footprint as the observed
    // patch. Comparing a tiny, interpolated X to a sharp 32px binary X loses
    // correlation even when the geometry is identical.
    const int sampledSize =
        std::max(2, int(std::nearbyint(size * std::min(1., portrait.cols / 54.))));
    cv::Mat sampled, reference;
    cv::resize(pattern, sampled, {sampledSize, sampledSize}, 0, 0, cv::INTER_AREA);
    cv::resize(sampled, reference, {size, size});
    cv::resize(reference, reference, {32, 32});
    // Search a complete bounded 2D neighborhood. Cardinal-only offsets miss
    // diagonal rounding shifts after an OBS preview has been resampled.
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) {
        int a = 27 - size / 2;
        cv::Mat patch, score;
        cv::resize(gray(cv::Rect(a + dx, a + dy, size, size)), patch, {32, 32});
        patch.convertTo(patch, CV_32F);
        if (cv::mean(patch, light)[0] - cv::mean(patch, dark)[0] <= 40)
          continue;
        cv::matchTemplate(patch, reference, score, cv::TM_CCOEFF_NORMED);
        out.cross = std::max(out.cross, double(score.at<float>(0)));
      }
  }
  return out;
}
} // namespace owcs
