#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

enum class Protocol { Nfs, Smb };
enum class AutomountMode { Disabled, Boot, Access };

struct Server {
  QString id, name, hostname, fallbackIp;
  Protocol protocol = Protocol::Nfs;
  QString nfsVersion = "4", smbUsername, smbDomain, smbVersion = "default",
          mountOptions;
};

struct Share {
  QString id, serverId, name, remotePath, localPath, mountOptions;
  AutomountMode automount = AutomountMode::Disabled;
  bool enabled = true;
};

struct AppConfig {
  int schemaVersion = 1;
  QString mountRoot;
  QVector<Server> servers;
  QVector<Share> shares;
};

QString protocolName(Protocol value);
QString automountName(AutomountMode value);
QStringList allowedNfsVersions();
QStringList allowedSmbVersions();
QString smbMountVersion(const QString &version);
QString smbClientProtocol(const QString &version);
QString formatNfsSource(const QString &address, const QString &remotePath);
QString escapeSystemdSpecifier(QString value);
QString escapeSystemdPath(const QString &path);
bool isValidMountOptions(const QString &value);
QJsonObject toJson(const AppConfig &config);
AppConfig fromJson(const QJsonObject &object);
AppConfig defaultConfig();
bool serverHasEnabledShares(const AppConfig &, const QString &serverId);

class ConfigStore {
public:
  explicit ConfigStore(QString path = {});
  QString path() const { return m_path; }
  AppConfig load() const;
  void save(const AppConfig &config) const;

private:
  QString m_path;
};
