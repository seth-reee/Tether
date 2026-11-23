#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <stdexcept>

QString protocolName(Protocol value) {
  return value == Protocol::Nfs ? "nfs" : "smb";
}
QString automountName(AutomountMode value) {
  if (value == AutomountMode::Boot)
    return "boot";
  if (value == AutomountMode::Access)
    return "access";
  return "disabled";
}
QStringList allowedNfsVersions() {
  return {"3", "4", "4.0", "4.1", "4.2"};
}
QStringList allowedSmbVersions() {
  return {"default", "2.0", "2.1", "3", "3.0", "3.02", "3.1.1", "SMB2", "SMB3"};
}
QString smbMountVersion(const QString &version) {
  if (version.isEmpty() || version == "default")
    return {};
  if (version == "SMB2")
    return "2.1";
  if (version == "SMB3")
    return "3";
  return version;
}
QString smbClientProtocol(const QString &version) {
  if (version.isEmpty() || version == "default")
    return {};
  if (version == "2.0")
    return "SMB2_02";
  if (version == "2.1")
    return "SMB2_10";
  if (version == "SMB2")
    return "SMB2";
  if (version == "3" || version == "SMB3")
    return "SMB3";
  if (version == "3.0")
    return "SMB3_00";
  if (version == "3.02")
    return "SMB3_02";
  if (version == "3.1.1")
    return "SMB3_11";
  return version;
}
QString formatNfsSource(const QString &address, const QString &remotePath) {
  const bool ipv6Literal =
      address.contains(':') && !address.startsWith('[');
  return (ipv6Literal ? "[" + address + "]" : address) + ":" + remotePath;
}
QString escapeSystemdSpecifier(QString value) {
  return value.replace('%', "%%");
}
QString escapeSystemdPath(const QString &path) {
  if (!path.startsWith('/') || path.contains(QChar('\0')) ||
      path.contains('\n') || path.contains('\r') ||
      path.split('/').contains(".."))
    throw std::invalid_argument("Invalid absolute path");
  const QByteArray raw = QDir::cleanPath(path).mid(1).toUtf8();
  if (raw.isEmpty())
    return "-";
  QByteArray out;
  for (int i = 0; i < raw.size(); ++i) {
    const uchar b = static_cast<uchar>(raw[i]);
    if (i == 0 && b == '.')
      out += "\\x2e";
    else if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
             (b >= '0' && b <= '9') || b == '_' || b == '.')
      out += char(b);
    else if (b == '/')
      out += '-';
    else
      out += QString("\\x%1").arg(b, 2, 16, QChar('0')).toLatin1();
  }
  return QString::fromLatin1(out);
}
bool isValidMountOptions(const QString &value) {
  if (value.size() > 1024 ||
      value.contains(QRegularExpression("[\\s\\x00]")))
    return false;
  if (value.isEmpty())
    return true;
  if (value.startsWith(',') || value.endsWith(',') || value.contains(",,"))
    return false;
  for (const auto &token : value.split(',')) {
    const QString key = token.section('=', 0, 0).toLower();
    if (key == "password" || key == "pass" || key == "credentials" ||
        key == "cred")
      return false;
  }
  return true;
}
static Protocol protocolFrom(const QString &value) {
  return value.compare("smb", Qt::CaseInsensitive) == 0 ? Protocol::Smb
                                                        : Protocol::Nfs;
}
static AutomountMode automountFrom(const QString &value) {
  if (value == "boot")
    return AutomountMode::Boot;
  if (value == "access")
    return AutomountMode::Access;
  return AutomountMode::Disabled;
}

QJsonObject toJson(const AppConfig &config) {
  QJsonArray servers, shares;
  for (const auto &s : config.servers)
    servers.append(QJsonObject{{"id", s.id},
                               {"name", s.name},
                               {"hostname", s.hostname},
                               {"fallback_ip", s.fallbackIp},
                               {"protocol", protocolName(s.protocol)},
                               {"nfs_version", s.nfsVersion},
                               {"smb_username", s.smbUsername},
                               {"smb_domain", s.smbDomain},
                               {"smb_version", s.smbVersion},
                               {"mount_options", s.mountOptions}});
  for (const auto &s : config.shares)
    shares.append(QJsonObject{{"id", s.id},
                              {"server_id", s.serverId},
                              {"name", s.name},
                              {"remote_path", s.remotePath},
                              {"local_path", s.localPath},
                              {"mount_options", s.mountOptions},
                              {"automount", automountName(s.automount)},
                              {"enabled", s.enabled}});
  return QJsonObject{{"schema_version", config.schemaVersion},
                     {"mount_root", config.mountRoot},
                     {"servers", servers},
                     {"shares", shares}};
}

AppConfig fromJson(const QJsonObject &o) {
  AppConfig c;
  c.schemaVersion = o.value("schema_version").toInt(1);
  c.mountRoot = o.value("mount_root").toString();
  for (const auto &v : o.value("servers").toArray()) {
    auto x = v.toObject();
    c.servers.append(
        {x["id"].toString(), x["name"].toString(), x["hostname"].toString(),
         x["fallback_ip"].toString(), protocolFrom(x["protocol"].toString()),
         x["nfs_version"].toString("4"), x["smb_username"].toString(),
         x["smb_domain"].toString(), x["smb_version"].toString("default"),
         x["mount_options"].toString()});
  }
  for (const auto &v : o.value("shares").toArray()) {
    auto x = v.toObject();
    c.shares.append({x["id"].toString(), x["server_id"].toString(),
                     x["name"].toString(), x["remote_path"].toString(),
                     x["local_path"].toString(), x["mount_options"].toString(),
                     automountFrom(x["automount"].toString()),
                     x["enabled"].toBool(true)});
  }
  return c;
}

AppConfig defaultConfig() {
  AppConfig c;
  c.mountRoot = QDir::homePath() + "/Mount";
  return c;
}
bool serverHasEnabledShares(const AppConfig &config, const QString &serverId) {
  for (const auto &share : config.shares)
    if (share.enabled && share.serverId == serverId)
      return true;
  return false;
}

ConfigStore::ConfigStore(QString path) : m_path(std::move(path)) {
  if (m_path.isEmpty())
    m_path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
             "/tether/config.json";
}
AppConfig ConfigStore::load() const {
  QFile file(m_path);
  if (!file.exists())
    return defaultConfig();
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error(file.errorString().toStdString());
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    throw std::runtime_error(error.errorString().toStdString());
  return fromJson(doc.object());
}
void ConfigStore::save(const AppConfig &config) const {
  QDir().mkpath(QFileInfo(m_path).absolutePath());
  QSaveFile file(m_path);
  if (!file.open(QIODevice::WriteOnly))
    throw std::runtime_error(file.errorString().toStdString());
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.write(QJsonDocument(toJson(config)).toJson(QJsonDocument::Indented));
  if (!file.commit())
    throw std::runtime_error(file.errorString().toStdString());
  QFile::setPermissions(m_path,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
