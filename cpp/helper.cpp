#include "config.h"
#include "filetransaction.h"
#include "mountidentity.h"
#include <QLockFile>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <netdb.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>

namespace {
#ifdef TETHER_HELPER_TEST
QString systemdDir, stateDir, manifestPath;
QJsonObject testRequest;
QByteArray testMountinfo;
QStringList testCommands;
QString testFailure;
#else
const QString systemdDir = "/etc/systemd/system";
const QString stateDir = "/etc/tether";
const QString manifestPath = stateDir + "/managed.json";
#endif

[[noreturn]] void fail(const QString &message) {
  throw std::runtime_error(message.toStdString());
}

void systemctl(const QStringList &arguments, bool tolerateFailure = false) {
#ifdef TETHER_HELPER_TEST
  const auto command = arguments.join(' ');
  testCommands << command;
  if (command == testFailure) {
    testFailure.clear();
    if (!tolerateFailure) fail("Injected systemctl failure");
  }
#else
  int code = QProcess::execute("/usr/bin/systemctl", arguments);
  if (code != 0 && !tolerateFailure)
    fail("systemctl " + arguments.join(' ') + " failed");
#endif
}

#ifndef TETHER_HELPER_TEST
QByteArray stdinData() {
  QFile input;
  if (!input.open(stdin, QIODevice::ReadOnly))
    fail("Could not read request");
  return input.readAll();
}
#endif
QJsonObject parseInput() {
#ifdef TETHER_HELPER_TEST
  return testRequest;
#else
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(stdinData(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    fail("Invalid JSON request");
  return doc.object();
#endif
}

QJsonArray loadManaged() {
  QFile f(manifestPath);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll())
      .object()
      .value("shares")
      .toArray();
}

QByteArray currentMountInfo() {
#ifdef TETHER_HELPER_TEST
  return testMountinfo;
#else
  QFile mounts("/proc/self/mountinfo");
  if (!mounts.open(QIODevice::ReadOnly))
    fail("Could not read mount state");
  return mounts.readAll();
#endif
}

void verifyMount(const QJsonObject &record) {
  if (!mountIdentityMatches(currentMountInfo(),
                            QDir::cleanPath(record["local_path"].toString()),
                            record["source"].toString(),
                            record["type"].toString(),
                            !record["automount_unit"].toString().isEmpty()))
    fail("Mount identity could not be verified; refusing to change this share. "
         "Reapply legacy configurations first.");
}

#if defined(TETHER_APPLY_HELPER) || defined(TETHER_HELPER_TEST)
struct Generated {
  QString source, type;
  QString shareId, mountUnit, automountUnit, localPath, enableUnit;
  QHash<QString, QByteArray> files;
};

bool validId(const QString &v) {
  return !v.isEmpty() && v.size() <= 128 &&
         !v.contains(QRegularExpression("[^A-Za-z0-9_.-]"));
}
bool validHost(const QString &v) {
  return !v.isEmpty() && v.size() <= 253 &&
         !v.contains(QRegularExpression("[^A-Za-z0-9.:-]"));
}
bool validPath(const QString &v) {
  return v.startsWith('/') && !v.contains(QChar('\0')) && !v.contains('\n') &&
         !v.contains('\r') && !v.split('/').contains("..");
}
bool validOptions(const QString &v) { return isValidMountOptions(v); }

bool isForbiddenMountTarget(const QString &cleanPath) {
  static const QSet<QString> exact{
      "/",    "/bin",  "/boot", "/dev",  "/etc",  "/home",
      "/lib", "/lib64", "/opt", "/proc", "/root", "/run",
      "/sbin", "/srv", "/sys",  "/tmp",  "/usr",  "/var"};
  if (exact.contains(cleanPath))
    return true;
  static const QStringList prefixes{"/bin/",  "/boot/", "/dev/", "/etc/",
                                    "/lib/",  "/lib64/", "/proc/", "/run/",
                                    "/sbin/", "/sys/",  "/usr/"};
  for (const auto &prefix : prefixes)
    if (cleanPath.startsWith(prefix))
      return true;
  return false;
}

QString chosenAddress(const Server &server) {
#ifdef TETHER_HELPER_TEST
  return server.hostname;
#else
  QByteArray hostname = server.hostname.toUtf8();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  addrinfo *result = nullptr;
  if (getaddrinfo(hostname.constData(), nullptr, &hints, &result) == 0) {
    freeaddrinfo(result);
    return server.hostname;
  }
  if (!server.fallbackIp.isEmpty() && validHost(server.fallbackIp))
    return server.fallbackIp;
  fail("Hostname did not resolve and no fallback IP is configured");
#endif
}

QString escapePath(const QString &value) {
  if (!validPath(value))
    fail("Invalid local mount path");
  return escapeSystemdPath(value);
}

void writeFile(const QString &path, const QByteArray &data,
               QFileDevice::Permissions permissions) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(permissions) ||
      file.write(data) != data.size() || !file.commit())
    fail("Could not write " + path);
  if (!QFile::setPermissions(path, permissions))
    fail("Could not secure " + path);
}

