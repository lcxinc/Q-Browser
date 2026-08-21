#include "FrameCodec.h"
#include "IpcSession.h"
#include "ProtocolMessage.h"
#include "WorkerTestEnvironment.h"

#include <QtEndian>
#include <QTest>

class WorkerApiSurfaceTest final : public QObject
{
    Q_OBJECT
private slots:
    void realWorkerExposesOnlyTypedRuntimeRequests();
    void rejectsOversizedMalformedAndReplayedFrames();
};

void WorkerApiSurfaceTest::realWorkerExposesOnlyTypedRuntimeRequests()
{
    const QByteArray qml = QByteArrayLiteral(R"QML(import QtQuick
Item {
    Component.onCompleted: Runtime.invoke("notDeclared", "escape", { value: 1 })
}
)QML");
    WorkerTestEnvironment environment(qml);
    QVERIFY2(environment.isValid(), qPrintable(environment.error()));
    auto launch = environment.launch(QStringLiteral("surface-nonce"),
                                     QStringLiteral("surface-nonce"), 1000);
    QVERIFY2(launch.has_value(), qPrintable(environment.error()));
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Handshake).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::SurfaceReady).status,
             SessionStatus::MessageReady);
    QCOMPARE(receiveUntil(launch->hostSession, ProtocolType::Ready).status,
             SessionStatus::MessageReady);
    const SessionReceiveResult request = receiveUntil(
        launch->hostSession, ProtocolType::Request, 10'000);
    QCOMPARE(request.status, SessionStatus::MessageReady);
    QVERIFY(request.message.has_value());
    QCOMPARE(request.message->payload().value(QStringLiteral("capability")).toString(),
             QStringLiteral("notDeclared"));
    QCOMPARE(request.message->payload().value(QStringLiteral("operation")).toString(),
             QStringLiteral("escape"));
    const auto rejection = ProtocolMessage::errorResponse(
        request.message->requestId(), QStringLiteral("capability.denied"),
        QStringLiteral("The capability is not granted."));
    QVERIFY(rejection.has_value());
    QVERIFY(launch->hostSession.send(*rejection, 5000));
    const auto shutdown = ProtocolMessage::shutdown(QStringLiteral("security.complete"));
    QVERIFY(shutdown.has_value());
    QVERIFY(launch->hostSession.send(*shutdown, 5000));
    QVERIFY(launch->process.waitForFinished(5000));
    const auto closed = launch->process.close();
    QVERIFY2(closed.value.has_value(), qPrintable(closed.errorCode));
}

void WorkerApiSurfaceTest::rejectsOversizedMalformedAndReplayedFrames()
{
    FrameCodec oversizedCodec;
    QByteArray oversizedHeader(4, '\0');
    qToBigEndian(FrameCodec::maximumPayloadBytes() + 1U,
                 reinterpret_cast<uchar *>(oversizedHeader.data()));
    const FrameFeedResult oversized = oversizedCodec.feed(oversizedHeader);
    QCOMPARE(oversized.status, FrameStatus::Failed);
    QCOMPARE(oversized.error, FrameError::PayloadTooLarge);

    FrameCodec malformedCodec;
    QByteArray malformed(4, '\0');
    qToBigEndian<quint32>(1U, reinterpret_cast<uchar *>(malformed.data()));
    malformed.append('{');
    const FrameFeedResult malformedResult = malformedCodec.feed(malformed);
    QCOMPARE(malformedResult.status, FrameStatus::Failed);
    QCOMPARE(malformedResult.error, FrameError::InvalidJson);

#ifdef Q_OS_WIN
    WinPipePair pair = WinPipeTransport::createHostPair();
    QVERIFY(pair.isValid());
    IpcSession host(pair.takeHost(), IpcRole::Host,
                    HostLaunchContext{QStringLiteral("replay"),
                                      QStringLiteral("company.security")});
    IpcSession worker(WinPipeTransport::adoptWorkerEnds(pair.takeWorkerEnds()),
                      IpcRole::Worker);
    QVERIFY(worker.send(*ProtocolMessage::handshake(QStringLiteral("replay"))));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
    QVERIFY(worker.sendRequest(QStringLiteral("request-1"), QStringLiteral("storage"),
                               QStringLiteral("get"), {}, 1000));
    QCOMPARE(host.receive(1000).status, SessionStatus::MessageReady);
    const auto response = ProtocolMessage::successResponse(QStringLiteral("request-1"), {});
    QVERIFY(response.has_value());
    QVERIFY(host.send(*response));
    QCOMPARE(worker.receive(1000).status, SessionStatus::MessageReady);
    QVERIFY(host.send(*response));
    QCOMPARE(worker.receive(1000).status, SessionStatus::Failed);
    QCOMPARE(worker.lastErrorCode(), QStringLiteral("ipc.session.unknown_response"));
#endif
}

QTEST_MAIN(WorkerApiSurfaceTest)
#include "tst_worker_api_surface.moc"
