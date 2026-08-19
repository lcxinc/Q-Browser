#pragma once

#include <QByteArray>
#include <QStringList>

class QmlSourcePolicy final
{
public:
    [[nodiscard]] static QStringList violations(const QByteArray &source);
};
