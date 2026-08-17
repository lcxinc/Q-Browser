#pragma once

#include <QMetaType>
#include <QString>

enum class Engine
{
    Invalid,
    TrustedQml,
    QmlWorker,
    WebEngine,
};

Q_DECLARE_METATYPE(Engine)

struct RouteRecord
{
    QString pattern;
    Engine engine = Engine::Invalid;
    QString packageId;
    QString entryPoint;
};