void mkpathPreservingOwner(const QString &cleanPath) {
  QStringList missing;
  QString cur = cleanPath;
  while (cur != "/" && !QFileInfo::exists(cur)) {
    if (QFileInfo(cur).isSymLink())
      fail("Mount destinations must not traverse symlinks");
    missing.append(cur);
    const QString parent = QFileInfo(cur).absolutePath();
    if (parent == cur)
      break;
    cur = parent;
  }
  struct stat st{};
  const bool hasOwner = (::stat(cur.toUtf8().constData(), &st) == 0);
  for (int i = missing.size() - 1; i >= 0; --i) {
    const QString &dir = missing[i];
    if (!QDir().mkdir(dir))
      fail("Could not create mount directory " + dir);
    if (hasOwner && st.st_uid != 0)
      (void)::chown(dir.toUtf8().constData(), st.st_uid, st.st_gid);
  }
}

Generated generate(const Server &server, const Share &share) {
  if (!validId(server.id) || !validId(share.id) ||
      !validHost(server.hostname) || !validPath(share.localPath) ||
      !validOptions(server.mountOptions) || !validOptions(share.mountOptions) ||
      share.name.contains(QRegularExpression("[\\n\\r\\x00]")))
    fail("Invalid configuration for share " + share.name);
  if (!server.fallbackIp.isEmpty() && !validHost(server.fallbackIp))
    fail("Invalid fallback address");
  const QString localPath = QDir::cleanPath(share.localPath);
  QString what, type,
      options = share.mountOptions.isEmpty() ? server.mountOptions
                                             : share.mountOptions;
  const QString address = chosenAddress(server);
  if (server.protocol == Protocol::Nfs) {
    if (!validPath(share.remotePath))
      fail("Invalid NFS export path");
    what = formatNfsSource(address, QDir::cleanPath(share.remotePath));
    type = "nfs";
    if (options.isEmpty())
      options = "nfsvers=" + server.nfsVersion;
  } else {
    if (share.remotePath.isEmpty() ||
        share.remotePath.contains(QRegularExpression("[\\\\/\\n\\r\\x00]")) ||
        server.smbUsername.isEmpty())
      fail("Invalid SMB configuration");
    what = "//" + address + "/" + share.remotePath;
    type = "cifs";
    QStringList values{"credentials=" + stateDir + "/credentials/" + server.id +
                       ".cred"};
    const QString vers = smbMountVersion(server.smbVersion);
    if (!vers.isEmpty())
      values << "vers=" + vers;
    if (!options.isEmpty())
      values << options;
    options = values.join(',');
  }
  Generated result;
  result.source = what;
  result.type = type;
  result.shareId = share.id;
  result.localPath = localPath;
  auto escaped = escapePath(localPath);
  result.mountUnit = escaped + ".mount";
  result.files[result.mountUnit] =
      QString("[Unit]\nDescription=Tether %1 share "
              "%2\nWants=network-online.target\nAfter=network-online."
              "target\n\n[Mount]\nWhat=%3\nWhere=%4\nType=%5\nOptions=%6\n\n["
              "Install]\nWantedBy=multi-user.target\n")
          .arg(protocolName(server.protocol).toUpper(),
               escapeSystemdSpecifier(share.name),
               escapeSystemdSpecifier(what),
               escapeSystemdSpecifier(localPath), type,
               escapeSystemdSpecifier(options))
          .toUtf8();
  if (share.automount == AutomountMode::Access) {
    result.automountUnit = escaped + ".automount";
    result.enableUnit = result.automountUnit;
    result.files[result.automountUnit] =
        QString(
            "[Unit]\nDescription=Automount Tether share "
            "%1\nWants=network-online.target\nAfter=network-online.target\n\n["
            "Automount]\nWhere=%2\n\n[Install]\nWantedBy=multi-user.target\n")
            .arg(escapeSystemdSpecifier(share.name),
                 escapeSystemdSpecifier(localPath))
            .toUtf8();
  } else if (share.automount == AutomountMode::Boot)
    result.enableUnit = result.mountUnit;
  return result;
}

