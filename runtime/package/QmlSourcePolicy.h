#pragma once

#include <QByteArray>
#include <QStringList>

class QmlSourcePolicy final
{
public:
    [[nodiscard]] static bool isQmlSourcePath(QByteArrayView path);
    [[nodiscard]] static QStringList staticImports(const QByteArray &source);
    [[nodiscard]] static QStringList violations(const QByteArray &source);
};
