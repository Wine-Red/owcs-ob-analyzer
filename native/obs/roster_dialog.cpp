#include "roster_dialog.hpp"
#include "../core/names.hpp"
#include "../core/roster.hpp"
#include "../runtime/registry.hpp"
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QUrl>
#include <QVBoxLayout>
namespace owcs {
static QJsonObject readRoster(const QString &file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}
void loadRoster(const QString &file) {
  QStringList names;
  for (auto name : readRoster(file)["names"].toArray())
    names << name.toString();
  Registry::instance().setRoster(parseRoster(names.join('\n')));
}
class RosterDialog : public QDialog {
public:
  QLineEdit *search;
  QComboBox *events;
  QPlainTextEdit *editor;
  QLabel *status, *count;
  QPushButton *searchButton, *importButton;
  QJsonArray sources;
  RosterDialog(QString file, LiquipediaClient *client, QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("选手名单"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 650);
    auto layout = new QVBoxLayout(this);
    auto intro = new QLabel(QStringLiteral(
        "所有选手使用同一个名单，不区分队伍。可从赛事导入，也可在下方手动添加或修改。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto searchRow = new QHBoxLayout;
    search = new QLineEdit(QString("OWCS %1").arg(QDate::currentDate().year()));
    search->setAccessibleName(QStringLiteral("赛事搜索"));
    search->setPlaceholderText(QStringLiteral("赛事名称、年份，或 Liquipedia 赛事链接"));
    searchButton = new QPushButton(QStringLiteral("搜索赛事"));
    searchRow->addWidget(search);
    searchRow->addWidget(searchButton);
    layout->addLayout(searchRow);
    auto eventRow = new QHBoxLayout;
    events = new QComboBox;
    events->setMinimumContentsLength(25);
    events->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    events->setAccessibleName(QStringLiteral("赛事列表"));
    importButton = new QPushButton(QStringLiteral("导入选手"));
    importButton->setEnabled(false);
    eventRow->addWidget(events, 1);
    eventRow->addWidget(importButton);
    layout->addLayout(eventRow);
    status = new QLabel(QStringLiteral("搜索后选择具体赛事；导入会追加到下方名单，保存后生效。"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    editor = new QPlainTextEdit(Registry::instance().roster().join('\n'));
    editor->setAccessibleName(QStringLiteral("选手名单编辑"));
    editor->setPlaceholderText(QStringLiteral("每行一个选手昵称，例如：\nLeave\nShy\nLIP"));
    layout->addWidget(editor, 1);
    count = new QLabel;
    layout->addWidget(count);
    auto hint = new QLabel(QStringLiteral("多个候选过于接近或未匹配时使用 OCR 名字。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto credit = new QLabel(QStringLiteral("赛事名单来源 Liquipedia"));
    credit->setAlignment(Qt::AlignCenter);
    layout->addWidget(credit);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存并应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    for (auto button : findChildren<QPushButton *>()) {
      button->setAutoDefault(false);
      button->setDefault(false);
    }
    sources = readRoster(file)["sources"].toArray();
    auto recount = [this] {
      count->setText(QStringLiteral("%1 名选手")
                         .arg(parseRoster(editor->toPlainText()).size()));
    };
    connect(editor, &QPlainTextEdit::textChanged, this, recount);
    recount();
    auto enabled = [this](bool value) {
      searchButton->setEnabled(value);
      importButton->setEnabled(value && events->count());
    };
    QPointer<RosterDialog> self(this);
    connect(searchButton, &QPushButton::clicked, this, [this, self, client, enabled] {
      const auto text = search->text().trimmed();
      if (text.startsWith("http")) {
        auto title = liquipediaPage(text);
        if (title.isEmpty()) {
          status->setText(QStringLiteral("请输入 Liquipedia Overwatch 的赛事链接"));
          return;
        }
        events->clear();
        events->addItem(title, title);
        enabled(true);
        status->setText(QStringLiteral("已选择赛事，点击“导入选手”读取参赛名单。"));
        return;
      }
      enabled(false);
      status->setText(QStringLiteral("正在搜索赛事…"));
      bool started = client->search(text, [self, enabled](QJsonObject data, QString error) {
        if (!self)
          return;
        if (error.isEmpty()) {
          self->events->clear();
          for (auto value : data["query"].toObject()["search"].toArray()) {
            auto title = value.toObject()["title"].toString();
            if (!liquipediaPage(title).isEmpty())
              self->events->addItem(title, title);
          }
          self->status->setText(self->events->count()
                                    ? QStringLiteral("选择具体赛事后导入选手；也可粘贴赛事链接。")
                                    : QStringLiteral("未找到赛事，请换关键词或粘贴赛事链接。"));
        } else
          self->status->setText(error);
        enabled(true);
      });
      if (!started) {
        enabled(true);
        status->setText(QStringLiteral("上一个请求仍在处理，请稍后再试。"));
      }
    });
    connect(search, &QLineEdit::returnPressed, searchButton, &QPushButton::click);
    connect(importButton, &QPushButton::clicked, this, [this, self, client, enabled] {
      auto title = events->currentData().toString();
      if (title.isEmpty())
        return;
      enabled(false);
      status->setText(
          QStringLiteral("正在获取赛事选手…连续导入遵循请求间隔，可能需等待约 30 秒。"));
      bool started = client->tournament(title, [self, enabled, title](QJsonObject data,
                                                                      QString error) {
        if (!self)
          return;
        if (error.isEmpty()) {
          const auto page = data["parse"].toObject();
          const auto roster = parseTournamentRoster(page["text"].toObject()["*"].toString());
          if (roster.names.isEmpty())
            self->status->setText(roster.warnings.join(QStringLiteral("；")));
          else {
            auto before = parseRoster(self->editor->toPlainText()).size();
            auto merged = parseRoster(self->editor->toPlainText() + '\n' + roster.names.join('\n'));
            if (merged.size() > 1000) {
              self->status->setText(QStringLiteral("名单超过 1000 人，请先移除不需要的赛事选手。"));
              enabled(true);
              return;
            }
            self->editor->setPlainText(merged.join('\n'));
            QUrl url("https://liquipedia.net");
            url.setPath("/overwatch/" + title);
            self->sources.append(
                QJsonObject{{"title", page["title"]},
                            {"url", url.toString()},
                            {"revision", page["revid"]},
                            {"imported_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                            {"names", QJsonArray::fromStringList(roster.names)}});
            self->status->setText(QStringLiteral("%1：读取 %2 人，新增 %3 人。保存后生效。%4")
                                      .arg(title)
                                      .arg(roster.names.size())
                                      .arg(merged.size() - before)
                                      .arg(roster.warnings.join(QStringLiteral("；"))));
          }
        } else
          self->status->setText(error);
        enabled(true);
      });
      if (!started) {
        enabled(true);
        status->setText(QStringLiteral("上一个请求仍在处理，请稍后再试。"));
      }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, file] {
      const auto names = parseRoster(editor->toPlainText());
      if (names.size() > 1000) {
        status->setText(QStringLiteral("最多保存 1000 名选手"));
        return;
      }
      for (auto line : editor->toPlainText().split('\n'))
        if (!line.trimmed().isEmpty() && parseRoster(line).isEmpty()) {
          status->setText(QStringLiteral("有昵称为空或超过 32 个字符，请修正后保存。"));
          return;
        }
      QDir().mkpath(QFileInfo(file).absolutePath());
      QSaveFile output(file);
      if (!output.open(QIODevice::WriteOnly)) {
        status->setText(QStringLiteral("无法保存名单，请检查配置目录权限。"));
        return;
      }
      auto bytes = QJsonDocument(QJsonObject{{"names", QJsonArray::fromStringList(names)},
                                             {"sources", sources}})
                       .toJson();
      if (output.write(bytes) != bytes.size() || !output.commit()) {
        status->setText(QStringLiteral("保存失败，原名单未改变。"));
        return;
      }
      Registry::instance().setRoster(names);
      accept();
    });
  }
};
QDialog *createRosterDialog(const QString &file, LiquipediaClient *client, QWidget *parent) {
  return new RosterDialog(file, client, parent);
}
} // namespace owcs