bool isLegacyOmamounterUnit(const QString &name, const QString &localPath) {
  QFileInfo info(systemdDir + "/" + name);
  if (info.isSymLink() || !info.isFile() || info.size() > 65536)
    return false;
  QFile file(info.filePath());
  if (!file.open(QIODevice::ReadOnly))
    return false;
  QString section;
  QHash<QString, QString> unit, mount, install;
  for (const auto &raw : QString::fromUtf8(file.readAll()).split('\n')) {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith('#'))
      continue;
    if (line.startsWith('[') && line.endsWith(']')) {
      section = line.mid(1, line.size() - 2);
      if (section != "Unit" && section != "Mount" && section != "Install")
        return false;
      continue;
    }
    const int equals = line.indexOf('=');
    if (equals <= 0)
      return false;
    const QString key = line.left(equals), value = line.mid(equals + 1);
    auto *fields = section == "Unit" ? &unit : section == "Mount" ? &mount
                             : section == "Install" ? &install : nullptr;
    const bool knownKey =
        (section == "Unit" &&
         QSet<QString>{"Description", "Wants", "After"}.contains(key)) ||
        (section == "Mount" &&
         QSet<QString>{"What", "Where", "Type", "Options"}.contains(key)) ||
        (section == "Install" && key == "WantedBy");
    if (!fields || !knownKey || fields->contains(key))
      return false;
    fields->insert(key, value);
  }
  const auto description = unit.value("Description");
  const auto type = mount.value("Type");
  const bool smb = description.startsWith("omamounter SMB share ") &&
                   type == "cifs" &&
                   mount.value("What").startsWith("//") &&
                   mount.value("Options").contains(
                       "credentials=/etc/omamounter/credentials/");
  const bool nfs = description.startsWith("omamounter NFS share ") &&
                   type == "nfs" && mount.value("What").contains(':');
  return (smb || nfs) && mount.value("Where") == localPath &&
         name == escapePath(localPath) + ".mount" &&
         !unit.value("Wants").isEmpty() && !unit.value("After").isEmpty() &&
         !install.value("WantedBy").isEmpty();
}

