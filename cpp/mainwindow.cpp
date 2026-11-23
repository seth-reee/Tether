#include "mainwindow.h"
#include "backends.h"
#include "mountidentity.h"
#include "plainmessagebox.h"
#include "settingsdialog.h"
#include <QCheckBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

MainWindow::MainWindow(AppConfig c, ConfigStore s)
    : m_config(std::move(c)), m_store(std::move(s)), m_helper(this) {
  setWindowTitle("Tether");
  resize(1000, 540);
  auto *w = new QWidget;
  auto *l = new QVBoxLayout(w);
  l->setContentsMargins(20, 20, 20, 12);
  l->setSpacing(12);
  auto *titleRow = new QHBoxLayout;
  auto *titleIcon = new QLabel;
  titleIcon->setPixmap(QPixmap(":/tether.png").scaled(
      42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  auto *title = new QLabel("Tether");
  QFont titleFont = title->font();
  titleFont.setBold(true);
  title->setFont(titleFont);
  titleRow->addWidget(titleIcon);
  titleRow->addWidget(title);
  titleRow->addStretch();
  auto *about = new QPushButton("About");
  about->setObjectName("headerAboutButton");
  titleRow->addWidget(about);
  connect(about, &QPushButton::clicked, this,
          [this] { showTetherAbout(this); });
  l->addLayout(titleRow);
  l->addWidget(new QLabel("Network share manager"));
  m_welcome = new QWidget;
  m_welcome->setObjectName("welcome");
  auto *welcomeLayout = new QVBoxLayout(m_welcome);
  welcomeLayout->addWidget(new QLabel("No servers configured. Add a server to get started."));
  auto *addServer = new QPushButton("Add server");
  addServer->setObjectName("addServer");
  welcomeLayout->addWidget(addServer);
  connect(addServer, &QPushButton::clicked, this, &MainWindow::settings);
  l->addWidget(m_welcome);
  m_table = new QTableWidget(0, 7);
  m_table->setHorizontalHeaderLabels({"", "Server", "Share", "Protocol",
                                      "Remote path", "Mount destination",
                                      "Status"});
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  l->addWidget(m_table);
  m_summary = new QLabel;
  l->addWidget(m_summary);
  auto *a = new QHBoxLayout;
  auto add = [&](QString n, auto f) {
    auto *b = new QPushButton(n);
    connect(b, &QPushButton::clicked, this, f);
    a->addWidget(b);
  };
  add("Mount All", [this] { control("mount", true); });
  add("Mount Selected", [this] { control("mount", false); });
  add("Unmount Selected", [this] { control("unmount", false); });
  add("Unmount All", [this] { control("unmount", true); });
  a->addStretch();
  add("Refresh", [this] { refresh(); });
  add("Settings", [this] { settings(); });
  l->addLayout(a);
  setCentralWidget(w);
  connect(&m_helper, &HelperClient::finished, this,
          [this](const QString &) { refresh(); });
  connect(&m_helper, &HelperClient::failed, this, [this](const QString &e) {
    refresh();
    PlainMessageBox::warning(this, "Operation failed", e);
  });
  refresh();
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &MainWindow::refresh);
  timer->start(3000);
}
void MainWindow::refresh() {
  m_welcome->setVisible(m_config.shares.isEmpty());
  m_welcome->findChild<QLabel *>()->setText(
      m_config.servers.isEmpty()
          ? "No servers configured. Add a server to get started."
          : "No shares configured. Open Settings to discover or add shares.");
  m_welcome->findChild<QPushButton *>()->setText(
      m_config.servers.isEmpty() ? "Add server" : "Add shares");

  struct RowData {
    QString key;
    QString shareId;
    bool enabled;
    QStringList columns;
    QString status;
  };
  QFile mounts("/proc/self/mountinfo");
  const bool readable = mounts.open(QIODevice::ReadOnly);
  const auto mountinfo = readable ? mounts.readAll() : QByteArray();

  QVector<RowData> rows;
  QStringList rowKeys;
  int mounted = 0;
  for (const auto &h : m_config.shares) {
    auto it = std::find_if(m_config.servers.begin(), m_config.servers.end(),
                           [&](const Server &s) { return s.id == h.serverId; });
    if (it == m_config.servers.end())
      continue;
    QStringList sources;
    for (const auto &host : {it->hostname, it->fallbackIp}) {
      if (!host.isEmpty())
        sources << (it->protocol == Protocol::Nfs
                        ? formatNfsSource(host, h.remotePath)
                        : "//" + host + "/" + h.remotePath);
    }
    QString status =
        readable ? mountStatus(mountinfo, h.localPath, sources,
                               it->protocol == Protocol::Nfs ? "nfs" : "cifs")
                 : "Unknown";
    if (status == "Mounted")
      mounted++;
    if (!h.enabled)
      status = (status == "Unmounted") ? "Disabled" : status + " (disabled)";
    const QString proto = protocolName(it->protocol).toUpper();
    const QString key = QStringList{h.id, it->name, h.name, proto, h.remotePath,
                                    h.localPath, h.enabled ? "1" : "0"}
                            .join('\x1f');
    rowKeys << key;
    rows.append({key,
                 h.id,
                 h.enabled,
                 {it->name, h.name, proto, h.remotePath, h.localPath},
                 status});
  }

  if (rowKeys == m_rowKeys && m_table->rowCount() == rows.size()) {
    for (int r = 0; r < rows.size(); ++r) {
      if (auto *item = m_table->item(r, 6))
        item->setText(rows[r].status);
      else
        m_table->setItem(r, 6, new QTableWidgetItem(rows[r].status));
    }
  } else {
    QHash<QString, bool> selected;
    for (int r = 0; r < m_table->rowCount(); ++r) {
      auto *check = qobject_cast<QCheckBox *>(m_table->cellWidget(r, 0));
      if (check && check->property("shareEnabled").toBool())
        selected.insert(check->property("shareId").toString(),
                        check->isChecked());
    }
    m_table->setRowCount(0);
    for (const auto &row : rows) {
      const int r = m_table->rowCount();
      m_table->insertRow(r);
      auto *c = new QCheckBox;
      c->setProperty("shareId", row.shareId);
      c->setProperty("shareEnabled", row.enabled);
      c->setChecked(row.enabled ? selected.value(row.shareId, true) : false);
      c->setEnabled(row.enabled);
      m_table->setCellWidget(r, 0, c);
      for (int i = 0; i < row.columns.size(); ++i)
        m_table->setItem(r, i + 1, new QTableWidgetItem(row.columns[i]));
      m_table->setItem(r, 6, new QTableWidgetItem(row.status));
    }
    m_rowKeys = rowKeys;
  }

  m_summary->setText(QString("%1 configured shares · %2 mounted")
                         .arg(m_config.shares.size())
                         .arg(mounted));
}
void MainWindow::settings() {
  SettingsDialog d(m_config, this, m_store);
  if (m_config.servers.isEmpty())
    d.beginAddServer();
  if (d.exec() == QDialog::Accepted) {
    m_config = d.config();
    if (!d.savedToDisk()) {
      try {
        m_store.save(m_config);
      } catch (const std::exception &e) {
        PlainMessageBox::critical(this, "Save failed", e.what());
      }
    }
    refresh();
  } else if (d.savedToDisk()) {
    try {
      m_config = m_store.load();
    } catch (const std::exception &e) {
      PlainMessageBox::warning(this, "Reload failed", e.what());
    }
    refresh();
  }
}
QStringList MainWindow::collectControlIds(bool all) const {
  QStringList ids;
  for (int r = 0; r < m_table->rowCount(); ++r) {
    auto *c = qobject_cast<QCheckBox *>(m_table->cellWidget(r, 0));
    if (c && c->property("shareEnabled").toBool() && (all || c->isChecked()))
      ids << c->property("shareId").toString();
  }
  return ids;
}
void MainWindow::control(const QString &a, bool all) {
  const auto ids = collectControlIds(all);
  if (ids.isEmpty()) {
    PlainMessageBox::information(this, "No shares",
                                 all ? "No enabled shares configured."
                                     : "Select at least one enabled share.");
    return;
  }
  m_helper.control(a, ids);
}
