#include "systemdclient.h"
#include "backends.h"
#include <QJsonArray>
#include <QJsonDocument>

QString systemdEscapePath(const QString &path) {
  return escapeSystemdPath(path);
}
QString previewUnits(const AppConfig &c) {
  QString text;
  for (const auto &h : c.shares) {
    if (!h.enabled)
      continue;
    auto it = std::find_if(c.servers.begin(), c.servers.end(),
                           [&](const Server &s) { return s.id == h.serverId; });
    if (it == c.servers.end())
      continue;
    const QString localPath = validateAbsolutePath(h.localPath);
    const QString name = systemdEscapePath(localPath);
    const QString what =
        it->protocol == Protocol::Nfs
            ? formatNfsSource(it->hostname, validateAbsolutePath(h.remotePath))
            : "//" + it->hostname + "/" + h.remotePath;
    QString options =
        h.mountOptions.isEmpty() ? it->mountOptions : h.mountOptions;
    if (it->protocol == Protocol::Nfs && options.isEmpty())
      options = "nfsvers=" + it->nfsVersion;
    if (it->protocol == Protocol::Smb) {
      QStringList o{"credentials=/etc/tether/credentials/" + it->id +
                    ".cred"};
      const QString vers = smbMountVersion(it->smbVersion);
      if (!vers.isEmpty())
        o << "vers=" + vers;
      if (!options.isEmpty())
        o << options;
      options = o.join(',');
    }
    text +=
        QString(
            "### %1.mount\n[Mount]\nWhat=%2\nWhere=%3\nType=%4\nOptions=%5\n\n")
            .arg(name, escapeSystemdSpecifier(what),
                 escapeSystemdSpecifier(localPath),
                 protocolName(it->protocol) == "nfs" ? "nfs" : "cifs",
                 escapeSystemdSpecifier(options));
    if (h.automount == AutomountMode::Access)
      text += QString("### %1.automount\n[Automount]\nWhere=%2\n\n")
                  .arg(name, escapeSystemdSpecifier(localPath));
  }
  return text;
}
HelperClient::HelperClient(QObject *p) : QObject(p) {}
void HelperClient::start(const QString &program, const QJsonObject &request) {
  if (m_process) {
    emit failed("Another privileged operation is running.");
    return;
  }
  m_process = new QProcess(this);
  connect(m_process, &QProcess::finished, this, [this](int code) {
    auto out = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    auto err = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
    m_process->deleteLater();
    m_process = nullptr;
    if (code == 0)
      emit finished(out.isEmpty() ? "Operation completed." : out);
    else
      emit failed(err.isEmpty() ? "Operation failed or was cancelled." : err);
  });
  connect(m_process, &QProcess::started, this, [this, request] {
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    m_process->closeWriteChannel();
  });
  connect(m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || !m_process)
              return;
            auto message = m_process->errorString();
            m_process->deleteLater();
            m_process = nullptr;
            emit failed(message);
          });
  m_process->start("/usr/bin/pkexec", {program});
}
void HelperClient::apply(const AppConfig &c,
                         const QHash<QString, QString> &passwords) {
  QJsonObject credentials;
  for (const auto &s : c.servers)
    if (s.protocol == Protocol::Smb && serverHasEnabledShares(c, s.id))
      credentials[s.id] = QJsonObject{{"username", s.smbUsername},
                                      {"password", passwords.value(s.id)},
                                      {"domain", s.smbDomain}};
  start("/usr/lib/tether/tether-apply-helper",
        {{"config", toJson(c)}, {"credentials", credentials}});
}
void HelperClient::control(const QString &a, const QStringList &ids) {
  QJsonArray array;
  for (const auto &id : ids)
    array.append(id);
  start("/usr/lib/tether/tether-control-helper",
        {{"action", a}, {"share_ids", array}});
}
