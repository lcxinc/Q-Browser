#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

#include <optional>

enum class SafeEventPhase
{
    Install,
    Activate,
    Worker,
    Health,
    Rollback,
    Startup,
};

enum class SafeEventCode
{
    Started,
    Completed,
    Rejected,
    Failed,
    Restarted,
    Healthy,
    Recovered,
};

enum class SafeMetric
{
    HeartbeatCount,
    RestartCount,
    AttemptNumber,
    QueueDepth,
    PackageBytes,
};

using SafeMetrics = QVector<QPair<SafeMetric, double>>;

struct SafeEventResult;

class SafeEvent final
{
public:
    [[nodiscard]] static SafeEventResult create(
        qint64 timestamp,
        const QString &appId,
        const QString &packageVersion,
        SafeEventPhase phase,
        SafeEventCode code,
        qint64 durationMs,
        const QString &routeTemplate,
        const SafeMetrics &metrics);

    [[nodiscard]] QByteArray toJson() const;

private:
    SafeEvent(qint64 timestamp,
              QString appId,
              QString packageVersion,
              SafeEventPhase phase,
              SafeEventCode code,
              qint64 durationMs,
              QString routeTemplate,
              SafeMetrics metrics);

    qint64 timestamp_ = 0;
    QString appId_;
    QString packageVersion_;
    SafeEventPhase phase_ = SafeEventPhase::Install;
    SafeEventCode code_ = SafeEventCode::Started;
    qint64 durationMs_ = 0;
    QString routeTemplate_;
    SafeMetrics metrics_;
};

struct SafeEventResult final
{
    std::optional<SafeEvent> event;
    QString stableError;

    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const SafeEvent &value() const;
};
