#pragma once

#include "RouteRecord.h"

#include <QHash>
#include <QString>
#include <QStringView>
#include <QVector>

enum class RouteAddResult
{
    Added,
    InvalidEngine,
    InvalidPattern,
    DuplicateShape,
};

Q_DECLARE_METATYPE(RouteAddResult)

class RouteMatch final
{
public:
    RouteRecord record;
    QHash<QString, QString> parameters;

    [[nodiscard]] bool isValid() const noexcept;

private:
    friend class RouteRegistry;

    RouteMatch() = default;
    RouteMatch(RouteRecord record, QHash<QString, QString> parameters);

    bool m_valid = false;
};

class RouteRegistry final
{
public:
    [[nodiscard]] RouteAddResult add(RouteRecord record);
    [[nodiscard]] RouteMatch match(QStringView path) const;

private:
    struct Segment
    {
        bool parameter = false;
        QString value;
    };

    struct CompiledRoute
    {
        RouteRecord record;
        QVector<Segment> segments;
        QString normalizedShape;
    };

    QVector<CompiledRoute> m_routes;
};
