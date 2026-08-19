#include "SafeEvent.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr qint64 MaximumDurationMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
constexpr double MaximumMetricMagnitude = 1.0e12;
constexpr qsizetype MaximumMetrics = 8;

QString phaseName(const SafeEventPhase phase)
{
    switch (phase) {
    case SafeEventPhase::Install: return QStringLiteral("install");
    case SafeEventPhase::Activate: return QStringLiteral("activate");
    case SafeEventPhase::Worker: return QStringLiteral("worker");
    case SafeEventPhase::Health: return QStringLiteral("health");
    case SafeEventPhase::Rollback: return QStringLiteral("rollback");
    case SafeEventPhase::Startup: return QStringLiteral("startup");
    }
    return {};
}

QString codeName(const SafeEventCode code)
{
    switch (code) {
    case SafeEventCode::Started: return QStringLiteral("started");
    case SafeEventCode::Completed: return QStringLiteral("completed");
    case SafeEventCode::Rejected: return QStringLiteral("rejected");
    case SafeEventCode::Failed: return QStringLiteral("failed");
    case SafeEventCode::Restarted: return QStringLiteral("restarted");
    case SafeEventCode::Healthy: return QStringLiteral("healthy");
    case SafeEventCode::Recovered: return QStringLiteral("recovered");
    }
    return {};
}

QString metricName(const SafeMetric metric)
{
    switch (metric) {
    case SafeMetric::HeartbeatCount: return QStringLiteral("heartbeatCount");
    case SafeMetric::RestartCount: return QStringLiteral("restartCount");
    case SafeMetric::AttemptNumber: return QStringLiteral("attemptNumber");
    case SafeMetric::QueueDepth: return QStringLiteral("queueDepth");
    case SafeMetric::PackageBytes: return QStringLiteral("packageBytes");
    }
    return {};
}

bool validAppId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^[a-z][a-z0-9]*(?:-[a-z0-9]+)*(?:\.[a-z][a-z0-9]*(?:-[a-z0-9]+)*)+$)"));
    if (value.size() > 253 || value.normalized(QString::NormalizationForm_C) != value
        || !pattern.match(value).hasMatch()) {
        return false;
    }
    const QStringList labels = value.split(u'.');
    return std::ranges::all_of(labels, [](const QString &label) {
        return label.size() <= 63;
    });
}

bool validVersion(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)"));
    return value.size() <= 128
        && value.normalized(QString::NormalizationForm_C) == value
        && pattern.match(value).hasMatch();
}

bool validRouteTemplate(const QString &value)
{
    if (value.isEmpty() || value.size() > 256 || !value.startsWith(u'/')
        || value.contains(u'?') || value.contains(u'#') || value.contains(u'\\')
        || value.contains(QStringLiteral("//"))
        || value.normalized(QString::NormalizationForm_C) != value) {
        return false;
    }
    if (value == QStringLiteral("/")) return true;
    static const QRegularExpression literal(QStringLiteral(R"(^[a-z][a-z0-9-]*$)"));
    static const QRegularExpression parameter(QStringLiteral(R"(^:[a-z][a-z0-9]*$)"));
    const QStringList segments = value.sliced(1).split(u'/', Qt::KeepEmptyParts);
    return std::ranges::all_of(segments, [&](const QString &segment) {
        return literal.match(segment).hasMatch()
            || parameter.match(segment).hasMatch();
    });
}

QByteArray quotedAscii(const QString &value)
{
    // Construction restricts strings to these JSON-safe ASCII alphabets.
    return QByteArrayLiteral("\"") + value.toUtf8() + QByteArrayLiteral("\"");
}

QByteArray number(const double value)
{
    if (value == 0.0) return QByteArrayLiteral("0");
    return QByteArray::number(value, 'g', 17);
}

SafeEventResult rejected(const QString &code)
{
    return {std::nullopt, code};
}
}

