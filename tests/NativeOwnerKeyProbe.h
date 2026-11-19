#pragma once
#include <QString>
#include <functional>
class QCoreApplication;
void runNativeOwnerKeyProbe(QCoreApplication& app, const QString& mode, const QString& identifier,
                            std::function<void(bool)> finish);
