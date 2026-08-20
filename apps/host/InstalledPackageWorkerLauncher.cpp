#include "InstalledPackageWorkerLauncher.h"

#include "InstalledPackageWorkerLauncherTestHooks.h"

#include "WindowsStableIo.h"
#include "WorkerSurface.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDirIterator>
#include <QPointer>
#include <QThread>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <utility>

namespace
{
struct ExpectedMessage final
{
    bool succeeded = false;
    QString windowHandle;
    QString stableError;
};

ExpectedMessage receiveExpected(IpcSession &session,
                                const ProtocolType expected,
                                const int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        const SessionReceiveResult received = session.receive(remaining);
        if (received.status != SessionStatus::MessageReady
            || !received.message.has_value()) {
            return {false, {}, received.errorCode.isEmpty()
                                   ? QStringLiteral("host.launch.ipc_failed")
                                   : received.errorCode};
        }
        if (received.message->type() == expected) {
            return {true,
                    received.message->payload()
                        .value(QStringLiteral("windowHandle"))
                        .toString(),
                    {}};
        }
        if (received.message->type() != ProtocolType::Heartbeat) {
            return {false, {}, QStringLiteral("host.launch.unexpected_message")};
        }
    }
    return {false, {}, QStringLiteral("host.launch.timeout")};
}

bool isStrictRelativeEntryPoint(const QString &entryPoint)
{
    if (entryPoint.isEmpty() || entryPoint.contains(u'\0')
        || entryPoint.contains(u'\\') || QDir::isAbsolutePath(entryPoint)
        || QDir::fromNativeSeparators(QDir::cleanPath(entryPoint)) != entryPoint) {
        return false;
    }
    const QStringList components = entryPoint.split(QLatin1Char('/'));
    return std::ranges::all_of(components, [](const QString &component) {
        return !component.isEmpty() && component != QLatin1String(".")
            && component != QLatin1String("..");
    });
}

bool isStableEntryPoint(const QString &packageDirectory,
                        const QString &entryPoint)
{
    if (!isStrictRelativeEntryPoint(entryPoint)) return false;
    const QString expected = QDir(packageDirectory).absoluteFilePath(entryPoint);
    const QFileInfo information(expected);
    const QString canonical = information.canonicalFilePath();
    const QString packagePrefix = QDir::toNativeSeparators(packageDirectory)
        + QDir::separator();
    if (!information.isFile() || information.isSymLink() || canonical.isEmpty()
        || canonical.compare(QDir::cleanPath(expected), Qt::CaseInsensitive) != 0
        || !QDir::toNativeSeparators(canonical).startsWith(
            packagePrefix, Qt::CaseInsensitive)) {
        return false;
    }
#ifdef Q_OS_WIN
    qbrowser_archive_detail::WindowsStableDirectoryTree tree;
    if (!tree.openRoot(packageDirectory)) return false;
    QString parent = packageDirectory;
    const QStringList components = entryPoint.split(QLatin1Char('/'));
    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        parent = QDir(parent).absoluteFilePath(components.at(index));
        if (!tree.addImmutableDirectory(parent)) return false;
    }
    qbrowser_archive_detail::WindowsStableFile file;
    return file.openSource(expected, tree) && file.isSameIdentityAt(expected);
#else
    return true;
#endif
}

bool matchesValidatedActivation(const UpdateLaunchRequest &request,
                                const InstallResult &validated)
{
    return validated.succeeded() && validated.activationBinding.has_value()
        && *validated.activationBinding == request.expectedActivation
        && validated.appId == request.appId
        && validated.version == request.packageVersion
        && validated.entryPoint == request.entryPoint
        && QFileInfo(validated.path).canonicalFilePath().compare(
               QFileInfo(request.packageDirectory).canonicalFilePath(),
               Qt::CaseInsensitive) == 0;
}

std::optional<QString> cryptographicNonce()
{
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<qsizetype>(bytes.size())).toHex());
}

struct OwnedWorkerTemp final
{
    QString path;
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tree;
};

