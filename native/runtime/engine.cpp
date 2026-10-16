#include "engine.hpp"
#include "../core/public_state.hpp"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
namespace owcs {
Engine::Engine(QString id, QString assets) : id_(std::move(id)), assets_(std::move(assets)) {
  thread_ = std::thread([this] { run(); });
}
Engine::~Engine() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    pending_.release();
  }
  ready_.notify_all();
  tab_.release();
  if (thread_.joinable())
    thread_.join();
  Registry::instance().releaseInput(id_);
  Registry::instance().remove(id_);
}
void Engine::configure(Settings s) {
  bool pauseNow = !s.enabled || !s.autoTab;
  {
    std::lock_guard lock(mutex_);
    settings_ = s;
  }
  if (pauseNow) {
    tab_.release();
    Registry::instance().releaseInput(id_);
  }
}
void Engine::submit(cv::Mat frame, QString name, qint64 time) {
  std::lock_guard lock(mutex_);
  if (stop_ || !settings_.enabled)
    return;
  pending_ = std::move(frame);
  source_ = std::move(name);
  captured_ = time;
  Registry::instance().received(id_, source_, time);
  ready_.notify_one();
}
void Engine::pause() {
  generation_++;
  {
    std::lock_guard lock(mutex_);
    pending_.release();
  }
  tab_.release();
  Registry::instance().releaseInput(id_);
  auto snapshot = Registry::instance().state(id_);
  if (snapshot["type"] == "state.snapshot") {
    snapshot["source_status"] = "stopped";
    auto b = snapshot["scoreboard"].toObject();
    b["status"] = "unavailable";
    b["confirmed"] = false;
    b["tab_held"] = false;
    snapshot["scoreboard"] = b;
    Registry::instance().publish(id_, State::age(snapshot, 0));
  }
}
void Engine::run() {
  try {
    cv::setNumThreads(2);
    Recognizer reader(assets_);
    std::unique_ptr<NameReader> names;
    State state(id_);
    int nameRow = 0;
    unsigned previousGeneration = generation_;
    qint64 authorized = -10000, opening = -1, lastRetry = -1;
    QString error;
    // Explicit developer opt-in only. Bound both storage and capture frequency.
    const auto diagnosticRoot = qEnvironmentVariable("OWCS_OB_DIAGNOSTICS");
    const auto diagnosticDir = diagnosticRoot.isEmpty() ? QString() : diagnosticRoot + "/" + id_;
    int diagnosticCount = 0;
    qint64 diagnosticLast = -10000;
    while (true) {
      cv::Mat frame;
      QString source;
      qint64 captured;
      Settings config;
      unsigned gen;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stop_ || !pending_.empty(); });
        if (stop_)
          break;
        frame = std::move(pending_);
        source = source_;
        captured = captured_;
        config = settings_;
        gen = generation_;
      }
      if (!config.enabled)
        continue;
      if (gen != previousGeneration) {
        state.reset();
        previousGeneration = gen;
        authorized = -10000;
      }
      auto start = monotonicMs();
      if (start - captured > 1000)
        continue;
      cv::Mat bgr;
      cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
      auto observed = reader.detect(bgr);
      auto snapshot = state.update(observed, source, start, monotonicMs() - start);
      snapshot = localizedState(snapshot, reader.localizedNames());
      snapshot["recognition"] =
          QJsonObject{{"asset_format", 3}, {"resource_version", reader.resourceVersion()}};
      if (observed.visible && config.names) {
        int checked = 0, read = 0;
        while (checked++ < 10 && read < 2) {
          int row = nameRow;
          nameRow = (nameRow + 1) % 10;
          if (!state.needsName(row))
            continue;
          if (!names)
            names = std::make_unique<NameReader>(assets_);
          state.name(row, names->read(observed.names[row]), start);
          ++read;
          if (monotonicMs() - start >= 140)
            break;
        }
      }
      if (!diagnosticDir.isEmpty() && diagnosticCount < 60 && start - diagnosticLast >= 800) {
        diagnosticLast = start;
        if (QDir().mkpath(diagnosticDir)) {
          auto base =
              diagnosticDir + QString("/frame-%1").arg(diagnosticCount++, 3, 10, QChar('0'));
          std::vector<uchar> png;
          cv::imencode(".png", bgr, png, {cv::IMWRITE_PNG_COMPRESSION, 1});
          QFile frameFile(base + ".png"), stateFile(base + ".json");
          if (frameFile.open(QIODevice::WriteOnly))
            frameFile.write(reinterpret_cast<const char *>(png.data()), png.size());
          if (stateFile.open(QIODevice::WriteOnly))
            stateFile.write(QJsonDocument(QJsonObject{{"captured_at", double(captured)},
                                                      {"raw", observed.players},
                                                      {"state", snapshot}})
                                .toJson());
        }
      }
      std::lock_guard finalLock(mutex_);
      if (stop_ || gen != generation_ || !settings_.enabled)
        continue;
      config = settings_;
      bool valid = observed.confirmed || observed.hudOffset != -10000;
      if (valid)
        authorized = start;
      bool allowed = valid || start - authorized < 800;
      bool input = config.autoTab && allowed && Registry::instance().claimInput(id_);
      if (!allowed)
        Registry::instance().releaseInput(id_);
      if (!config.autoTab) {
        Registry::instance().releaseInput(id_);
        opening = -1;
        error.clear();
      }
      if (input && allowed && !observed.visible && tab_.held()) {
        if (opening < 0) {
          opening = start;
          lastRetry = start;
        }
        if (start - opening > 15000) {
          allowed = false;
          error = QStringLiteral("自动 Tab 未在 15 "
                                 "秒内打开面板；请检查权限和按键，关闭后重新启用自动 Tab");
        } else if (start - lastRetry > 2000) {
          tab_.release();
          lastRetry = start;
        }
      } else if (observed.visible) {
        opening = -1;
        error.clear();
      }
      tab_.update(input && error.isEmpty(), allowed);
      auto board = snapshot["scoreboard"].toObject();
      board["tab_held"] = tab_.held();
      snapshot["scoreboard"] = board;
      auto inputError = tab_.error();
      snapshot["error"] = !inputError.isEmpty() ? QJsonValue(inputError) : nullable(error);
      snapshot["source"] = QJsonObject{{"name", source},
                                       {"cycle_latency_ms", double(monotonicMs() - start)},
                                       {"target_fps", config.fps},
                                       {"captured_at", double(captured)}};
      // The observation is already this old when the worker finishes. Never publish
      // a delayed result as a newly captured frame, even while input is healthy.
      snapshot = State::age(snapshot, monotonicMs() - start);
      snapshot["source_status"] = "online";
      Registry::instance().publish(id_, snapshot);
    }
  } catch (const std::exception &e) {
    tab_.release();
    Registry::instance().releaseInput(id_);
    Registry::instance().publish(id_, {{"type", "state.unavailable"},
                                       {"schema_version", "2.0"},
                                       {"stream_id", id_},
                                       {"source_status", "error"},
                                       {"error", QString::fromUtf8(e.what())}});
  }
}
} // namespace owcs
