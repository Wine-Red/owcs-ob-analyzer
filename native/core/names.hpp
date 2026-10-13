#pragma once
#include "recognizer.hpp"
#include <memory>
namespace owcs {
class NameReader {
public:
  explicit NameReader(const QString &models);
  ~NameReader();
  Match read(const cv::Mat &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace owcs