std::optional<OwnedWorkerTemp> createOwnedWorkerTemp(const QString &sandboxRoot)
{
    using qbrowser_archive_detail::WindowsStableDirectoryTree;
    WindowsStableDirectoryTree parentTree;
    if (!parentTree.openRoot(sandboxRoot)) return std::nullopt;
    const QString workers = QDir(sandboxRoot).absoluteFilePath(
        QStringLiteral("workers"));
    const QFileInfo workersInfo(workers);
    if (workersInfo.exists()) {
        if (!workersInfo.isDir() || workersInfo.isSymLink()
            || !parentTree.addExistingDirectory(workers)) {
            return std::nullopt;
        }
    } else if (!parentTree.createAndHoldDirectory(workers)) {
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto nonce = cryptographicNonce();
        if (!nonce.has_value()) return std::nullopt;
        const QString path = QDir(workers).absoluteFilePath(
            QStringLiteral("launch-") + *nonce);
        if (!parentTree.createAndHoldDirectory(path)) continue;
        auto owned = std::make_shared<WindowsStableDirectoryTree>();
        if (!owned->adoptCreatedDirectoryRoot(parentTree, path)) {
            parentTree.cleanupCreatedDirectories();
            return std::nullopt;
        }
        return OwnedWorkerTemp{path, std::move(owned)};
    }
    return std::nullopt;
}

bool cleanupOwnedWorkerTemp(
    const QString &root,
    qbrowser_archive_detail::WindowsStableDirectoryTree &tree)
{
#ifdef Q_BROWSER_HOST_TESTING
    const auto hooks =
        qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
    if (hooks.failWorkerTempCleanup && hooks.failWorkerTempCleanup(root)) {
        return false;
    }
#endif
    QStringList files;
    qsizetype members = 0;
    QDirIterator iterator(
        root, QDir::AllEntries | QDir::Hidden | QDir::System
                  | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (++members > 4096 || info.isSymLink()) return false;
        if (info.isDir()) {
            if (!tree.addExistingDirectory(path)) return false;
        } else if (info.isFile()) {
            files.push_back(path);
        } else {
            return false;
        }
    }
    if (!tree.isStable()) return false;
    for (const QString &path : files) {
        qbrowser_archive_detail::WindowsStableFile file;
        if (!file.openForDelete(path, tree) || !file.deleteOwned()) return false;
    }
    return tree.deleteHeldTree();
}

}

