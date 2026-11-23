#include "theme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#include <QRegularExpression>
#include <cmath>

static QColor blendColor(const QColor &a, const QColor &b, qreal weightA) {
  const qreal weightB = 1.0 - weightA;
  return QColor::fromRgb(
      int(std::lround(a.red() * weightA + b.red() * weightB)),
      int(std::lround(a.green() * weightA + b.green() * weightB)),
      int(std::lround(a.blue() * weightA + b.blue() * weightB)));
}

static qreal relativeLuminance(const QColor &color) {
  auto channel = [](qreal c) {
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
         0.0722 * channel(color.blueF());
}

static qreal contrastRatio(const QColor &a, const QColor &b) {
  const qreal l1 = relativeLuminance(a), l2 = relativeLuminance(b);
  return (qMax(l1, l2) + 0.05) / (qMin(l1, l2) + 0.05);
}

ThemeManager::ThemeManager(QApplication *app)
    : QObject(app), m_app(app),
      m_path(QDir::homePath() +
             "/.local/state/omarchy/current/theme/colors.toml") {
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
          &ThemeManager::apply);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &ThemeManager::apply);
  armWatcher();
  apply();
}

void ThemeManager::armWatcher() {
  const QFileInfo colorsInfo(m_path);
  const QString themeDir = colorsInfo.absolutePath();
  const QString currentDir = QFileInfo(themeDir).absolutePath();
  for (const auto &dir : {currentDir, themeDir}) {
    if (QFileInfo::exists(dir) && !m_watcher.directories().contains(dir))
      m_watcher.addPath(dir);
  }
  if (QFile::exists(m_path) && !m_watcher.files().contains(m_path))
    m_watcher.addPath(m_path);
}

void ThemeManager::apply() {
  QMap<QString, QString> c{{"background", "#121212"},
                           {"lighter_background", "#1e1e1e"},
                           {"dark_background", "#121212"},
                           {"foreground", "#bebebe"},
                           {"light_foreground", "#8a8a8d"},
                           {"dark_foreground", "#555555"},
                           {"accent", "#e68e0d"},
                           {"selection", "#333333"},
                           {"selection_foreground", ""},
                           {"muted", "#333333"}};
  QFile f(m_path);
  if (f.open(QIODevice::ReadOnly)) {
    QRegularExpression re("^([a-z_]+)\\s*=\\s*\"(#[0-9A-Fa-f]{6,8})\"");
    for (const auto &line : QString::fromUtf8(f.readAll()).split('\n')) {
      auto m = re.match(line.trimmed());
      if (m.hasMatch())
        c[m.captured(1)] = m.captured(2);
    }
  }
  QPalette p;
  const QColor bg(c["background"]), panel(c["lighter_background"]),
      fg(c["foreground"]), accent(c["accent"]), rawSelection(c["selection"]),
      muted(c["muted"]), darkFg(c["dark_foreground"]);
  const QColor selection =
      (rawSelection == panel || rawSelection == bg)
          ? blendColor(accent, panel, 0.35)
          : rawSelection;
  QColor selectionFg = fg;
  if (!c["selection_foreground"].isEmpty()) {
    const QColor candidate(c["selection_foreground"]);
    if (contrastRatio(candidate, selection) >= 3.0)
      selectionFg = candidate;
  }
  const QColor placeholder =
      contrastRatio(darkFg, fg) >= 1.25 ? darkFg : muted;

  p.setColor(QPalette::Window, bg);
  p.setColor(QPalette::WindowText, fg);
  p.setColor(QPalette::Base, panel);
  p.setColor(QPalette::AlternateBase, bg);
  p.setColor(QPalette::Text, fg);
  p.setColor(QPalette::Button, panel);
  p.setColor(QPalette::ButtonText, fg);
  p.setColor(QPalette::Highlight, selection);
  p.setColor(QPalette::HighlightedText, selectionFg);
  p.setColor(QPalette::PlaceholderText, placeholder);
  p.setColor(QPalette::ToolTipBase, panel);
  p.setColor(QPalette::ToolTipText, fg);
  p.setColor(QPalette::Link, accent);
  p.setColor(QPalette::LinkVisited, accent);
  p.setColor(QPalette::Disabled, QPalette::WindowText, darkFg);
  p.setColor(QPalette::Disabled, QPalette::Text, darkFg);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, darkFg);
  m_app->setPalette(p);
  m_app->setStyleSheet(
      QString("QWidget { font-size: 13px; } "
              "QDialogButtonBox { dialogbuttonbox-buttons-have-icons: 0; } "
              "QPushButton, QLineEdit, QComboBox { padding: 7px 10px; border: 1px solid %1; border-radius: 6px; } "
              "QPushButton:hover { border-color: %2; } QPushButton:disabled { color: %3; } "
              "QTableWidget { border: 1px solid %1; border-radius: 6px; gridline-color: %1; } "
              "QHeaderView::section { background: %4; padding: 8px; border: none; border-bottom: 1px solid %1; } "
              "QTableWidget::item { padding: 5px; } QTableWidget::item:selected { background: %5; color: %6; }")
          .arg(muted.name(), accent.name(), darkFg.name(), panel.name(),
               selection.name(), selectionFg.name()));
  armWatcher();
}
