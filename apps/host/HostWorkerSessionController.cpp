#include "HostWorkerSessionController.h"

#include "HostWorkerSessionIo.h"
#include "IpcSession.h"
#include "MainWindow.h"

#include <QJsonObject>
#include <QScopedValueRollback>
#include <QThread>

#include <utility>

namespace {
QString routeFromAppUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("app")
        || url.host() != QStringLiteral("pilot") || !url.fragment().isEmpty()) {
        return {};
    }
    QString route = url.path(QUrl::FullyEncoded);
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) route += u'?' + query;
    return route.startsWith(u'/') && !route.startsWith(QStringLiteral("//"))
        ? route : QString{};
}
} // namespace

HostWorkerSessionController::HostWorkerSessionController(MainWindow *window,
                                                         QObject *parent)
    : QObject(parent), window_(window)
{
    if (window_ != nullptr) {
        connect(window_, &MainWindow::workerRouteRequested, this,
                &HostWorkerSessionController::handleHostWorkerRoute,
                Qt::DirectConnection);
    }
}

HostWorkerSessionController::~HostWorkerSessionController()
{
    stopIoThreadForDestruction();
}

bool HostWorkerSessionController::attach(std::unique_ptr<IpcSession> session)
{
    if (window_ == nullptr || session == nullptr || !session->isAuthenticated()
        || session->isClosed()
        || session->appIdentity().isEmpty()) {
        lastErrorCode_ = QStringLiteral("host.worker_session.invalid_attachment");
        return false;
    }
    if (io_ != nullptr || ioThread_ != nullptr) {
        pendingSession_ = std::move(session);
        cleanupFinalState_ = HostWorkerSessionState::Detached;
        state_ = HostWorkerSessionState::ShuttingDown;
        pendingRouteLoads_.clear();
        outbound_.clear();
        activeCommand_.reset();
        requestIoStop();
        return true;
    }
    return startSession(std::move(session));
}

bool HostWorkerSessionController::startSession(std::unique_ptr<IpcSession> session)
{
    if (session == nullptr || io_ != nullptr || ioThread_ != nullptr) return false;
    ++generation_;
    appIdentity_ = session->appIdentity();
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    lastErrorCode_.clear();
    ioThread_ = new QThread;
    ioThread_->setObjectName(QStringLiteral("host-worker-session-io"));
    io_ = new HostWorkerSessionIo(std::move(session), generation_);
    ioIdentity_ = io_;
    HostWorkerSessionIo *const attachedIo = io_;
    QThread *const attachedThread = ioThread_;
    const quint64 attachedGeneration = generation_;
    io_->moveToThread(ioThread_);
    connect(ioThread_, &QThread::started, io_, &HostWorkerSessionIo::start);
    connect(ioThread_, &QThread::finished, io_, &QObject::deleteLater);
    connect(io_, &QObject::destroyed, this,
            [this, attachedIo, attachedThread, attachedGeneration] {
                if (generation_ == attachedGeneration && io_ == attachedIo
                    && ioIdentity_ == attachedIo && ioThread_ == attachedThread)
                    io_ = nullptr;
            }, Qt::QueuedConnection);
    connect(ioThread_, &QThread::finished, this,
            [this, attachedIo, attachedThread, attachedGeneration] {
                handleIoThreadFinished(attachedIo, attachedThread,
                                       attachedGeneration);
            }, Qt::QueuedConnection);
    connect(io_, &HostWorkerSessionIo::navigationRequested, this,
            &HostWorkerSessionController::handleNavigationRequest, Qt::QueuedConnection);
    connect(io_, &HostWorkerSessionIo::routeLoadResponse, this,
            &HostWorkerSessionController::handleRouteLoadResponse, Qt::QueuedConnection);
    connect(io_, &HostWorkerSessionIo::commandFinished, this,
            &HostWorkerSessionController::handleCommandFinished, Qt::QueuedConnection);
    connect(io_, &HostWorkerSessionIo::sessionFailed, this,
            [this](const quint64 generation, const QString &errorCode) {
                if (generation == generation_) failClosed(errorCode);
            }, Qt::QueuedConnection);
    connect(io_, &HostWorkerSessionIo::shutdownFinished, this,
            [this](const quint64 generation) {
                if (generation != generation_
                    || state_ != HostWorkerSessionState::ShuttingDown) return;
                pendingRouteLoads_.clear();
                outbound_.clear();
                activeCommand_.reset();
                appIdentity_.clear();
                cleanupFinalState_ = HostWorkerSessionState::Detached;
                requestIoStop();
            }, Qt::QueuedConnection);
    cleanupFinalState_ = HostWorkerSessionState::Failed;
    state_ = HostWorkerSessionState::Running;
    ioThread_->start();
    return true;
}

