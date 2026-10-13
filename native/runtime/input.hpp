#pragma once
#include <QString>
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>
namespace owcs {
class TabHold {
public:
  TabHold();
  ~TabHold();
  void update(bool enabled, bool allowed);
  void release();
  bool held() const;
  QString error() const;

private:
  void watch();
  bool foregroundGame() const;
  void send(bool down);
  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic_bool stop_{false};
  HWND target_ = nullptr;
  DWORD pid_ = 0;
  bool owned_ = false;
  qint64 lease_ = 0;
  QString error_;
};
} // namespace owcs
