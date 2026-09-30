#pragma once

#include <QMessageBox>
#include <QPushButton>

namespace PlainMessageBox {
inline QMessageBox::StandardButton show(QWidget *parent, const QString &title,
                                        const QString &message) {
  QMessageBox box(QMessageBox::NoIcon, title, message, QMessageBox::Ok, parent);
  for (auto *button : box.findChildren<QPushButton *>())
    button->setIcon(QIcon());
  return static_cast<QMessageBox::StandardButton>(box.exec());
}

inline QMessageBox::StandardButton information(QWidget *parent,
                                               const QString &title,
                                               const QString &message) {
  return show(parent, title, message);
}
inline QMessageBox::StandardButton warning(QWidget *parent,
                                           const QString &title,
                                           const QString &message) {
  return show(parent, title, message);
}
inline QMessageBox::StandardButton critical(QWidget *parent,
                                            const QString &title,
                                            const QString &message) {
  return show(parent, title, message);
}
} // namespace PlainMessageBox
