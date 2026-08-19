#include "SafeEvent.h"
#include "EventRecorder.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>

#include <cmath>
#include <limits>
#include <thread>
#include <vector>

class SafeEventTest final : public QObject
{
    Q_OBJECT

private slots:
    void serializesOnlyAllowlistedCanonicalFields();
    void rejectsSecretsAndUserControlledTextAtConstruction();
    void validatesBoundsUnicodeRoutesAndMetrics();
    void recorderIsAsynchronousBoundedAndRotates();
    void recorderIsThreadSafeAndReportsTypedWriteFailures();
};

void SafeEventTest::serializesOnlyAllowlistedCanonicalFields()
{
    const SafeEventResult created = SafeEvent::create(
        1'723'456'789'012,
        QStringLiteral("company.pilot"),
        QStringLiteral("1.2.0"),
        SafeEventPhase::Health,
        SafeEventCode::Healthy,
        31'000,
        QStringLiteral("/orders/:id"),
        {{SafeMetric::HeartbeatCount, 42.0},
         {SafeMetric::RestartCount, 1.0}});
    QVERIFY2(created.hasValue(), qPrintable(created.stableError));

    const QByteArray expected = QByteArrayLiteral(
        "{\"timestamp\":1723456789012,\"appId\":\"company.pilot\","
        "\"packageVersion\":\"1.2.0\",\"phase\":\"health\","
        "\"code\":\"healthy\",\"durationMs\":31000,"
        "\"routeTemplate\":\"/orders/:id\",\"metrics\":{"
        "\"heartbeatCount\":42,\"restartCount\":1}}");
    QCOMPARE(created.value().toJson(), expected);

    const QJsonObject object = QJsonDocument::fromJson(expected).object();
    QCOMPARE(object.keys(), QStringList({QStringLiteral("appId"),
                                        QStringLiteral("code"),
                                        QStringLiteral("durationMs"),
                                        QStringLiteral("metrics"),
                                        QStringLiteral("packageVersion"),
                                        QStringLiteral("phase"),
                                        QStringLiteral("routeTemplate"),
                                        QStringLiteral("timestamp")}));
}

SafeEvent eventFor(const qint64 timestamp)
{
    const SafeEventResult result = SafeEvent::create(
        timestamp, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
        SafeEventPhase::Worker, SafeEventCode::Completed, 12,
        QStringLiteral("/orders/:id"),
        {{SafeMetric::HeartbeatCount, static_cast<double>(timestamp % 100)}});
    Q_ASSERT(result.hasValue());
    return result.value();
}