void apply() {
  auto request = parseInput(),
       configObject = request.value("config").toObject(),
       credentials = request.value("credentials").toObject();
  AppConfig config = fromJson(configObject);
  QHash<QString, Server> servers;
  for (const auto &s : config.servers) {
    if (!validId(s.id) || !validHost(s.hostname))
      fail("Invalid server identity");
    if (!allowedNfsVersions().contains(s.nfsVersion))
      fail("Unsupported NFS version");
    if (!allowedSmbVersions().contains(s.smbVersion))
      fail("Unsupported SMB version");
    if (s.protocol == Protocol::Smb && serverHasEnabledShares(config, s.id)) {
      auto c = credentials.value(s.id).toObject();
      const auto user = c["username"].toString(),
                 password = c["password"].toString(),
                 domain = c["domain"].toString();
      if (user != s.smbUsername || user.isEmpty() || password.isEmpty() ||
          (user + password + domain)
              .contains(QRegularExpression("[\\n\\r\\x00]")))
        fail("Invalid or missing SMB credentials");
    }
    if (servers.contains(s.id))
      fail("Duplicate server ID");
    servers[s.id] = s;
  }
  QVector<Generated> generated;
  QSet<QString> shareIds;
  for (const auto &share : config.shares)
    if (share.enabled) {
      if (shareIds.contains(share.id))
        fail("Duplicate share ID");
      shareIds.insert(share.id);
      if (!servers.contains(share.serverId))
        fail("Share references missing server");
      generated << generate(servers[share.serverId], share);
    }
  QSet<QString> newUnits;
  for (const auto &g : generated)
    for (auto it = g.files.begin(); it != g.files.end(); ++it) {
      if (newUnits.contains(it.key()))
        fail("Two shares use the same mount destination");
      newUnits.insert(it.key());
    }
  const auto oldManifest = loadManaged();
  QHash<QString, QJsonObject> oldByPath;
  for (const auto &v : oldManifest) {
    const auto o = v.toObject();
    oldByPath.insert(QDir::cleanPath(o["local_path"].toString()), o);
  }
  // Check every target before any unit removal, directory creation, or write.
  for (const auto &g : generated) {
    QString path = g.localPath;
    if (isForbiddenMountTarget(path))
      fail("Refusing a system directory as a mount destination");
    while (path != "/") {
      if (QFileInfo(path).isSymLink())
        fail("Mount destinations must not traverse symlinks");
      path = QFileInfo(path).absolutePath();
    }
    if (oldByPath.contains(g.localPath)) {
      verifyMount(oldByPath.value(g.localPath));
    } else {
      verifyMount(QJsonObject{{"local_path", g.localPath},
                              {"source", g.source},
                              {"type", g.type},
                              {"automount_unit", ""}});
    }
  }
  QSet<QString> ownedUnits;
  for (const auto &v : oldManifest) {
    ownedUnits.insert(v.toObject()["mount_unit"].toString());
    ownedUnits.insert(v.toObject()["automount_unit"].toString());
  }
  for (const auto &g : generated)
    if (!ownedUnits.contains(g.mountUnit) &&
        isLegacyOmamounterUnit(g.mountUnit, g.localPath)) {
      if (mountStatus(currentMountInfo(), g.localPath, {g.source}, g.type) !=
          "Unmounted")
        fail("Unmount the old OmaMounter share at " + g.localPath +
             " before applying its Tether replacement.");
      ownedUnits.insert(g.mountUnit);
    }
  for (const auto &name : newUnits) {
    if (!ownedUnits.contains(name) &&
        (QFileInfo::exists(systemdDir + "/" + name) ||
         QFileInfo::exists("/usr/lib/systemd/system/" + name) ||
         QFileInfo::exists("/run/systemd/system/" + name)))
      fail("Another application owns systemd unit " + name +
           ". Choose a different local path for this share in Tether's Shares "
           "tab, or retire the existing unit before applying again.");
  }
  const QString credDirPath = stateDir + "/credentials";
  QSet<QString> existingCredFiles;
  for (const auto &name :
       QDir(credDirPath).entryList({"*.cred"}, QDir::Files))
    existingCredFiles.insert(credDirPath + "/" + name);
  QSet<QString> activeCredFiles;
  for (const auto &server : config.servers)
    if (server.protocol == Protocol::Smb &&
        serverHasEnabledShares(config, server.id))
      activeCredFiles.insert(credDirPath + "/" + server.id + ".cred");

  FileTransaction transaction;
  transaction.capture(manifestPath);
  for (const auto &name : ownedUnits)
    if (!name.isEmpty())
      transaction.capture(systemdDir + "/" + name);
  for (const auto &name : newUnits)
    transaction.capture(systemdDir + "/" + name);
  for (const auto &path : existingCredFiles)
    transaction.capture(path);
  for (const auto &path : activeCredFiles)
    transaction.capture(path);
  try {
    for (const auto &v : oldManifest) {
      auto o = v.toObject();
      for (const auto &name :
           {o["automount_unit"].toString(), o["mount_unit"].toString()})
        if (!name.isEmpty() && !newUnits.contains(name)) {
          verifyMount(o);
          // Stop before removal; enablement is reconciled after files commit.
          systemctl({"stop", name});
          if (QFile::exists(systemdDir + "/" + name) &&
              !QFile::remove(systemdDir + "/" + name))
            fail("Could not remove retired unit");
        }
    }
    for (const auto &g : generated) {
      if (!oldByPath.contains(g.localPath))
        continue;
      const auto old = oldByPath.value(g.localPath);
      QByteArray oldBytes;
      QFile existingUnit(systemdDir + "/" + g.mountUnit);
      if (existingUnit.open(QIODevice::ReadOnly))
        oldBytes = existingUnit.readAll();
      const bool unitChanged = old["source"].toString() != g.source ||
                               old["type"].toString() != g.type ||
                               oldBytes != g.files.value(g.mountUnit);
      const bool switchingToAccess =
          !g.automountUnit.isEmpty() &&
          old["automount_unit"].toString().isEmpty();
      if ((unitChanged || switchingToAccess) &&
          mountStatus(currentMountInfo(), g.localPath,
                      {old["source"].toString()},
                      old["type"].toString()) != "Unmounted") {
        verifyMount(old);
        const QString oldAuto = old["automount_unit"].toString();
        if (!oldAuto.isEmpty() && newUnits.contains(oldAuto))
          systemctl({"stop", oldAuto});
        systemctl({"stop", old["mount_unit"].toString()});
      }
    }
    QDir().mkpath(systemdDir);
    QDir().mkpath(credDirPath);
    QFile::setPermissions(stateDir, QFileDevice::ReadOwner |
                                        QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
    QFile::setPermissions(credDirPath, QFileDevice::ReadOwner |
                                           QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
    for (const auto &g : generated) {
      mkpathPreservingOwner(g.localPath);
      for (auto it = g.files.begin(); it != g.files.end(); ++it)
        writeFile(systemdDir + "/" + it.key(), it.value(),
                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                      QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    for (const auto &s : config.servers)
      if (s.protocol == Protocol::Smb && serverHasEnabledShares(config, s.id)) {
        auto c = credentials.value(s.id).toObject();
        auto user = c["username"].toString(),
             password = c["password"].toString(),
             domain = c["domain"].toString();
        if (user != s.smbUsername || user.isEmpty() || password.isEmpty() ||
            (user + password + domain)
                .contains(QRegularExpression("[\\n\\r\\x00]")))
          fail("Invalid or missing SMB credentials");
        QByteArray content = "username=" + user.toUtf8() +
                             "\npassword=" + password.toUtf8() + "\n";
        if (!domain.isEmpty())
          content += "domain=" + domain.toUtf8() + "\n";
        writeFile(credDirPath + "/" + s.id + ".cred", content,
                  QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      }
    for (const auto &staleCred : existingCredFiles)
      if (!activeCredFiles.contains(staleCred) && QFile::exists(staleCred) &&
          !QFile::remove(staleCred))
        fail("Could not remove unused credentials file");
    QJsonArray manifest;
    for (const auto &g : generated)
      manifest.append(QJsonObject{{"share_id", g.shareId},
                                  {"local_path", g.localPath},
                                  {"source", g.source},
                                  {"type", g.type},
                                  {"mount_unit", g.mountUnit},
                                  {"automount_unit", g.automountUnit}});
    writeFile(manifestPath,
              QJsonDocument(QJsonObject{{"shares", manifest}})
                  .toJson(QJsonDocument::Indented),
              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // Persist ownership before activation: an offline server must not leave
    // successfully installed units absent from the control manifest.
    systemctl({"daemon-reload"});
  } catch (const std::exception &error) {
    const QString cause = QString::fromUtf8(error.what());
    transaction.rollback();
    systemctl({"daemon-reload"});
    fail(cause + "\nConfiguration files restored. Previously stopped shares "
                 "may need mounting again.");
  }
  for (const auto &name : ownedUnits)
    if (!name.isEmpty() && !newUnits.contains(name))
      systemctl({"disable", name}, true);
  QStringList failures;
  for (const auto &g : generated) {
    try {
      systemctl({"disable", g.mountUnit});
      if (!g.automountUnit.isEmpty())
        systemctl({"disable", g.automountUnit});
      if (!g.enableUnit.isEmpty())
        systemctl({"enable", "--now", g.enableUnit});
    } catch (const std::exception &error) {
      failures << QString::fromUtf8(error.what());
    }
  }
  if (!failures.isEmpty())
    fail("Configuration installed; some shares could not activate. Retry after "
         "checking the server.\n" +
         failures.join('\n'));
}
#endif

#if defined(TETHER_CONTROL_HELPER) || defined(TETHER_HELPER_TEST)
void control() {
  auto request = parseInput();
  auto action = request["action"].toString();
  if (action != "mount" && action != "unmount")
    fail("Invalid action");
  QHash<QString, QJsonObject> managed;
  for (const auto &v : loadManaged()) {
    auto o = v.toObject();
    managed[o["share_id"].toString()] = o;
  }
  auto ids = request["share_ids"].toArray();
  if (ids.isEmpty())
    fail("No shares requested");
  QStringList failures;
  for (const auto &v : ids) {
    const auto id = v.toString();
    try {
      if (!managed.contains(id))
        fail("Share is not in the root-owned manifest");
      verifyMount(managed[id]);
      if (action == "unmount" &&
          !managed[id]["automount_unit"].toString().isEmpty())
        systemctl({"stop", managed[id]["automount_unit"].toString()});
      verifyMount(managed[id]);
      systemctl({action == "mount" ? "start" : "stop",
                 managed[id]["mount_unit"].toString()});
    } catch (const std::exception &error) {
      failures << (id + ": " + QString::fromUtf8(error.what()));
    }
  }
  if (!failures.isEmpty())
    fail(failures.join('\n'));
}
#endif
} // namespace

#ifndef TETHER_HELPER_TEST
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (geteuid() != 0) {
    fprintf(stderr, "Tether helper must run as root\n");
    return 1;
  }
  QLockFile lock("/run/tether.lock");
  if (!lock.tryLock(0)) {
    fprintf(stderr, "Another Tether operation is running\n");
    return 1;
  }
  try {
#ifdef TETHER_APPLY_HELPER
    apply();
#else
    control();
#endif
    fprintf(stdout, "{\"ok\":true}\n");
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "Tether helper: %s\n", e.what());
    return 1;
  }
}
#endif
