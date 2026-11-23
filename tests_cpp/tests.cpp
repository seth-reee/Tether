#include "backends.h"
#include "config.h"
#include "fixtures.h"
#include "mainwindow.h"
#include "filetransaction.h"
#include "mountidentity.h"
#include "settingsdialog.h"
#include "systemdclient.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QListWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QtTest>

class Tests : public QObject {
  Q_OBJECT
private slots:
  void protocolFields() {
    auto config = sampleConfig();
    auto smb = config.servers[0];
    smb.id = "smb-server";
    smb.protocol = Protocol::Smb;
    smb.smbUsername = "tester";
    config.servers.append(smb);
    SettingsDialog dialog(config);
    auto *protocol = dialog.findChild<QComboBox *>("serverProtocol");
    auto *nfs = dialog.findChild<QComboBox *>("nfsVersion");
    auto *user = dialog.findChild<QLineEdit *>("smbUsername");
    QVERIFY(nfs);
    auto *form = qobject_cast<QFormLayout *>(nfs->parentWidget()->layout());
    QVERIFY(form);
    QVERIFY(!nfs->isHidden());
    QVERIFY(!form->labelForField(nfs)->isHidden());
    for (auto name : {"smbUsername", "smbDomain", "smbVersion", "smbPassword"}) {
      auto *field = dialog.findChild<QWidget *>(name);
      QVERIFY(field);
      QVERIFY(field->isHidden());
      QVERIFY(form->labelForField(field)->isHidden());
    }
    protocol->setCurrentText("SMB");
    QVERIFY(nfs->isHidden());
    QVERIFY(form->labelForField(nfs)->isHidden());
    QVERIFY(!user->isHidden());
    user->setText("kept");
    protocol->setCurrentText("NFS");
    protocol->setCurrentText("SMB");
    QCOMPARE(user->text(), QString("kept"));
    dialog.findChild<QListWidget *>()->setCurrentRow(1);
    QVERIFY(nfs->isHidden());
    QCOMPARE(user->text(), QString("tester"));
    dialog.findChild<QListWidget *>()->setCurrentRow(0);
    QCOMPARE(user->text(), QString("kept"));
  }
  void removeAndSwitchEmptyServer() {
    SettingsDialog dialog(sampleConfig());
    auto *list = dialog.findChild<QListWidget *>();
    QCOMPARE(list->count(), 1);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "addServer", Qt::DirectConnection));
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->currentRow(), 1);
    // Switching away from an incomplete server should not trap the user.
    list->setCurrentRow(0);
    QCOMPARE(list->currentRow(), 0);
    // Switching back and deleting the empty server must succeed without errors.
    list->setCurrentRow(1);
    QVERIFY(
        QMetaObject::invokeMethod(&dialog, "removeServer", Qt::DirectConnection));
    QCOMPARE(list->count(), 1);
    QCOMPARE(dialog.config().servers.size(), 1);
  }
  void disabledSharesExcludedFromControlAll() {
    auto config = sampleConfig();
    Share disabled = config.shares[0];
    disabled.id = "disabled-share";
    disabled.name = "Archive";
    disabled.localPath = "/mnt/test/Archive";
    disabled.enabled = false;
    config.shares.append(disabled);
    MainWindow window(config, ConfigStore("/unused/test-config"));
    auto *table = window.findChild<QTableWidget *>();
    QCOMPARE(table->rowCount(), 2);
    auto *disabledCheck = qobject_cast<QCheckBox *>(table->cellWidget(1, 0));
    QVERIFY(disabledCheck);
    QVERIFY(!disabledCheck->isEnabled());
    QVERIFY(!disabledCheck->isChecked());
    QCOMPARE(table->item(1, 6)->text(), QString("Disabled"));

    QStringList allIds;
    QVERIFY(QMetaObject::invokeMethod(&window, "collectControlIds",
                                      Qt::DirectConnection,
                                      Q_RETURN_ARG(QStringList, allIds),
                                      Q_ARG(bool, true)));
    QCOMPARE(allIds, QStringList{"test-share"});

    // Polling refresh should preserve existing row widgets in place.
    auto *firstCheckBefore = table->cellWidget(0, 0);
    QVERIFY(QMetaObject::invokeMethod(&window, "refresh", Qt::DirectConnection));
    QCOMPARE(table->cellWidget(0, 0), firstCheckBefore);
  }
  void plainSettingsButtons() {
    SettingsDialog dialog(defaultConfig());
    auto *save = dialog.findChild<QPushButton *>("saveApplyButton");
    auto *cancel = dialog.findChild<QPushButton *>("cancelButton");
    QVERIFY(save);
    QVERIFY(cancel);
    QCOMPARE(save->text(), QString("Save && Apply"));
    QCOMPARE(cancel->text(), QString("Cancel"));
    QVERIFY(save->icon().isNull());
    QVERIFY(cancel->icon().isNull());
    QVERIFY(save->shortcut().isEmpty());
  }
  void removeAllShares() {
    auto config = sampleConfig();
    auto second = config.shares[0];
    second.id = "second";
    config.shares.append(second);
    SettingsDialog dialog(config);
    auto *table = dialog.findChild<QTableWidget *>();
    table->item(0, 3)->setText("invalid path");
    table->selectAll();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "removeShare", Qt::DirectConnection));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(dialog.config().shares.isEmpty());
    bool collected = false;
    QVERIFY(QMetaObject::invokeMethod(&dialog, "collectSettings",
        Qt::DirectConnection, Q_RETURN_ARG(bool, collected)));
    QVERIFY(collected);
    QCOMPARE(dialog.config().servers.size(), 1);
    MainWindow window(dialog.config(), ConfigStore("/unused/test-config"));
    QVERIFY(!window.findChild<QWidget *>("welcome")->isHidden());
    QCOMPARE(window.findChild<QPushButton *>("addServer")->text(), QString("Add shares"));
  }
  void aboutPopup() {
    SettingsDialog dialog(defaultConfig());
    QCOMPARE(dialog.windowTitle(), QString("Tether Settings"));
    auto *button = dialog.findChild<QPushButton *>("aboutButton");
    QVERIFY(button);
    button->click();
    auto *popup = dialog.findChild<QDialog *>("aboutDialog");
    QVERIFY(popup);
    const auto labels = popup->findChildren<QLabel *>();
    QString aboutText;
    for (const auto *label : labels) aboutText += label->text();
    QVERIFY(aboutText.contains("seth-reee"));
    QVERIFY(aboutText.contains("https://github.com/seth-reee"));
    QVERIFY(aboutText.contains("NFS and SMB"));
    QVERIFY(aboutText.contains("Tether"));
    QVERIFY(aboutText.contains(TETHER_VERSION));
    auto *license = popup->findChild<QTextBrowser *>();
    QVERIFY(license);
    QVERIFY(license->toPlainText().contains("MIT License"));
    popup->accept();
    QVERIFY(dialog.config().servers.isEmpty());
  }
  void failedWriteRecovery() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto original = dir.filePath("unit.mount"),
               created = dir.filePath("new.mount");
    QFile file(original);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("original");
    file.close();
    FileTransaction transaction;
    transaction.capture(original);
    transaction.capture(created);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("changed");
    file.close();
    QFile added(created);
    QVERIFY(added.open(QIODevice::WriteOnly));
    added.write("new");
    added.close();
    transaction.rollback();
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("original"));
    QVERIFY(!QFile::exists(created));
  }
  void statusDistinguishesAutomounts() {
    const QByteArray waiting =
        "31 24 0:30 / /mnt/media rw - autofs systemd-1 rw\n";
    QCOMPARE(mountStatus(waiting, "/mnt/media", {"nas:/media"}, "nfs"),
             QString("Waiting for access"));
    QCOMPARE(mountStatus(
                 waiting + "32 31 0:31 / /mnt/media rw - nfs4 nas:/media rw\n",
                 "/mnt/media", {"nas:/media"}, "nfs"),
             QString("Mounted"));
    QCOMPARE(mountStatus("32 24 0:31 / /mnt/media rw - ext4 /dev/sdb rw\n",
                         "/mnt/media", {"nas:/media"}, "nfs"),
             QString("Conflicting filesystem"));
    QCOMPARE(mountStatus({}, "/mnt/media", {"nas:/media"}, "nfs"),
             QString("Unmounted"));
  }
  void settingsControlsPersist() {
    SettingsDialog dialog(sampleConfig());
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *mode = qobject_cast<QComboBox *>(table->cellWidget(0, 4));
    auto *enabled = qobject_cast<QCheckBox *>(table->cellWidget(0, 5));
    QVERIFY(mode);
    QVERIFY(enabled);
    QCOMPARE(mode->currentText(), QString("Manual"));
    mode->setCurrentIndex(mode->findData("access"));
    enabled->setChecked(false);
    table->item(0, 3)->setText("/mnt/test-share");
    bool collected = false;
    QVERIFY(QMetaObject::invokeMethod(&dialog, "collectSettings",
                                      Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, collected)));
    QVERIFY(collected);
    QCOMPARE(dialog.config().shares[0].automount, AutomountMode::Access);
    QVERIFY(!dialog.config().shares[0].enabled);
    QCOMPARE(dialog.config().shares[0].localPath, QString("/mnt/test-share"));
  }
  void mountIdentity() {
    const QByteArray mounted =
        "31 24 0:30 / /mnt/media rw - nfs4 nas:/media rw\n";
    QVERIFY(mountIdentityMatches(mounted, "/mnt/media", "nas:/media", "nfs",
                                 false));
    QVERIFY(!mountIdentityMatches(mounted, "/mnt/media", "other:/media", "nfs",
                                  false));
    QVERIFY(!mountIdentityMatches(mounted, "/mnt/media", "nas:/media", "cifs",
                                  false));
    QVERIFY(!mountIdentityMatches(
        mounted + "32 24 0:31 / /mnt/media rw - ext4 /dev/sdb rw\n",
        "/mnt/media", "nas:/media", "nfs", false));
    QVERIFY(!mountIdentityMatches("malformed\n", "/mnt/media", "nas:/media",
                                  "nfs", false));
    QVERIFY(!mountIdentityMatches(mounted, "", "", "", false));
  }
  void automountIdentity() {
    const QByteArray mounted =
        "31 24 0:30 / /mnt/media rw - autofs systemd-1 rw\n";
    QVERIFY(
        mountIdentityMatches(mounted, "/mnt/media", "nas:/media", "nfs", true));
    QVERIFY(!mountIdentityMatches(mounted, "/mnt/media", "nas:/media", "nfs",
                                  false));
    QVERIFY(mountIdentityMatches(
        "31 24 0:30 / /mnt/My\\040Share rw - cifs //nas/My\\040Share rw\n",
        "/mnt/My Share", "//nas/My Share", "cifs", false));
  }
  void defaults() {
    QTemporaryDir dir;
    ConfigStore store(dir.filePath("config.json"));
    auto c = store.load();
    QVERIFY(c.servers.isEmpty());
    QVERIFY(c.shares.isEmpty());
    QCOMPARE(c.mountRoot, QDir::homePath() + "/Mount");
    QVERIFY(!QFile::exists(store.path()));
    store.save(sampleConfig());
    QCOMPARE(toJson(store.load()), toJson(sampleConfig()));
    store.save(defaultConfig());
    QVERIFY(store.load().servers.isEmpty());
  }
  void firstRunPrompt() {
    QTemporaryDir dir;
    MainWindow window(defaultConfig(), ConfigStore(dir.filePath("config.json")));
    QVERIFY(!window.findChild<QWidget *>("welcome")->isHidden());
    QVERIFY(window.findChild<QPushButton *>("addServer"));
    SettingsDialog dialog(defaultConfig());
    dialog.beginAddServer();
    QCOMPARE(dialog.findChild<QTabWidget *>()->currentIndex(), 1);
    QCOMPARE(dialog.config().servers.size(), 1);
    QVERIFY(dialog.config().servers[0].hostname.isEmpty());
    QVERIFY(dialog.config().shares.isEmpty());
    dialog.reject();
    QVERIFY(!QFile::exists(dir.filePath("config.json")));
    MainWindow configured(sampleConfig(), ConfigStore(dir.filePath("config.json")));
    QVERIFY(configured.findChild<QWidget *>("welcome")->isHidden());
  }
  void protocolHelpersAndValidation() {
    QCOMPARE(smbMountVersion("default"), QString());
    QCOMPARE(smbMountVersion("SMB2"), QString("2.1"));
    QCOMPARE(smbMountVersion("SMB3"), QString("3"));
    QCOMPARE(smbMountVersion("3.1.1"), QString("3.1.1"));
    QCOMPARE(smbClientProtocol("default"), QString());
    QCOMPARE(smbClientProtocol("2.0"), QString("SMB2_02"));
    QCOMPARE(smbClientProtocol("3.0"), QString("SMB3_00"));
    QCOMPARE(smbClientProtocol("3.1.1"), QString("SMB3_11"));
    QCOMPARE(smbClientProtocol("SMB2"), QString("SMB2"));
    QCOMPARE(formatNfsSource("nas.example", "/exports/media"),
             QString("nas.example:/exports/media"));
    QCOMPARE(formatNfsSource("fd00::1", "/exports/media"),
             QString("[fd00::1]:/exports/media"));
    QCOMPARE(validateMountOptions("bypass=1,uid=1000"),
             QString("bypass=1,uid=1000"));
    QVERIFY_EXCEPTION_THROWN(validateMountOptions("pass=secret"),
                             std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(validateMountOptions("credentials=/etc/shadow"),
                             std::invalid_argument);
    QVERIFY_EXCEPTION_THROWN(validateMountOptions("cred=/etc/shadow"),
                             std::invalid_argument);
  }
  void previewMatchesHelperFormat() {
    auto c = sampleConfig();
    c.servers[0].hostname = "fd00::1";
    c.shares[0].localPath = "/mnt/100%media/";
    const auto text = previewUnits(c);
    QVERIFY(text.contains("What=[fd00::1]:/exports/media\n"));
    QVERIFY(text.contains("Where=/mnt/100%%media\n"));

    c.servers[0].protocol = Protocol::Smb;
    c.servers[0].hostname = "nas.example";
    c.servers[0].smbVersion = "SMB2";
    c.shares[0].remotePath = "Media";
    const auto smbText = previewUnits(c);
    QVERIFY(smbText.contains("vers=2.1"));
  }
  void parsers() {
    QCOMPARE(parseNfsExports("Export list for host:\n/exports/media *\n"),
             QStringList{"/exports/media"});
    QCOMPARE(parseSmbShares("Disk|Media|Films\nIPC|IPC$|\n"),
             QStringList{"Media"});
  }
  void escaping() {
    QCOMPARE(systemdEscapePath("/mnt/TV-Shows"), QString("mnt-TV\\x2dShows"));
    QCOMPARE(systemdEscapePath("/.hidden"), QString("\\x2ehidden"));
  }
  void configRoundTrip() {
    auto c = sampleConfig();
    auto r = fromJson(toJson(c));
    QCOMPARE(r.shares.size(), 1);
    QCOMPARE(r.servers[0].fallbackIp, QString("192.0.2.10"));
  }
};
QTEST_MAIN(Tests)
#include "tests.moc"
