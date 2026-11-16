#pragma once
#include <QtGlobal>
#include <algorithm>

namespace owcs {
// Frame-driven schedule: key-up and key-down must occur in different cycles.
// No sleeping on the recognition thread and no autonomous background key-down.
class TabRefresh {
public:
  void configure(bool enabled, int seconds) {
    seconds = std::clamp(seconds, 2, 300);
    if (enabled_ != enabled || seconds_ != seconds) {
      enabled_ = enabled;
      seconds_ = seconds;
      heldSince_ = -1;
      releasedAt_ = -1;
    }
  }
  void reset() { heldSince_ = releasedAt_ = -1; }
  void pressed(qint64 now) {
    heldSince_ = now;
    releasedAt_ = -1;
  }
  void released(qint64 now) {
    heldSince_ = -1;
    releasedAt_ = now;
  }
  bool waiting(qint64 now) const { return releasedAt_ >= 0 && now - releasedAt_ < 250; }
  bool due(qint64 now) {
    if (!enabled_)
      return false;
    if (heldSince_ < 0)
      heldSince_ = now;
    return now - heldSince_ >= qint64(seconds_) * 1000;
  }

private:
  bool enabled_ = false;
  int seconds_ = 10;
  qint64 heldSince_ = -1, releasedAt_ = -1;
};
} // namespace owcs