void SafeEventTest::recorderIsAsynchronousBoundedAndRotates()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    EventRecorder recorder({directory.path(), QStringLiteral("events.jsonl"),
                            512, 2, 4});
    QVERIFY2(recorder.isValid(), qPrintable(recorder.stableErrorCode()));

    QElapsedTimer elapsed;
    elapsed.start();
    int accepted = 0;
    int full = 0;
    for (int index = 0; index < 20'000; ++index) {
        const EventRecordStatus status = recorder.record(eventFor(index + 1));
        if (status == EventRecordStatus::Accepted) ++accepted;
        if (status == EventRecordStatus::QueueFull) ++full;
    }
    QVERIFY(elapsed.elapsed() < 2'000);
    QVERIFY(accepted > 0);
    QVERIFY(full > 0);
    QVERIFY(recorder.flush(10'000));
    QCOMPARE(recorder.lastError(), EventRecorderError::None);
    QCOMPARE(recorder.queuedEventCount(), qsizetype(0));

    const QStringList files = QDir(directory.path()).entryList(
        {QStringLiteral("events.jsonl*")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 2);
    QVERIFY(files.size() <= 3);
    for (const QString &name : files) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.size() <= 512);
        const QByteArray bytes = file.readAll();
        QVERIFY(!bytes.contains("secret"));
        const QList<QByteArray> lines = bytes.split('\n');
        for (const QByteArray &line : lines) {
            if (!line.isEmpty()) QVERIFY(QJsonDocument::fromJson(line).isObject());
        }
    }
}

void SafeEventTest::recorderIsThreadSafeAndReportsTypedWriteFailures()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    EventRecorder recorder({directory.path(), QStringLiteral("threaded.jsonl"),
                            1'048'576, 1, 256});
    QVERIFY(recorder.isValid());
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 8; ++thread) {
        writers.emplace_back([&recorder, thread] {
            for (int index = 0; index < 100; ++index) {
                (void)recorder.record(eventFor(10'000 + thread * 100 + index));
            }
        });
    }
    for (std::thread &writer : writers) writer.join();
    QVERIFY(recorder.flush(10'000));
    QCOMPARE(recorder.lastError(), EventRecorderError::None);

    const QString moved = directory.path() + QStringLiteral("-moved");
    QVERIFY(QDir().rename(directory.path(), moved));
    QCOMPARE(recorder.record(eventFor(99'999)), EventRecordStatus::Accepted);
    QVERIFY(!recorder.flush(10'000));
    QCOMPARE(recorder.lastError(), EventRecorderError::OpenFailed);
    QCOMPARE(recorder.stableErrorCode(), QStringLiteral("telemetry.file_open_failed"));
    QVERIFY(!recorder.stableErrorCode().contains(QStringLiteral("secret")));
    QCOMPARE(recorder.record(eventFor(100'000)), EventRecordStatus::RecorderFailed);
    QVERIFY(QDir().rename(moved, directory.path()));
}

void SafeEventTest::rejectsSecretsAndUserControlledTextAtConstruction()
{
    const QStringList forbidden{
        QStringLiteral("Authorization: Bearer top-secret-token"),
        QStringLiteral("Cookie: session=top-secret-cookie"),
        QStringLiteral("password=hunter2"),
        QStringLiteral("customer Alice Example said hello"),
        QStringLiteral("C:/private/file.txt contents top-secret-file"),
        QStringLiteral("/orders?token=top-secret-query"),
        QStringLiteral("../private/secret.txt")};

    for (const QString &value : forbidden) {
        const SafeEventResult app = SafeEvent::create(
            1, value, QStringLiteral("1.0.0"), SafeEventPhase::Install,
            SafeEventCode::Rejected, 0, QStringLiteral("/"), {});
        QVERIFY2(!app.hasValue(), qPrintable(value));
        QCOMPARE(app.stableError, QStringLiteral("telemetry.invalid_app_id"));

        const SafeEventResult version = SafeEvent::create(
            1, QStringLiteral("company.pilot"), value,
            SafeEventPhase::Install, SafeEventCode::Rejected, 0,
            QStringLiteral("/"), {});
        QVERIFY2(!version.hasValue(), qPrintable(value));

        const SafeEventResult route = SafeEvent::create(
            1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
            SafeEventPhase::Install, SafeEventCode::Rejected, 0, value, {});
        QVERIFY2(!route.hasValue(), qPrintable(value));
    }
}

void SafeEventTest::validatesBoundsUnicodeRoutesAndMetrics()
{
    const auto make = [](const qint64 timestamp,
                         const QString &appId,
                         const QString &version,
                         const qint64 duration,
                         const QString &route,
                         const SafeMetrics &metrics) {
        return SafeEvent::create(timestamp, appId, version,
                                 SafeEventPhase::Worker,
                                 SafeEventCode::Failed,
                                 duration, route, metrics);
    };

    QVERIFY(!make(-1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  0, QStringLiteral("/"), {}).hasValue());
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("01.0.0"),
                  0, QStringLiteral("/"), {}).hasValue());
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  -1, QStringLiteral("/"), {}).hasValue());
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  0, QStringLiteral("/orders/1234"), {}).hasValue());
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  0, QStringLiteral("/orders/:id?x=1"), {}).hasValue());
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  0, QStringLiteral("/orders/:id"),
                  {{SafeMetric::HeartbeatCount,
                    std::numeric_limits<double>::infinity()}}).hasValue());
    SafeMetrics tooMany;
    for (int index = 0; index < 9; ++index) {
        tooMany.push_back({SafeMetric::HeartbeatCount,
                           static_cast<double>(index)});
    }
    QVERIFY(!make(1, QStringLiteral("company.pilot"), QStringLiteral("1.0.0"),
                  0, QStringLiteral("/"), tooMany).hasValue());
}

QTEST_APPLESS_MAIN(SafeEventTest)

#include "tst_safe_event.moc"
