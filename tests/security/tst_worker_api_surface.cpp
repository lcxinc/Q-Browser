#include "FrameCodec.h"
#include "IpcSession.h"
#include "ProtocolMessage.h"
#include "RuntimeFacade.h"
#include "WorkerTestEnvironment.h"

#include <QtEndian>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

class WorkerApiSurfaceTest final : public QObject
{
    Q_OBJECT
private slots:
    void realWorkerExposesOnlyTypedRuntimeRequests();
    void rejectsOversizedMalformedAndReplayedFrames();
    void runtimeFacadeAddsOnlyBoundedPageMetadata();
    void runtimeFacadeExposesReadOnlyActiveState();
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

void WorkerApiSurfaceTest::runtimeFacadeAddsOnlyBoundedPageMetadata()
{
    using ExactSetter = bool (RuntimeFacade::*)(const QString &, const QString &);
    const ExactSetter exactSetter = &RuntimeFacade::setPageMetadata;
    QVERIFY(exactSetter != nullptr);

    RuntimeFacade facade;
    QSignalSpy metadataSpy(&facade, &RuntimeFacade::pageMetadataChanged);
    QString unsafe = QStringLiteral("Orders");
    unsafe.append(u'\n');
    unsafe.append(QChar(0x202e));
    unsafe.append(QChar(0x206f));
    QVERIFY(facade.setPageMetadata(unsafe, QStringLiteral("loading")));
    QCOMPARE(metadataSpy.count(), 1);
    QCOMPARE(metadataSpy.at(0).at(0).toString(), QStringLiteral("Orders"));
    QCOMPARE(metadataSpy.at(0).at(1).toString(), QStringLiteral("loading"));

    QVERIFY(facade.setPageMetadata(QString(257, u'x')));
    QCOMPARE(metadataSpy.count(), 2);
    QCOMPARE(metadataSpy.at(1).at(0).toString(), QString(256, u'x'));
    QCOMPARE(metadataSpy.at(1).at(1).toString(), QString());

    QString loneHighSurrogate;
    loneHighSurrogate.append(QChar(0xd800));
    QVERIFY(!facade.setPageMetadata(loneHighSurrogate));
    QVERIFY(!facade.setPageMetadata(QStringLiteral("<b>Orders</b>")));
    QVERIFY(!facade.setPageMetadata(QStringLiteral("Orders"),
                                    QStringLiteral("not ready")));
    for (const QString &status : {QStringLiteral("admin"),
                                  QStringLiteral("trusted"),
                                  QStringLiteral("loading-1"),
                                  QStringLiteral("READY"),
                                  QStringLiteral("Loading")}) {
        QVERIFY(!facade.setPageMetadata(QStringLiteral("Orders"), status));
    }
    QVERIFY(!facade.setPageMetadata(QString(4097, u'x')));
    QCOMPARE(metadataSpy.count(), 2);

    QSet<QByteArray> invokables;
    QSet<QByteArray> signalSignatures;
    const QMetaObject &metaObject = RuntimeFacade::staticMetaObject;
    for (int index = metaObject.methodOffset(); index < metaObject.methodCount(); ++index) {
        const QMetaMethod method = metaObject.method(index);
        if (method.methodType() == QMetaMethod::Signal) {
            signalSignatures.insert(method.methodSignature());
        } else if (method.methodType() == QMetaMethod::Method
                   && method.access() == QMetaMethod::Public) {
            invokables.insert(method.methodSignature());
        }
    }
    QCOMPARE(invokables,
             QSet<QByteArray>({QByteArrayLiteral("invoke(QString,QString,QJsonObject)"),
                               QByteArrayLiteral("invoke(QString,QString)"),
                               QByteArrayLiteral("navigate(QString)"),
                               QByteArrayLiteral("setPageMetadata(QString,QString)"),
                               QByteArrayLiteral("setPageMetadata(QString)")}));
    QCOMPARE(signalSignatures,
             QSet<QByteArray>({QByteArrayLiteral("appIdentityChanged()"),
                               QByteArrayLiteral("apiOriginChanged()"),
                               QByteArrayLiteral("routeChanged()"),
                               QByteArrayLiteral("capabilityRequested(QString,QString,QString,QJsonObject)"),
                               QByteArrayLiteral("capabilityFinished(QString,QJsonObject)"),
                               QByteArrayLiteral("navigationRequested(QString,QString)"),
                               QByteArrayLiteral("navigationFinished(QString,QJsonObject)"),
                               QByteArrayLiteral("activeChanged()"),
                               QByteArrayLiteral("pageMetadataChanged(QString,QString)")}));

    QSet<QByteArray> properties;
    for (int index = metaObject.propertyOffset(); index < metaObject.propertyCount(); ++index) {
        const QMetaProperty property = metaObject.property(index);
        properties.insert(property.name());
        QVERIFY(property.isReadable());
        QVERIFY(!property.isWritable());
    }
    QCOMPARE(properties,
             QSet<QByteArray>({QByteArrayLiteral("appIdentity"),
                               QByteArrayLiteral("apiOrigin"),
                               QByteArrayLiteral("route"),
                               QByteArrayLiteral("active")}));
    QVERIFY(metaObject.indexOfMethod("sendMessage(QJsonObject)") < 0);
    QVERIFY(metaObject.indexOfMethod("postMessage(QVariant)") < 0);
    QVERIFY(metaObject.indexOfProperty("securityContext") < 0);
}

void WorkerApiSurfaceTest::runtimeFacadeExposesReadOnlyActiveState()
{
    RuntimeFacade facade;
    const QMetaObject &metaObject = RuntimeFacade::staticMetaObject;
    const int activeIndex = metaObject.indexOfProperty("active");
    QVERIFY(activeIndex >= 0);
    const QMetaProperty active = metaObject.property(activeIndex);
    QVERIFY(active.isReadable());
    QVERIFY(!active.isWritable());
    QCOMPARE(active.notifySignal().methodSignature(), QByteArrayLiteral("activeChanged()"));
    QVERIFY(!facade.active());
}

QTEST_MAIN(WorkerApiSurfaceTest)
#include "tst_worker_api_surface.moc"
