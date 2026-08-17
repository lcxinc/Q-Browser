#pragma once

#include <QMetaType>
#include <QString>

enum class Engine
{
    TrustedQml,
    QmlWorker,
    WebEngine,
};

Q_DECLARE_METATYPE(Engine)

struct RouteRecord
{
    QString pattern;
    Engine engine = Engine::TrustedQml;
    QString packageId;
    QString entryPoint;
};
