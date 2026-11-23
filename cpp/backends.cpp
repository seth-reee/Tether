#include "backends.h"
#include <QDir>
#include <QRegularExpression>
#include <stdexcept>

static void fail(const QString &message) {
  throw std::invalid_argument(message.toStdString());
}
QString validateAddress(const QString &v) {
  auto s = v.trimmed();
  static QRegularExpression h("^(?=.{1,253}$)[A-Za-z0-9][A-Za-z0-9.:-]*$");
  if (s.isEmpty() || !h.match(s).hasMatch())
    fail("Invalid server address");
  return s;
}
QString validateAbsolutePath(const QString &v) {
  if (!v.startsWith('/') || v.contains(QChar('\0')) || v.contains('\n') ||
      v.contains('\r') || v.split('/').contains(".."))
    fail("Invalid absolute path");
  return QDir::cleanPath(v);
}
QString validateMountOptions(const QString &v) {
  auto s = v.trimmed();
  if (!isValidMountOptions(s))
    fail("Invalid mount options");
  return s;
}
QStringList parseNfsExports(const QByteArray &o) {
  QStringList result;
  for (auto line : QString::fromUtf8(o).split('\n')) {
    line = line.trimmed();
    if (line.startsWith('/'))
      result << line.section(QRegularExpression("\\s+"), 0, 0);
  }
  result.removeDuplicates();
  return result;
}
QStringList parseSmbShares(const QByteArray &o) {
  QStringList result;
  for (const auto &line : QString::fromUtf8(o).split('\n')) {
    auto f = line.split('|');
    if (f.size() > 1 && f[0].compare("Disk", Qt::CaseInsensitive) == 0)
      result << f[1];
  }
  result.removeDuplicates();
  return result;
}
