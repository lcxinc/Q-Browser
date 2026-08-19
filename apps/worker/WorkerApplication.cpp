#include "WorkerApplication.h"

#include "WorkerWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <limits>

namespace {

std::optional<QString> singleValue(const QStringList &arguments, const QString &name)
{
    const qsizetype index = arguments.indexOf(name);
    if (index <= 0 || index + 1 >= arguments.size()
        || arguments.lastIndexOf(name) != index) {
        return std::nullopt;
    }
    return arguments.at(index + 1);
}

std::optional<HANDLE> inheritedHandle(const QString &text)
{
    bool converted = false;
    const qulonglong raw = text.toULongLong(&converted, 10);
    if (!converted || raw == 0 || QString::number(raw) != text
        || raw > std::numeric_limits<quintptr>::max()) {
        return std::nullopt;
    }
    return reinterpret_cast<HANDLE>(static_cast<quintptr>(raw));
}

} // namespace

WorkerApplication::WorkerApplication(QObject *parent)
    : QObject(parent), runtimeFacade_(this)
{
    pollTimer_.setInterval(5);
    pollTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &WorkerApplication::pollIpc);
    connect(&heartbeatTimer_, &QTimer::timeout, this, &WorkerApplication::sendHeartbeat);
    connect(&runtimeFacade_, &RuntimeFacade::capabilityRequested,
            this, &WorkerApplication::sendCapabilityRequest);
}

WorkerApplication::~WorkerApplication() = default;

bool WorkerApplication::start(const QStringList &arguments)
{
    const auto parsed = parseArguments(arguments);
    if (!parsed.has_value()) {
        return false;
    }
    launch_ = *parsed;
    auto transport = WinPipeTransport::adoptInheritedHandles(launch_.readHandle,
                                                              launch_.writeHandle);
    if (!transport.has_value()) {
        return false;
    }
    session_ = std::make_unique<IpcSession>(std::move(*transport), IpcRole::Worker);
    const auto handshake = ProtocolMessage::handshake(launch_.nonce);
    if (!handshake.has_value() || !session_->send(*handshake, 5000)) {
        session_.reset();
        return false;
    }
    heartbeatTimer_.setInterval(launch_.heartbeatMs);
    pollTimer_.start();
    return true;
}

std::optional<WorkerApplication::LaunchArguments> WorkerApplication::parseArguments(
    const QStringList &arguments)
{
    const auto readText = singleValue(arguments, QStringLiteral("--qbrowser-ipc-read-handle"));
    const auto writeText = singleValue(arguments, QStringLiteral("--qbrowser-ipc-write-handle"));
    const auto nonce = singleValue(arguments, QStringLiteral("--qbrowser-nonce"));
    const auto package = singleValue(arguments, QStringLiteral("--qbrowser-package"));
    const auto entry = singleValue(arguments, QStringLiteral("--qbrowser-entry"));
    const auto apiOriginText = singleValue(arguments, QStringLiteral("--qbrowser-api-origin"));
    const auto heartbeat = singleValue(arguments, QStringLiteral("--qbrowser-heartbeat-ms"));
    if (!readText || !writeText || !nonce || !package || !entry || !apiOriginText
        || !heartbeat) {
        return std::nullopt;
    }
    const auto read = inheritedHandle(*readText);
    const auto write = inheritedHandle(*writeText);
    bool heartbeatOk = false;
    const int heartbeatMs = heartbeat->toInt(&heartbeatOk);
    const QFileInfo packageInfo(*package);
    const QUrl apiOrigin(*apiOriginText, QUrl::StrictMode);
    const QString originPath = apiOrigin.path(QUrl::FullyDecoded);
    if (!read || !write || *read == *write || nonce->isEmpty()
        || nonce->size() > 256 || !packageInfo.isAbsolute() || !packageInfo.isDir()
        || packageInfo.isSymLink() || entry->isEmpty() || entry->size() > 2048
        || !apiOrigin.isValid() || apiOrigin.isRelative()
        || (apiOrigin.scheme() != QStringLiteral("http")
            && apiOrigin.scheme() != QStringLiteral("https"))
        || apiOrigin.host().isEmpty() || !apiOrigin.userInfo().isEmpty()
        || apiOrigin.hasQuery() || apiOrigin.hasFragment()
        || (!originPath.isEmpty() && originPath != QStringLiteral("/"))
        || apiOrigin.port(apiOrigin.scheme() == QStringLiteral("https") ? 443 : 80) <= 0
        || !heartbeatOk || heartbeatMs < 20 || heartbeatMs > 60'000) {
        return std::nullopt;
    }
    for (const QChar character : *nonce) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) {
            return std::nullopt;
        }
    }
    QString canonicalOrigin = apiOrigin.adjusted(QUrl::RemovePath | QUrl::RemoveQuery
                                                  | QUrl::RemoveFragment)
                                  .toString(QUrl::FullyEncoded);
    if (canonicalOrigin.endsWith(u'/')) canonicalOrigin.chop(1);
    return LaunchArguments{*read, *write, *nonce,
                           packageInfo.canonicalFilePath(), *entry,
                           canonicalOrigin,
                           heartbeatMs};
}