bool HostWorkerSessionController::shutdown(const QString &reason)
{
    if (state_ != HostWorkerSessionState::Running || io_ == nullptr || reason.isEmpty())
        return false;
    state_ = HostWorkerSessionState::ShuttingDown;
    cleanupFinalState_ = HostWorkerSessionState::Detached;
    outbound_.clear();
    activeCommand_.reset();
    pendingRouteLoads_.clear();
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation, reason] {
        io->beginShutdown(generation, reason);
    }, Qt::QueuedConnection);
    return true;
}

HostWorkerSessionState HostWorkerSessionController::state() const noexcept { return state_; }
QString HostWorkerSessionController::lastErrorCode() const { return lastErrorCode_; }
qsizetype HostWorkerSessionController::pendingRouteLoadCount() const noexcept
{
    return pendingRouteLoads_.size();
}

bool HostWorkerSessionController::hasIoThread() const noexcept
{
    return ioThread_ != nullptr;
}

void HostWorkerSessionController::handleHostWorkerRoute(
    const QString &packageId, const QString &entryPoint,
    const QVariantMap &parameters, const QUrl &appUrl)
{
    Q_UNUSED(entryPoint)
    Q_UNUSED(parameters)
    if (suppressHostRoute_ || state_ != HostWorkerSessionState::Running
        || packageId != appIdentity_) return;
    const QString route = routeFromAppUrl(appUrl);
    if (!route.isEmpty() && !enqueueRouteLoad(route))
        failClosed(QStringLiteral("host.worker_session.outbound_queue_full"));
}

void HostWorkerSessionController::handleNavigationRequest(
    const quint64 generation, const QString &requestId, const QString &route)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) return;
    if (!pendingRouteLoads_.isEmpty()) {
        const auto response = ProtocolMessage::errorResponse(
            requestId, QStringLiteral("navigation.busy"),
            QStringLiteral("A route load is already pending."));
        if (!response.has_value() || !enqueueMessage(*response, true))
            failClosed(QStringLiteral("host.worker_session.response_queue_failed"));
        return;
    }
    bool navigated = false;
    {
        QScopedValueRollback suppression(suppressHostRoute_, true);
        navigated = window_ != nullptr && window_->navigateFromWorker(appIdentity_, route);
    }
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) return;
    if (!navigated) {
        const auto response = ProtocolMessage::errorResponse(
            requestId, QStringLiteral("navigation.denied"),
            QStringLiteral("The requested route is not assigned to this worker."));
        if (!response.has_value() || !enqueueMessage(*response, true))
            failClosed(QStringLiteral("host.worker_session.response_queue_failed"));
        return;
    }
    const auto response = ProtocolMessage::successResponse(
        requestId, QJsonObject{{QStringLiteral("route"), route}});
    if (!response.has_value() || !enqueueMessage(*response) || !enqueueRouteLoad(route)) {
        failClosed(QStringLiteral("host.worker_session.outbound_queue_full"));
        return;
    }
    if (!outbound_.isEmpty()) outbound_.back().resumePollingAfter = true;
    else if (activeCommand_.has_value()) activeCommand_->resumePollingAfter = true;
}

void HostWorkerSessionController::handleRouteLoadResponse(
    const quint64 generation, const QString &requestId, const QJsonObject &payload)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running) return;
    const auto iterator = pendingRouteLoads_.find(requestId);
    if (iterator == pendingRouteLoads_.end()) {
        failClosed(QStringLiteral("host.worker_session.unexpected_response"));
        return;
    }
    const QString route = iterator.value();
    const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
    if (!payload.value(QStringLiteral("ok")).toBool(false)
        || result.value(QStringLiteral("route")).toString() != route) {
        failClosed(QStringLiteral("host.worker_session.route_load_rejected"));
        return;
    }
    pendingRouteLoads_.erase(iterator);
    emit routeLoadAcknowledged(route);
    if (generation == generation_ && state_ == HostWorkerSessionState::Running)
        resumeIoPolling();
}

void HostWorkerSessionController::handleCommandFinished(
    const quint64 generation, const quint64 commandId, const bool success,
    const QString &errorCode)
{
    if (generation != generation_ || state_ != HostWorkerSessionState::Running
        || !activeCommand_.has_value() || activeCommand_->id != commandId) return;
    const bool resume = activeCommand_->resumePollingAfter;
    activeCommand_.reset();
    if (!success) {
        failClosed(errorCode.isEmpty() ? QStringLiteral("host.worker_session.send_failed")
                                        : errorCode);
        return;
    }
    pumpOutbound();
    if (resume && generation == generation_ && state_ == HostWorkerSessionState::Running)
        resumeIoPolling();
}

