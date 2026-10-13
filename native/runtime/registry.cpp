#include "registry.hpp"
#include <QMutexLocker>
namespace owcs {
Registry &Registry::instance() {
  static Registry r;
  return r;
}
void Registry::publish(const QString &id, QJsonObject s) {
  QMutexLocker lock(&mutex_);
  auto &entry = entries_[id];
  entry.snapshot = std::move(s);
  entry.seen = monotonicMs();
  if (selected_.isEmpty())
    selected_ = id;
}
void Registry::received(const QString &id, const QString &source, qint64 captured) {
  QMutexLocker lock(&mutex_);
  auto &entry = entries_[id];
  entry.captured = captured;
  entry.source = source;
  if (selected_.isEmpty())
    selected_ = id;
}
void Registry::remove(const QString &id) {
  QMutexLocker lock(&mutex_);
  entries_.remove(id);
  if (selected_ == id)
    selected_ = entries_.isEmpty() ? QString() : entries_.firstKey();
  if (inputOwner_ == id)
    inputOwner_.clear();
}
void Registry::select(const QString &id) {
  QMutexLocker lock(&mutex_);
  if (entries_.contains(id))
    selected_ = id;
}
QJsonObject Registry::state(const QString &id) const {
  QMutexLocker lock(&mutex_);
  auto it = entries_.find(id.isEmpty() ? selected_ : id);
  if (it == entries_.end())
    return {{"type", "state.unavailable"}, {"schema_version", "2.0"}, {"source_status", "waiting"}};
  const auto now = monotonicMs();
  const auto inputAge = now - (it->captured >= 0 ? it->captured : it->seen);
  if (it->snapshot.isEmpty())
    return {{"type", "state.unavailable"},
            {"schema_version", "2.0"},
            {"stream_id", it.key()},
            {"source_status", inputAge > 2500 ? "stale" : "waiting"},
            {"source_age_ms", double(inputAge)},
            {"analysis_status", "initializing"},
            {"source", QJsonObject{{"name", it->source}}}};
  auto result = State::age(it->snapshot, now - it->seen);
  result["source_age_ms"] = double(inputAge);
  if (it->snapshot["source_status"] == "online")
    result["source_status"] = inputAge > 2500 ? "stale" : "online";
  auto source = result["source"].toObject();
  double processing = source["cycle_latency_ms"].toDouble();
  auto analyzed = source["captured_at"].toDouble(-1);
  double resultAge =
      analyzed >= 0 ? std::max(0., double(now) - analyzed) : double(now - it->seen) + processing;
  result["analysis_age_ms"] = resultAge;
  result["analysis_status"] = resultAge > 1500 ? "delayed" : "current";
  return result;
}
QJsonArray Registry::streams() const {
  QMutexLocker lock(&mutex_);
  QJsonArray list;
  for (auto it = entries_.begin(); it != entries_.end(); ++it)
    list.append(QJsonObject{
        {"stream_id", it.key()},
        {"name",
         it->source.isEmpty() ? it->snapshot["source"].toObject()["name"] : QJsonValue(it->source)},
        {"selected", it.key() == selected_},
        {"source_age_ms", double(monotonicMs() - (it->captured >= 0 ? it->captured : it->seen))}});
  return list;
}
bool Registry::claimInput(const QString &id) {
  QMutexLocker lock(&mutex_);
  if (inputOwner_.isEmpty() || inputOwner_ == id) {
    inputOwner_ = id;
    return true;
  }
  return false;
}
void Registry::releaseInput(const QString &id) {
  QMutexLocker lock(&mutex_);
  if (inputOwner_ == id)
    inputOwner_.clear();
}
} // namespace owcs