void WorkerApplication::pollIpc()
{
    if (session_ == nullptr || state_ == State::Exiting) return;
    const SessionReceiveResult received = session_->poll(0);
    if (received.status == SessionStatus::TimedOut) return;
    if (received.status != SessionStatus::MessageReady || !received.message.has_value()) {
        failClosed();
        return;
    }
    handleMessage(*received.message);
}

bool WorkerApplication::finishAuthentication()
{
    runtimeFacade_.assignAppIdentity(session_->appIdentity());
    runtimeFacade_.assignApiOrigin(launch_.apiOrigin);
    window_ = std::make_unique<WorkerWindow>();
    state_ = State::Loading;
    if (!window_->load(launch_.packageDirectory, launch_.entryPoint, &runtimeFacade_)) {
        return false;
    }
    const auto surface = ProtocolMessage::surfaceReady(
        QString::number(static_cast<qulonglong>(window_->windowId())));
    if (!surface.has_value() || !session_->send(*surface, 5000)
        || !session_->send(ProtocolMessage::ready(), 5000)) {
        return false;
    }
    state_ = State::Ready;
    if (!flushPendingCapabilities()) {
        return false;
    }
    heartbeatTimer_.start();
    return true;
}

bool WorkerApplication::flushPendingCapabilities()
{
    while (auto request = pendingCapabilities_.takeNext()) {
        if (!session_->sendRequest(request->requestId,
                                   request->capability,
                                   request->operation,
                                   request->payload,
                                   5000)) {
            pendingCapabilities_.clear();
            return false;
        }
    }
    return true;
}

void WorkerApplication::handleMessage(const ProtocolMessage &message)
{
    if (state_ == State::Authenticating) {
        if (message.type() != ProtocolType::HandshakeAck || !session_->isAuthenticated()
            || !finishAuthentication()) {
            failClosed();
        }
        return;
    }
    switch (message.type()) {
    case ProtocolType::RouteLoad: {
        const QString route = message.payload().value(QStringLiteral("route")).toString();
        runtimeFacade_.loadRoute(route);
        const auto response = ProtocolMessage::successResponse(
            message.requestId(), QJsonObject{{QStringLiteral("route"), route}});
        if (!response.has_value() || !session_->send(*response, 5000)) failClosed();
        break;
    }
    case ProtocolType::Response:
        runtimeFacade_.complete(message.requestId(), message.payload());
        break;
    case ProtocolType::Heartbeat:
        break;
    case ProtocolType::Shutdown: {
        state_ = State::Exiting;
        pollTimer_.stop();
        heartbeatTimer_.stop();
        const auto acknowledgement = ProtocolMessage::shutdown(QStringLiteral("worker.ack"));
        if (acknowledgement.has_value()) (void)session_->send(*acknowledgement, 5000);
        QCoreApplication::exit(0);
        break;
    }
    default:
        failClosed();
        break;
    }
}

void WorkerApplication::sendHeartbeat()
{
    if (session_ != nullptr && state_ == State::Ready
        && !session_->send(ProtocolMessage::heartbeat(), 5000)) {
        failClosed();
    }
}

void WorkerApplication::sendCapabilityRequest(const QString &requestId,
                                              const QString &capability,
                                              const QString &operation,
                                              const QJsonObject &payload)
{
    if (session_ == nullptr) {
        failClosed();
        return;
    }
    if (state_ == State::Loading) {
        if (pendingCapabilities_.enqueue({requestId, capability, operation, payload})
            != PendingCapabilityPushResult::Accepted) {
            failClosed();
        }
        return;
    }
    if (state_ != State::Ready
        || !session_->sendRequest(requestId, capability, operation, payload, 5000)) {
        failClosed();
    }
}

void WorkerApplication::failClosed(const int exitCode)
{
    if (state_ == State::Exiting) return;
    state_ = State::Exiting;
    pendingCapabilities_.clear();
    pollTimer_.stop();
    heartbeatTimer_.stop();
    if (session_ != nullptr) session_->close();
    QCoreApplication::exit(exitCode);
}
