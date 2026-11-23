#pragma once
#include "config.h"
#include <QStringList>

QString validateAddress(const QString &value);
QString validateAbsolutePath(const QString &value);
QString validateMountOptions(const QString &value);
QStringList parseNfsExports(const QByteArray &output);
QStringList parseSmbShares(const QByteArray &output);
