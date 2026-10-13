#pragma once
#include "../core/names.hpp"
#include "input.hpp"
#include "registry.hpp"
#include <condition_variable>
namespace owcs {
struct Settings {
  bool enabled = true, autoTab = false, names = true;
  int fps = 5;
};
class Engine {
public:
  Engine(QString id, QString assets);
  ~Engine();
  void configure(Settings);
  void submit(cv::Mat bgra, QString source, qint64 captured);
  void pause();

private:
  void run();
  QString id_, assets_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::thread thread_;
  cv::Mat pending_;
  QString source_;
  qint64 captured_ = 0;
  Settings settings_;
  bool stop_ = false;
  std::atomic_uint generation_{0};
  TabHold tab_;
};
} // namespace owcs