struct InstalledPackageWorkerLauncher::LaunchRetirementContext final
    : std::enable_shared_from_this<LaunchRetirementContext>
{
    UpdateLaunchRequest request;
    quint64 serial = 0;
    std::unique_ptr<IpcSession> session;
    std::shared_ptr<SandboxProcess> process;
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree;
    QString tempDirectory;
    QString windowHandle;
    std::function<void(std::shared_ptr<LaunchRetirementContext>, bool,
                       const QString &)> retired;
    std::atomic_bool launching{true};
    std::atomic_bool retirementStarted{false};
    std::atomic_bool attached{false};
    std::atomic_bool fatalCleanupObserved{false};
    std::mutex resourceMutex;

    void requestTerminate() noexcept
    {
        std::lock_guard lock(resourceMutex);
        if (process != nullptr)
            process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    }

    void launchFinished() noexcept
    {
        launching.store(false, std::memory_order_release);
    }

    void retireAsync()
    {
        bool expected = false;
        if (!retirementStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) return;
        auto self = shared_from_this();
        QThread *const thread = QThread::create([self] {
            while (self->launching.load(std::memory_order_acquire)) {
                QThread::msleep(1);
            }
            std::shared_ptr<SandboxProcess> process;
            std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree>
                tempTree;
            QString tempDirectory;
            {
                std::lock_guard lock(self->resourceMutex);
                self->session.reset();
                process = self->process;
                tempTree = self->tempTree;
                tempDirectory = self->tempDirectory;
            }
            QString error;
            if (process != nullptr) {
                if (process->nativeProcessHandle() != nullptr) {
                    process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
                    if (!process->waitForFinished(5'000)) {
                        error = QStringLiteral("host.launch.process_wait_failed");
                    }
                }
                if (error.isEmpty()) {
                    const auto closed = process->close();
                    if (!closed.value.has_value()) {
                        error = closed.errorCode.isEmpty()
                            ? QStringLiteral("host.launch.process_cleanup_failed")
                            : closed.errorCode;
                    }
                }
            }
            if (error.isEmpty() && tempTree != nullptr
                && !cleanupOwnedWorkerTemp(tempDirectory, *tempTree)) {
                error = QStringLiteral("host.launch.temp_cleanup_failed");
            }
            const bool succeeded = error.isEmpty();
            if (succeeded) {
                std::lock_guard lock(self->resourceMutex);
                self->process.reset();
                self->tempTree.reset();
                self->tempDirectory.clear();
            }
            if (self->retired) self->retired(self, succeeded, error);
        });
        QObject::connect(thread, &QThread::finished,
                         thread, &QObject::deleteLater);
        thread->start();
    }

    void retryAsync()
    {
        retirementStarted.store(false, std::memory_order_release);
        retireAsync();
    }
};

struct InstalledPackageWorkerLauncher::ReadyPayload final
{
    ~ReadyPayload()
    {
        if (!consumed && context != nullptr) context->retireAsync();
    }

    std::shared_ptr<LaunchRetirementContext> context;
    bool consumed = false;
};

InstalledPackageWorkerLauncher::InstalledPackageWorkerLauncher(
    SandboxTrustBoundary boundary,
    QString workerExecutable,
    QString sandboxTempRoot,
    QUrl apiOrigin,
    BindingValidator validateBinding,
    AttachCallback attach,
    StopCallback stop,
    ExitCallback exited,
    FailureCallback failed,
    QObject *parent)
    : QObject(parent)
    , boundary_(std::move(boundary))
    , workerExecutable_(std::move(workerExecutable))
    , sandboxTempRoot_(std::move(sandboxTempRoot))
    , apiOrigin_(std::move(apiOrigin))
    , validateBinding_(std::move(validateBinding))
    , attach_(std::move(attach))
    , stop_(std::move(stop))
    , exited_(std::move(exited))
    , failed_(std::move(failed))
{
    if (!boundary_.isValid() || workerExecutable_.isEmpty()
        || sandboxTempRoot_.isEmpty() || !apiOrigin_.isValid()
        || apiOrigin_.scheme() != QStringLiteral("http")
        || apiOrigin_.host() != QStringLiteral("127.0.0.1")
        || apiOrigin_.port() <= 0 || !validateBinding_ || !attach_ || !stop_ || !exited_
        || !failed_) {
        accepting_ = false;
    }
}

InstalledPackageWorkerLauncher::~InstalledPackageWorkerLauncher()
{
    cancel();
}

bool InstalledPackageWorkerLauncher::requestLaunch(
    const UpdateLaunchRequest &request)
{
    if (!accepting_ || request.appId.isEmpty() || request.packageVersion.isEmpty()
        || request.packageDirectory.isEmpty() || request.entryPoint.isEmpty()
        || request.expectedActivation.currentDirectory.isEmpty()
        || request.expectedActivation.versionDigestHex.isEmpty()
        || request.expectedActivation.generation <= 0
        || request.key.activation.value == 0
        || request.key.attempt.value == 0) {
        return false;
    }
    const QFileInfo package(request.packageDirectory);
    const QString canonicalPackage = package.canonicalFilePath();
    if (!package.isDir() || package.isSymLink() || canonicalPackage.isEmpty()
        || QDir::cleanPath(canonicalPackage)
               .compare(QDir::cleanPath(request.packageDirectory),
                        Qt::CaseInsensitive)
            != 0) {
        fail(request.key, QStringLiteral("host.launch.package_path_invalid"));
        return false;
    }
    if (!isStableEntryPoint(canonicalPackage, request.entryPoint)) {
        fail(request.key, QStringLiteral("host.launch.entry_point_invalid"));
        return false;
    }
    if (currentProcess_ != nullptr || !inflight_.empty()) {
        pendingRequest_ = request;
        stopCurrent();
        for (const auto &[unused, context] : inflight_) {
            Q_UNUSED(unused);
            context->requestTerminate();
        }
        return true;
    }
    ++serial_;
    const quint64 launchSerial = serial_;
    const auto ownedTemp = createOwnedWorkerTemp(sandboxTempRoot_);
    if (!ownedTemp.has_value()) {
        fail(request.key, QStringLiteral("host.launch.temp_unavailable"));
        return false;
    }
    auto context = std::make_shared<LaunchRetirementContext>();
    context->request = request;
    context->serial = launchSerial;
    context->tempDirectory = ownedTemp->path;
    context->tempTree = ownedTemp->tree;
    QPointer<InstalledPackageWorkerLauncher> guard(this);
    context->retired = [guard](std::shared_ptr<LaunchRetirementContext> retired,
                               const bool succeeded,
                               const QString &stableError) {
        if (!guard) return;
        (void)QMetaObject::invokeMethod(
            guard,
            [guard, retired = std::move(retired), succeeded, stableError] {
                if (guard) guard->handleRetirement(
                    retired, succeeded, stableError);
            }, Qt::QueuedConnection);
    };
    inflight_.emplace(launchSerial, context);
    const QString nonce = QUuid::createUuid().toString(QUuid::Id128);
    SandboxLaunchRequest launchRequest;
    launchRequest.appId = request.appId;
    launchRequest.executablePath = workerExecutable_;
    launchRequest.packageDirectory = canonicalPackage;
    launchRequest.tempDirectory = ownedTemp->path;
    launchRequest.arguments = {
        QStringLiteral("--qbrowser-package"), canonicalPackage,
        QStringLiteral("--qbrowser-entry"), request.entryPoint,
        QStringLiteral("--qbrowser-api-origin"),
        apiOrigin_.toString(QUrl::FullyEncoded),
        QStringLiteral("--qbrowser-nonce"), nonce,
        QStringLiteral("--qbrowser-heartbeat-ms"), QStringLiteral("50"),
    };
    launchRequest.resourceLimits = {1, 512ULL * 1024ULL * 1024ULL};
    auto configured = boundary_.makeLaunchConfig(launchRequest);
    if (!configured.value.has_value()) {
        context->launchFinished();
        context->retireAsync();
        fail(request.key, configured.errorCode.isEmpty()
                              ? QStringLiteral("host.launch.trust_rejected")
                              : configured.errorCode);
        return false;
    }
    QThread *const thread = QThread::create(
        [guard, context, request, nonce, launchSerial,
         validateBinding = validateBinding_,
         config = std::move(*configured.value)]() mutable {
            struct LaunchFinished final
            {
                std::shared_ptr<LaunchRetirementContext> context;
                ~LaunchFinished() { context->launchFinished(); }
            } launchFinished{context};
            auto payload = std::make_shared<ReadyPayload>();
            payload->context = context;
            WinPipePair pair = WinPipeTransport::createHostPair();
            if (!pair.isValid()) {
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key] {
                    if (guard) guard->fail(key, QStringLiteral("host.launch.pipe_failed"));
                }, Qt::QueuedConnection);
                return;
            }
            WinPipeTransport hostPipe = pair.takeHost();
            const InstallResult prelaunch = validateBinding(
                request.appId, request.expectedActivation);
            if (!matchesValidatedActivation(request, prelaunch)) {
                if (guard) QMetaObject::invokeMethod(
                    guard, [guard, key = request.key] {
                        if (guard) guard->fail(
                            key, QStringLiteral("host.launch.stale_activation"));
                    }, Qt::QueuedConnection);
                return;
            }
#ifdef Q_BROWSER_HOST_TESTING
            const auto prelaunchHooks =
                qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
            if (prelaunchHooks.afterBindingValidationBeforeProcessLaunch) {
                prelaunchHooks.afterBindingValidationBeforeProcessLaunch(request);
            }
#endif
            SandboxLaunchResult launched = SandboxLauncher::launch(
                config, pair.takeWorkerEnds());
            if (!launched.process.has_value()) {
                const QString error = launched.errorCode.isEmpty()
                    ? QStringLiteral("host.launch.process_failed")
                    : launched.errorCode;
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                return;
            }
            {
                std::lock_guard lock(context->resourceMutex);
                context->process = std::make_shared<SandboxProcess>(
                    std::move(*launched.process));
            }
#ifdef Q_BROWSER_HOST_TESTING
            const auto processHooks =
                qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
            if (processHooks.afterProcessStartBeforeHandshake) {
                processHooks.afterProcessStartBeforeHandshake(request);
            }
#endif
            {
                std::lock_guard lock(context->resourceMutex);
                context->session = std::make_unique<IpcSession>(
                    std::move(hostPipe), IpcRole::Host,
                    HostLaunchContext{nonce, request.appId});
            }
            const ExpectedMessage handshake = receiveExpected(
                *context->session, ProtocolType::Handshake, 15'000);
            const ExpectedMessage surface = handshake.succeeded
                ? receiveExpected(*context->session, ProtocolType::SurfaceReady, 15'000)
                : handshake;
            const ExpectedMessage ready = surface.succeeded
                ? receiveExpected(*context->session, ProtocolType::Ready, 15'000)
                : surface;
            if (!ready.succeeded || surface.windowHandle.isEmpty()) {
                const QString error = ready.stableError.isEmpty()
                    ? QStringLiteral("host.launch.handshake_failed")
                    : ready.stableError;
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                return;
            }
            const InstallResult admitted = validateBinding(
                request.appId, request.expectedActivation);
            if (!matchesValidatedActivation(request, admitted)) {
                if (guard) QMetaObject::invokeMethod(
                    guard, [guard, key = request.key] {
                        if (guard) guard->fail(
                            key, QStringLiteral("host.launch.stale_activation"));
                    }, Qt::QueuedConnection);
                return;
            }
            context->windowHandle = surface.windowHandle;
#ifdef Q_BROWSER_HOST_TESTING
            const auto handshakeHooks =
                qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
            if (handshakeHooks.afterHandshakeBeforeCompletionQueued) {
                handshakeHooks.afterHandshakeBeforeCompletionQueued(
                    context->process->processId());
            }
#endif
            if (guard) QMetaObject::invokeMethod(
                guard,
                [guard, launchSerial, payload] {
                    if (guard) guard->completeLaunch(launchSerial, payload);
                },
                Qt::QueuedConnection);
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return true;
}

void InstalledPackageWorkerLauncher::completeLaunch(
    const quint64 serial,
    std::shared_ptr<ReadyPayload> payload)
{
    const std::shared_ptr<LaunchRetirementContext> context =
        payload != nullptr ? payload->context : nullptr;
    if (!accepting_ || serial != serial_ || payload == nullptr
        || context == nullptr || context->process == nullptr
        || context->session == nullptr) {
        return;
    }
    std::unique_ptr<WorkerSurface> surface(WorkerSurface::create(
        context->windowHandle, context->process->nativeProcessHandle(),
        context->request.key.attempt));
    std::unique_ptr<IpcSession> session;
    {
        std::lock_guard lock(context->resourceMutex);
        session = std::move(context->session);
    }
    if (surface == nullptr
        || attach_(std::move(session), std::move(surface),
                   context->process, context->request.key)
            != AttachResult::Attached) {
        fail(context->request.key, QStringLiteral("host.launch.attach_failed"));
        return;
    }
    payload->consumed = true;
    context->attached.store(true, std::memory_order_release);
    inflight_.erase(serial);
    currentProcess_ = context->process;
    currentRetirement_ = context;
    currentKey_ = context->request.key;
    expectedStop_.reset();
    emit ready(context->request.appId, context->request.packageVersion,
               context->request.packageDirectory,
               context->request.key.activation.value,
               context->request.key.attempt.value,
               context->process->processId());
    observeProcess(context);
}

void InstalledPackageWorkerLauncher::observeProcess(
    std::shared_ptr<LaunchRetirementContext> context)
{
    QThread *const observer = QThread::create(
        [context = std::move(context)] {
            std::shared_ptr<SandboxProcess> process;
            {
                std::lock_guard lock(context->resourceMutex);
                process = context->process;
            }
            if (process != nullptr) {
                while (!process->waitForFinished(100)) {}
            }
            context->retireAsync();
    });
    connect(observer, &QThread::finished, observer, &QObject::deleteLater);
    observer->start();
}

void InstalledPackageWorkerLauncher::handleRetirement(
    std::shared_ptr<LaunchRetirementContext> context,
    const bool succeeded,
    const QString &stableError)
{
    if (context == nullptr) return;
    inflight_.erase(context->serial);
    if (!succeeded) {
        context->fatalCleanupObserved.store(true, std::memory_order_release);
        accepting_ = false;
        pendingRequest_.reset();
        if (std::ranges::find(fatalCleanup_, context) == fatalCleanup_.end())
            fatalCleanup_.push_back(context);
        if (currentRetirement_ == context) {
            currentRetirement_.reset();
            currentProcess_.reset();
            currentKey_.reset();
        }
        fail(context->request.key,
             stableError.isEmpty()
                 ? QStringLiteral("host.launch.cleanup_failed")
                 : stableError);
        return;
    }
    fatalCleanup_.erase(
        std::remove(fatalCleanup_.begin(), fatalCleanup_.end(), context),
        fatalCleanup_.end());
    const bool wasAttached = context->attached.load(std::memory_order_acquire);
    const WorkerAttemptKey key = context->request.key;
    const bool expected = expectedStop_.has_value() && *expectedStop_ == key;
    if (currentRetirement_ == context) {
        currentRetirement_.reset();
        currentProcess_.reset();
        currentKey_.reset();
    }
    if (wasAttached
        && !context->fatalCleanupObserved.load(std::memory_order_acquire)) {
        if (!expected) emit unexpectedExit(
            key.activation.value, key.attempt.value);
        exited_(key, expected || context->serial != serial_);
        expectedStop_.reset();
    }
    if (accepting_ && inflight_.empty() && currentProcess_ == nullptr
        && pendingRequest_.has_value()) {
        const UpdateLaunchRequest pending = *pendingRequest_;
        pendingRequest_.reset();
        (void)requestLaunch(pending);
    }
}

void InstalledPackageWorkerLauncher::stopCurrent()
{
    if (currentKey_.has_value() && currentProcess_ != nullptr
        && currentProcess_->isRunning()) {
        expectedStop_ = currentKey_;
    }
    if (stop_) stop_();
    if (currentProcess_ != nullptr)
        currentProcess_->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    for (const auto &[unused, context] : inflight_) {
        Q_UNUSED(unused);
        context->requestTerminate();
    }
}

void InstalledPackageWorkerLauncher::cancel() noexcept
{
    accepting_ = false;
    ++serial_;
    pendingRequest_.reset();
    stopCurrent();
    if (currentRetirement_ != nullptr) currentRetirement_->retireAsync();
    for (const auto &[unused, context] : inflight_) {
        Q_UNUSED(unused);
        context->retireAsync();
    }
}

bool InstalledPackageWorkerLauncher::isAccepting() const noexcept
{
    return accepting_;
}

#ifdef Q_BROWSER_HOST_TESTING
bool InstalledPackageWorkerLauncher::retryFatalCleanupForTesting()
{
    if (fatalCleanup_.empty()) return false;
    const auto failures = fatalCleanup_;
    for (const auto &context : failures) context->retryAsync();
    return true;
}
#endif

void InstalledPackageWorkerLauncher::fail(
    const WorkerAttemptKey key,
    const QString &stableError)
{
    if (failed_) failed_(key, stableError.isEmpty()
                                  ? QStringLiteral("host.launch.failed")
                                  : stableError);
}