bool HostWorkerSessionController::enqueueMessage(const ProtocolMessage &message,
                                                 const bool resumePollingAfter)
{
    if (outbound_.size() + (activeCommand_.has_value() ? 1 : 0)
        >= maximumQueuedCommands) return false;
    OutboundCommand command;
    command.id = ++nextCommandId_;
    command.message = message;
    command.resumePollingAfter = resumePollingAfter;
    outbound_.enqueue(std::move(command));
    pumpOutbound();
    return true;
}

bool HostWorkerSessionController::enqueueRouteLoad(const QString &route)
{
    if (route.isEmpty() || pendingRouteLoads_.size() >= maximumQueuedCommands
        || outbound_.size() + (activeCommand_.has_value() ? 1 : 0)
               >= maximumQueuedCommands) return false;
    const QString requestId = QStringLiteral("host-route-%1").arg(++nextRouteLoadId_);
    const auto message = ProtocolMessage::routeLoad(requestId, route);
    if (!message.has_value()) return false;
    pendingRouteLoads_.insert(requestId, route);
    OutboundCommand command;
    command.id = ++nextCommandId_;
    command.message = *message;
    command.route = route;
    command.trackedRouteLoad = true;
    outbound_.enqueue(std::move(command));
    pumpOutbound();
    return true;
}

void HostWorkerSessionController::pumpOutbound()
{
    if (state_ != HostWorkerSessionState::Running || io_ == nullptr
        || activeCommand_.has_value() || outbound_.isEmpty()) return;
    activeCommand_ = outbound_.dequeue();
    const OutboundCommand command = *activeCommand_;
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation, command] {
        if (command.message.has_value())
            io->sendMessage(generation, command.id, *command.message,
                            command.trackedRouteLoad, command.route);
    }, Qt::QueuedConnection);
}

void HostWorkerSessionController::resumeIoPolling()
{
    if (io_ == nullptr) return;
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = io_;
    QMetaObject::invokeMethod(io, [io, generation] { io->resumePolling(generation); },
                              Qt::QueuedConnection);
}

void HostWorkerSessionController::failClosed(const QString &errorCode)
{
    if (state_ == HostWorkerSessionState::Failed
        || state_ == HostWorkerSessionState::Detached) return;
    state_ = HostWorkerSessionState::Failed;
    lastErrorCode_ = errorCode.isEmpty() ? QStringLiteral("host.worker_session.failed")
                                         : errorCode;
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    appIdentity_.clear();
    cleanupFinalState_ = HostWorkerSessionState::Failed;
    requestIoStop();
    emit failed(lastErrorCode_);
}

void HostWorkerSessionController::requestIoStop()
{
    if (ioIdentity_ == nullptr || ioThread_ == nullptr) return;
    const quint64 generation = generation_;
    HostWorkerSessionIo *const io = ioIdentity_;
    (void)QMetaObject::invokeMethod(
        io, [io, generation] { io->abort(generation); }, Qt::QueuedConnection);
}

void HostWorkerSessionController::handleIoThreadFinished(
    HostWorkerSessionIo *const oldIo, QThread *const oldThread,
    const quint64 generation)
{
    if (generation_ != generation || ioIdentity_ != oldIo
        || ioThread_ != oldThread) {
        oldThread->deleteLater();
        return;
    }

    io_ = nullptr;
    ioIdentity_ = nullptr;
    ioThread_ = nullptr;
    delete oldThread;
    pendingRouteLoads_.clear();
    outbound_.clear();
    activeCommand_.reset();
    appIdentity_.clear();

    if (pendingSession_ != nullptr) {
        std::unique_ptr<IpcSession> replacement = std::move(pendingSession_);
        state_ = HostWorkerSessionState::Detached;
        if (!startSession(std::move(replacement))) {
            state_ = HostWorkerSessionState::Failed;
            lastErrorCode_ = QStringLiteral("host.worker_session.reattach_failed");
            emit failed(lastErrorCode_);
        }
        return;
    }

    state_ = cleanupFinalState_;
}

void HostWorkerSessionController::stopIoThreadForDestruction()
{
    pendingSession_.reset();
    HostWorkerSessionIo *const oldIo = ioIdentity_;
    QThread *const oldThread = ioThread_;
    const quint64 oldGeneration = generation_;
    if (oldThread == nullptr) return;

    disconnect(oldThread, nullptr, this, nullptr);
    if (oldIo != nullptr) {
        disconnect(oldIo, nullptr, this, nullptr);
        (void)QMetaObject::invokeMethod(
            oldIo, [oldIo, oldGeneration] { oldIo->abort(oldGeneration); },
            Qt::QueuedConnection);
    } else {
        oldThread->quit();
    }

    if (oldThread->wait(50)) {
        delete oldThread;
    } else {
        connect(oldThread, &QThread::finished, oldThread,
                &QObject::deleteLater, Qt::QueuedConnection);
    }
    ioThread_ = nullptr;
    io_ = nullptr;
    ioIdentity_ = nullptr;
}
