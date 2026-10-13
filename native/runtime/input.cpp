#include "input.hpp"
#include "../core/state.hpp"
#include <QFileInfo>
namespace owcs {
static DWORD integrity(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    return 0;
  DWORD bytes = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
  std::vector<BYTE> buf(bytes);
  DWORD value = 0;
  if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), bytes, &bytes)) {
    auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data());
    auto sid = label->Label.Sid;
    value = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
  }
  CloseHandle(token);
  return value;
}
TabHold::TabHold() {
  thread_ = std::thread([this] { watch(); });
}
TabHold::~TabHold() {
  stop_ = true;
  if (thread_.joinable())
    thread_.join();
  release();
}
bool TabHold::foregroundGame() const {
  DWORD pid = 0;
  auto hwnd = GetForegroundWindow();
  GetWindowThreadProcessId(hwnd, &pid);
  return hwnd == target_ && pid == pid_ && IsWindowVisible(hwnd) && !IsIconic(hwnd);
}
void TabHold::send(bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = VK_TAB;
  input.ki.wScan = 0x0f;
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (SendInput(1, &input, sizeof(input)) != 1)
    error_ = QStringLiteral("无法发送 Tab，请检查 OBS 和游戏的权限级别");
  else
    owned_ = down;
}
void TabHold::update(bool enabled, bool allowed) {
  std::lock_guard lock(mutex_);
  if (!enabled || !allowed) {
    if (owned_)
      send(false);
    if (!enabled && !owned_)
      error_.clear();
    return;
  }
  if (owned_ && !foregroundGame()) {
    send(false);
    return;
  }
  if (!owned_) {
    HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
      return;
    wchar_t path[32768];
    DWORD len = 32768;
    bool game = QueryFullProcessImageNameW(process, 0, path, &len) &&
                QFileInfo(QString::fromWCharArray(path, len))
                        .fileName()
                        .compare("Overwatch.exe", Qt::CaseInsensitive) == 0;
    DWORD gameLevel = integrity(process), ownLevel = integrity(GetCurrentProcess());
    CloseHandle(process);
    if (!game)
      return;
    if (gameLevel && ownLevel && gameLevel > ownLevel) {
      error_ = QStringLiteral("游戏以管理员权限运行，请以管理员身份启动 OBS，或关闭自动 Tab");
      return;
    }
    if (GetAsyncKeyState(VK_TAB) & 0x8000)
      return;
    target_ = hwnd;
    pid_ = pid;
    error_.clear();
    send(true);
  }
  lease_ = monotonicMs() + 1500;
}
void TabHold::watch() {
  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard lock(mutex_);
    if (owned_ && (!foregroundGame() || monotonicMs() > lease_))
      send(false);
  }
}
void TabHold::release() {
  std::lock_guard lock(mutex_);
  if (owned_)
    send(false);
}
bool TabHold::held() const {
  std::lock_guard lock(mutex_);
  return owned_;
}
QString TabHold::error() const {
  std::lock_guard lock(mutex_);
  return error_;
}
} // namespace owcs