bool SafeEventResult::hasValue() const noexcept
{
    return event.has_value();
}

const SafeEvent &SafeEventResult::value() const
{
    return event.value();
}

SafeEventResult SafeEvent::create(
    const qint64 timestamp,
    const QString &appId,
    const QString &packageVersion,
    const SafeEventPhase phase,
    const SafeEventCode code,
    const qint64 durationMs,
    const QString &routeTemplate,
    const SafeMetrics &metrics)
{
    if (timestamp < 0) return rejected(QStringLiteral("telemetry.invalid_timestamp"));
    if (!validAppId(appId)) return rejected(QStringLiteral("telemetry.invalid_app_id"));
    if (!validVersion(packageVersion)) {
        return rejected(QStringLiteral("telemetry.invalid_package_version"));
    }
    if (phaseName(phase).isEmpty()) return rejected(QStringLiteral("telemetry.invalid_phase"));
    if (codeName(code).isEmpty()) return rejected(QStringLiteral("telemetry.invalid_code"));
    if (durationMs < 0 || durationMs > MaximumDurationMs) {
        return rejected(QStringLiteral("telemetry.invalid_duration"));
    }
    if (!validRouteTemplate(routeTemplate)) {
        return rejected(QStringLiteral("telemetry.invalid_route_template"));
    }
    if (metrics.size() > MaximumMetrics) {
        return rejected(QStringLiteral("telemetry.too_many_metrics"));
    }
    QSet<int> seen;
    SafeMetrics canonical = metrics;
    for (const auto &[key, value] : canonical) {
        const int numericKey = static_cast<int>(key);
        if (metricName(key).isEmpty() || seen.contains(numericKey)
            || !std::isfinite(value) || value < 0.0
            || std::abs(value) > MaximumMetricMagnitude) {
            return rejected(QStringLiteral("telemetry.invalid_metric"));
        }
        seen.insert(numericKey);
    }
    std::ranges::sort(canonical, [](const auto &left, const auto &right) {
        return metricName(left.first) < metricName(right.first);
    });
    return {SafeEvent(timestamp, appId, packageVersion, phase, code, durationMs,
                      routeTemplate, std::move(canonical)), {}};
}

SafeEvent::SafeEvent(const qint64 timestamp,
                     QString appId,
                     QString packageVersion,
                     const SafeEventPhase phase,
                     const SafeEventCode code,
                     const qint64 durationMs,
                     QString routeTemplate,
                     SafeMetrics metrics)
    : timestamp_(timestamp)
    , appId_(std::move(appId))
    , packageVersion_(std::move(packageVersion))
    , phase_(phase)
    , code_(code)
    , durationMs_(durationMs)
    , routeTemplate_(std::move(routeTemplate))
    , metrics_(std::move(metrics))
{
}

QByteArray SafeEvent::toJson() const
{
    QByteArray result = QByteArrayLiteral("{\"timestamp\":")
        + QByteArray::number(timestamp_)
        + QByteArrayLiteral(",\"appId\":") + quotedAscii(appId_)
        + QByteArrayLiteral(",\"packageVersion\":") + quotedAscii(packageVersion_)
        + QByteArrayLiteral(",\"phase\":") + quotedAscii(phaseName(phase_))
        + QByteArrayLiteral(",\"code\":") + quotedAscii(codeName(code_))
        + QByteArrayLiteral(",\"durationMs\":") + QByteArray::number(durationMs_)
        + QByteArrayLiteral(",\"routeTemplate\":") + quotedAscii(routeTemplate_)
        + QByteArrayLiteral(",\"metrics\":{");
    for (qsizetype index = 0; index < metrics_.size(); ++index) {
        if (index != 0) result += ',';
        result += quotedAscii(metricName(metrics_.at(index).first));
        result += ':';
        result += number(metrics_.at(index).second);
    }
    result += QByteArrayLiteral("}}");
    return result;
}
