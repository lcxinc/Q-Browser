#include "InstalledPackageWorkerLauncher.h"

#include "InstalledPackageWorkerLauncherTestHooks.h"
#include "WorkerRetirementManager.h"

#include "WindowsStableIo.h"
#include "WorkerSurface.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QPointer>
#include <QTimer>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <semaphore>
#include <system_error>
#include <thread>
#include <utility>

namespace
{
#ifdef Q_BROWSER_HOST_TESTING
std::atomic<qsizetype> liveRetirementContexts{0};
std::atomic<qsizetype> activeLaunchThreads{0};
std::atomic<qsizetype> activeObservers{0};
#endif

enum class LauncherThreadRole
{
    Launch,
    Observer,
};

void recordLauncherValidationFailure(const char *stage,
                                      const WorkerLaunchRequest &request,
                                      const InstallResult &validated)
{
    if (!qEnvironmentVariableIsSet("Q_BROWSER_HOST_DIAGNOSTIC_PHASES")) return;
    const bool bindingMatches = validated.activationBinding.has_value()
        && validated.activationBinding->currentDirectory
               == request.lease.versionDirectory
        && validated.activationBinding->versionDigestHex
               == request.lease.digestHex
        && validated.activationBinding->generation
               == request.lease.activationGenerationAtIssue;
    const QByteArray line = QStringLiteral(
        "qbrowser-host launcher: stale_activation.%1 succeeded=%2 phase=%3 "
        "error=%4 binding=%5 app=%6 version=%7 entry=%8 path=%9\n")
        .arg(QString::fromLatin1(stage))
        .arg(validated.succeeded() ? 1 : 0)
        .arg(static_cast<int>(validated.phase))
        .arg(validated.stableError)
        .arg(bindingMatches ? 1 : 0)
        .arg(validated.appId == request.lease.appId ? 1 : 0)
        .arg(validated.version == request.lease.version ? 1 : 0)
        .arg(validated.entryPoint == request.lease.entryPoint ? 1 : 0)
        .arg(QFileInfo(validated.path).canonicalFilePath().compare(
                 QFileInfo(request.lease.packageDirectory).canonicalFilePath(),
                 Qt::CaseInsensitive) == 0 ? 1 : 0)
        .toUtf8();
    QFile standardError;
    if (!standardError.open(stderr, QIODevice::WriteOnly,
                            QFileDevice::DontCloseHandle)) {
        return;
    }
    (void)standardError.write(line);
    (void)standardError.flush();
}

template<typename Task>
bool startDetachedLauncherThread(const LauncherThreadRole role,
                                 Task &&task) noexcept
{
    try {
#ifdef Q_BROWSER_HOST_TESTING
        const bool injectFailure = role == LauncherThreadRole::Launch
            ? qbrowser_host_testing::
                  consumeLaunchThreadStartFailureForTesting()
            : qbrowser_host_testing::
                  consumeObserverThreadStartFailureForTesting();
        if (injectFailure) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
        }
#else
        Q_UNUSED(role);
#endif
        std::thread(std::forward<Task>(task)).detach();
        return true;
    } catch (...) {
        return false;
    }
}

struct ExpectedMessage final
{
    bool succeeded = false;
    QString windowHandle;
    QString stableError;
};

constexpr int workerStartupPhaseTimeoutMilliseconds = 30'000;

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

bool matchesPermissions(const ManifestPermissions &left,
                        const ManifestPermissions &right)
{
    return left.network.hosts == right.network.hosts
        && left.network.methods == right.network.methods
        && left.storage == right.storage
        && left.clipboardWrite == right.clipboardWrite
        && left.clipboardRead == right.clipboardRead
        && left.fileOpen == right.fileOpen;
}

bool matchesValidatedLease(const WorkerLaunchRequest &request,
                           const InstallResult &validated)
{
    return validated.succeeded() && validated.activationBinding.has_value()
        && validated.activationBinding->currentDirectory
               == request.lease.versionDirectory
        && validated.activationBinding->versionDigestHex
               == request.lease.digestHex
        && validated.activationBinding->generation
               == request.lease.activationGenerationAtIssue
        && validated.appId == request.lease.appId
        && validated.version == request.lease.version
        && validated.entryPoint == request.lease.entryPoint
        && QFileInfo(validated.path).fileName()
               == request.lease.versionDirectory
        && QFileInfo(validated.path).canonicalFilePath().compare(
               QFileInfo(request.lease.packageDirectory).canonicalFilePath(),
               Qt::CaseInsensitive) == 0
        && matchesPermissions(validated.permissions,
                              request.lease.permissions);
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

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
qsizetype installedPackageWorkerLiveRetirementContexts()
{
    return liveRetirementContexts.load(std::memory_order_acquire);
}

qsizetype installedPackageWorkerActiveObservers()
{
    return activeObservers.load(std::memory_order_acquire);
}

qsizetype installedPackageWorkerActiveLaunchThreads()
{
    return activeLaunchThreads.load(std::memory_order_acquire);
}
}
#endif

struct InstalledPackageWorkerLauncher::LaunchRetirementContext final
    : std::enable_shared_from_this<LaunchRetirementContext>
{
    WorkerLaunchRequest request;
    quint64 serial = 0;
    std::unique_ptr<IpcSession> session;
    std::shared_ptr<SandboxProcess> process;
    std::optional<SandboxProcessWaitHandle> observerHandle;
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree;
    QString tempDirectory;
    QString windowHandle;
    std::function<void(std::shared_ptr<LaunchRetirementContext>, bool,
                       const QString &)> retired;
    std::atomic_bool launching{true};
    std::atomic_bool retirementSubmitted{false};
    std::atomic_bool attached{false};
    std::atomic_bool fatalCleanupObserved{false};
    std::atomic_bool observationFailed{false};
    std::atomic_bool terminationRequested{false};
    std::atomic<DWORD> observedExitCode{STILL_ACTIVE};
    std::mutex launchMutex;
    std::condition_variable launchChanged;
    std::mutex resourceMutex;

    LaunchRetirementContext()
    {
#ifdef Q_BROWSER_HOST_TESTING
        liveRetirementContexts.fetch_add(1, std::memory_order_acq_rel);
#endif
    }

    ~LaunchRetirementContext()
    {
#ifdef Q_BROWSER_HOST_TESTING
        liveRetirementContexts.fetch_sub(1, std::memory_order_acq_rel);
#endif
    }

    void requestTerminateNoWait() noexcept
    {
        terminationRequested.store(true, std::memory_order_release);
        std::unique_lock lock(resourceMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        if (process != nullptr)
            process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
    }

    void honorTerminationRequest() noexcept
    {
        if (terminationRequested.load(std::memory_order_acquire))
            requestTerminateNoWait();
    }

    void launchFinished() noexcept
    {
        {
            std::lock_guard lock(launchMutex);
            launching.store(false, std::memory_order_release);
        }
        launchChanged.notify_all();
    }

    WorkerRetirementAttemptResult attemptRetirement()
    {
        {
            std::unique_lock lock(launchMutex);
            if (!launchChanged.wait_for(
                    lock, std::chrono::seconds(2), [this] {
                        return !launching.load(std::memory_order_acquire);
                    })) {
                return {false,
                        QStringLiteral("host.launch.retirement_wait_timeout")};
            }
        }
#ifdef Q_BROWSER_HOST_TESTING
        const auto hooks =
            qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
        if (hooks.beforeRetirementCleanup) hooks.beforeRetirementCleanup();
#endif
        std::shared_ptr<SandboxProcess> ownedProcess;
        std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree>
            ownedTempTree;
        QString ownedTempDirectory;
        {
            std::lock_guard lock(resourceMutex);
            session.reset();
            observerHandle.reset();
            ownedProcess = process;
            ownedTempTree = tempTree;
            ownedTempDirectory = tempDirectory;
        }
        QString error;
        if (ownedProcess != nullptr) {
            ownedProcess->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
            const auto closed = ownedProcess->close();
            if (!closed.value.has_value()) {
                error = closed.errorCode.isEmpty()
                    ? QStringLiteral("host.launch.process_cleanup_failed")
                    : closed.errorCode;
            }
        }
        if (error.isEmpty() && ownedTempTree != nullptr
            && !cleanupOwnedWorkerTemp(ownedTempDirectory, *ownedTempTree)) {
            error = QStringLiteral("host.launch.temp_cleanup_failed");
        }
        if (!error.isEmpty()) return {false, error};
        {
            std::lock_guard lock(resourceMutex);
            process.reset();
            tempTree.reset();
            tempDirectory.clear();
        }
        return {true, {}};
    }

    void submitRetirement()
    {
        bool expected = false;
        if (!retirementSubmitted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) return;
        const auto self = shared_from_this();
        const std::weak_ptr<LaunchRetirementContext> weak = self;
        const auto ticket = WorkerRetirementManager::instance().retire(
            [self] { return self->attemptRetirement(); },
            [weak](const bool succeeded, const QString &stableError) {
                const auto context = weak.lock();
                if (context != nullptr && context->retired) {
                    context->retired(context, succeeded, stableError);
                }
            });
        if (ticket == 0 && retired) {
            retired(self, false,
                    QStringLiteral("host.launch.retirement_unavailable"));
        }
    }

    void retireAsync()
    {
        submitRetirement();
    }
};

struct InstalledPackageWorkerLauncher::ObserverStartGate final
{
    std::binary_semaphore released{0};
};

struct InstalledPackageWorkerLauncher::ReadyPayload final
{
    ~ReadyPayload()
    {
        if (!consumed.load(std::memory_order_acquire) && context != nullptr)
            context->retireAsync();
    }

    std::shared_ptr<LaunchRetirementContext> context;
    std::optional<WorkerLaunchRequest> revalidatedAuthority;
    std::atomic_bool consumed{false};
    std::atomic_bool admissionResolved{false};
    QPointer<QTimer> admissionTimer;
};

InstalledPackageWorkerLauncher::InstalledPackageWorkerLauncher(
    SandboxTrustBoundary boundary,
    QString workerExecutable,
    QString sandboxTempRoot,
    QUrl apiOrigin,
    BindingValidator validateBinding,
    AdmissionCallback requestAdmission,
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
    , requestAdmission_(std::move(requestAdmission))
    , attach_(std::move(attach))
    , stop_(std::move(stop))
    , exited_(std::move(exited))
    , failed_(std::move(failed))
{
    if (!boundary_.isValid() || workerExecutable_.isEmpty()
        || sandboxTempRoot_.isEmpty() || !apiOrigin_.isValid()
        || apiOrigin_.scheme() != QStringLiteral("http")
        || apiOrigin_.host() != QStringLiteral("127.0.0.1")
        || apiOrigin_.port() <= 0 || !validateBinding_ || !requestAdmission_
        || !attach_ || !stop_ || !exited_
        || !failed_) {
        accepting_ = false;
    }
}

InstalledPackageWorkerLauncher::~InstalledPackageWorkerLauncher()
{
    cancel();
}

bool InstalledPackageWorkerLauncher::requestLaunch(
    const WorkerLaunchRequest &request)
{
    if (!accepting_ || request.tabId.isEmpty()
        || request.runtimeIncarnation == 0 || request.route.isEmpty()
        || request.lease.appId.isEmpty() || request.lease.version.isEmpty()
        || request.lease.versionDirectory.isEmpty()
        || request.lease.packageDirectory.isEmpty()
        || request.lease.entryPoint.isEmpty()
        || request.lease.digestHex.isEmpty()
        || request.lease.activationGenerationAtIssue <= 0
        || request.lease.leaseAuthorityEpoch == 0
        || request.admission == nullptr
        || request.attempt.activation.value == 0
        || request.attempt.attempt.value == 0) {
        return false;
    }
    const QFileInfo package(request.lease.packageDirectory);
    const QString canonicalPackage = package.canonicalFilePath();
    if (!package.isDir() || package.isSymLink() || canonicalPackage.isEmpty()
        || QDir::cleanPath(canonicalPackage)
               .compare(QDir::cleanPath(request.lease.packageDirectory),
                         Qt::CaseInsensitive)
            != 0) {
        fail(request.attempt, QStringLiteral("host.launch.package_path_invalid"));
        return false;
    }
    if (!isStableEntryPoint(canonicalPackage, request.lease.entryPoint)) {
        fail(request.attempt, QStringLiteral("host.launch.entry_point_invalid"));
        return false;
    }
    if (currentProcess_ != nullptr || !inflight_.empty()) {
        pendingRequest_ = request;
        stopCurrent();
        for (const auto &[unused, context] : inflight_) {
            Q_UNUSED(unused);
            context->requestTerminateNoWait();
        }
        return true;
    }
    ++serial_;
    const quint64 launchSerial = serial_;
    const auto ownedTemp = createOwnedWorkerTemp(sandboxTempRoot_);
    if (!ownedTemp.has_value()) {
        fail(request.attempt, QStringLiteral("host.launch.temp_unavailable"));
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
    launchRequest.appId = request.lease.appId;
    launchRequest.executablePath = workerExecutable_;
    launchRequest.packageDirectory = canonicalPackage;
    launchRequest.tempDirectory = ownedTemp->path;
    launchRequest.arguments = {
        QStringLiteral("--qbrowser-package"), canonicalPackage,
        QStringLiteral("--qbrowser-entry"), request.lease.entryPoint,
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
        fail(request.attempt, configured.errorCode.isEmpty()
                              ? QStringLiteral("host.launch.trust_rejected")
                              : configured.errorCode);
        return false;
    }
    auto launchTask =
        [guard, context, request, nonce, launchSerial,
         validateBinding = validateBinding_,
         config = std::move(*configured.value)]() mutable {
#ifdef Q_BROWSER_HOST_TESTING
            struct LaunchThreadCount final
            {
                ~LaunchThreadCount()
                {
                    activeLaunchThreads.fetch_sub(1,
                                                  std::memory_order_acq_rel);
                }
            } launchThreadCount;
            static_cast<void>(launchThreadCount);
#endif
            struct LaunchFinished final
            {
                std::shared_ptr<LaunchRetirementContext> context;
                ~LaunchFinished() { context->launchFinished(); }
            } launchFinished{context};
            auto payload = std::make_shared<ReadyPayload>();
            payload->context = context;
            WinPipePair pair = WinPipeTransport::createHostPair();
            if (!pair.isValid()) {
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.attempt] {
                    if (guard) guard->fail(key, QStringLiteral("host.launch.pipe_failed"));
                }, Qt::QueuedConnection);
                return;
            }
            WinPipeTransport hostPipe = pair.takeHost();
#ifdef Q_BROWSER_HOST_TESTING
            const auto beforeValidationHooks =
                qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
            if (beforeValidationHooks.beforeBindingValidation) {
                beforeValidationHooks.beforeBindingValidation(request);
            }
#endif
            const InstallResult prelaunch = validateBinding(request);
            if (!matchesValidatedLease(request, prelaunch)) {
                recordLauncherValidationFailure("prelaunch", request,
                                                prelaunch);
                if (guard) QMetaObject::invokeMethod(
                    guard, [guard, key = request.attempt] {
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
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.attempt, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                return;
            }
            {
                auto process = std::make_shared<SandboxProcess>(
                    std::move(*launched.process));
                auto observer = process->duplicateWaitHandle();
                if (!observer.value.has_value()) {
                    const QString error = observer.errorCode.isEmpty()
                        ? QStringLiteral("host.launch.process_observer_failed")
                        : observer.errorCode;
                    {
                        std::lock_guard lock(context->resourceMutex);
                        context->process = std::move(process);
                    }
                    if (guard) QMetaObject::invokeMethod(
                        guard, [guard, key = request.attempt, error] {
                            if (guard) guard->fail(key, error);
                        }, Qt::QueuedConnection);
                    return;
                }
                std::lock_guard lock(context->resourceMutex);
                context->process = std::move(process);
                context->observerHandle = std::move(*observer.value);
            }
            context->honorTerminationRequest();
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
                    HostLaunchContext{nonce, request.lease.appId});
            }
            context->honorTerminationRequest();
            const ExpectedMessage handshake = receiveExpected(
                *context->session, ProtocolType::Handshake,
                workerStartupPhaseTimeoutMilliseconds);
            const ExpectedMessage surface = handshake.succeeded
                ? receiveExpected(*context->session, ProtocolType::SurfaceReady,
                                  workerStartupPhaseTimeoutMilliseconds)
                : handshake;
            const ExpectedMessage ready = surface.succeeded
                ? receiveExpected(*context->session, ProtocolType::Ready,
                                  workerStartupPhaseTimeoutMilliseconds)
                : surface;
            if (!ready.succeeded || surface.windowHandle.isEmpty()) {
                const QString error = ready.stableError.isEmpty()
                    ? QStringLiteral("host.launch.handshake_failed")
                    : ready.stableError;
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.attempt, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                return;
            }
            const InstallResult admitted = validateBinding(request);
            if (!matchesValidatedLease(request, admitted)) {
                recordLauncherValidationFailure("posthandshake", request,
                                                admitted);
                if (guard) QMetaObject::invokeMethod(
                    guard, [guard, key = request.attempt] {
                        if (guard) guard->fail(
                            key, QStringLiteral("host.launch.stale_activation"));
                    }, Qt::QueuedConnection);
                return;
            }
            payload->revalidatedAuthority = request;
            context->windowHandle = surface.windowHandle;
#ifdef Q_BROWSER_HOST_TESTING
            const auto handshakeHooks =
                qbrowser_host_testing::installedPackageWorkerLauncherTestHooks();
            if (handshakeHooks.afterHandshakeBeforeCompletionQueued) {
                handshakeHooks.afterHandshakeBeforeCompletionQueued(
                    request, context->process->processId());
            }
#endif
            if (guard) QMetaObject::invokeMethod(
                guard,
                [guard, launchSerial, payload] {
                    if (guard) guard->requestAdmission(launchSerial, payload);
                },
                Qt::QueuedConnection);
        };
#ifdef Q_BROWSER_HOST_TESTING
    activeLaunchThreads.fetch_add(1, std::memory_order_acq_rel);
#endif
    if (!startDetachedLauncherThread(LauncherThreadRole::Launch,
                                     std::move(launchTask))) {
#ifdef Q_BROWSER_HOST_TESTING
        activeLaunchThreads.fetch_sub(1, std::memory_order_acq_rel);
#endif
        context->launchFinished();
        context->retireAsync();
        fail(request.attempt,
             QStringLiteral("host.launch.launch_thread_unavailable"));
        return false;
    }
    return true;
}

void InstalledPackageWorkerLauncher::requestAdmission(
    const quint64 serial,
    std::shared_ptr<ReadyPayload> payload)
{
    if (!accepting_ || serial != serial_ || payload == nullptr
        || payload->context == nullptr) {
        return;
    }
    auto *const timeout = new QTimer(this);
    timeout->setSingleShot(true);
    payload->admissionTimer = timeout;
    QPointer<InstalledPackageWorkerLauncher> guard(this);
    const std::weak_ptr<ReadyPayload> weakPayload = payload;
    connect(timeout, &QTimer::timeout, this,
            [guard, serial, weakPayload] {
        const auto retained = weakPayload.lock();
        if (guard && retained != nullptr) {
            guard->completeLaunch(
                serial, retained,
                {false, QStringLiteral("host.launch.admission_timeout")});
        }
    });
    timeout->start(5'000);
    const bool queued = requestAdmission_(
        payload->context->request,
        [guard, serial, payload](AdmissionResult admission) mutable {
            if (!guard) return;
            (void)QMetaObject::invokeMethod(
                guard,
                [guard, serial, payload,
                 admission = std::move(admission)]() mutable {
                    if (guard) guard->completeLaunch(
                        serial, payload, std::move(admission));
                }, Qt::QueuedConnection);
        });
    if (!queued) {
        completeLaunch(
            serial, std::move(payload),
            {false, QStringLiteral("host.launch.admission_unavailable")});
    }
}

void InstalledPackageWorkerLauncher::completeLaunch(
    const quint64 serial,
    std::shared_ptr<ReadyPayload> payload,
    AdmissionResult admission)
{
    const std::shared_ptr<LaunchRetirementContext> context =
        payload != nullptr ? payload->context : nullptr;
    if (payload == nullptr
        || payload->admissionResolved.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }
    if (payload->admissionTimer) {
        payload->admissionTimer->stop();
        payload->admissionTimer->deleteLater();
        payload->admissionTimer = nullptr;
    }
    if (!admission.accepted) {
        if (context != nullptr) context->retireAsync();
        if (admission.ignoredStale) return;
        fail(context != nullptr ? context->request.attempt : WorkerAttemptKey{},
             admission.stableError.isEmpty()
                 ? QStringLiteral("host.launch.admission_rejected")
                 : admission.stableError);
        return;
    }
    if (!accepting_ || serial != serial_ || payload == nullptr
        || context == nullptr || context->process == nullptr
        || context->session == nullptr
        || !payload->revalidatedAuthority.has_value()
        || !admission.authority.has_value()
        || *payload->revalidatedAuthority != context->request
        || *admission.authority != context->request) {
        if (context != nullptr) context->retireAsync();
        if (context != nullptr && accepting_ && serial == serial_) {
            fail(context->request.attempt,
                 QStringLiteral("host.launch.admission_authority_mismatch"));
        }
        return;
    }
    std::optional<SandboxProcessWaitHandle> observerHandle;
    {
        std::lock_guard lock(context->resourceMutex);
        if (context->observerHandle.has_value()) {
            observerHandle.emplace(std::move(*context->observerHandle));
            context->observerHandle.reset();
        }
    }
    if (!observerHandle.has_value()) {
        context->observationFailed.store(true, std::memory_order_release);
        context->retireAsync();
        fail(context->request.attempt,
             QStringLiteral("host.launch.process_observer_failed"));
        return;
    }
    const std::shared_ptr<ObserverStartGate> observerGate = observeProcess(
        context, std::move(*observerHandle));
    if (observerGate == nullptr) return;
    std::unique_ptr<WorkerSurface> surface(WorkerSurface::create(
        context->windowHandle, context->process->nativeProcessHandle(),
        context->request.attempt.attempt));
    std::unique_ptr<IpcSession> session;
    {
        std::lock_guard lock(context->resourceMutex);
        session = std::move(context->session);
    }
    auto use = context->request.admission != nullptr
        ? context->request.admission->tryAcquireUse()
        : std::nullopt;
    const bool surfaceCreated = surface != nullptr;
    bool authorityCommitted = false;
    const bool published = surfaceCreated && use.has_value()
        && use->publishIfStillAdmitted([&] {
               if (!payload->revalidatedAuthority.has_value()
                   || !admission.authority.has_value()
                   || *payload->revalidatedAuthority != context->request
                   || *admission.authority != context->request) {
                   return false;
               }
               authorityCommitted = true;
               return true;
           });
    AttachResult attachResult = AttachResult::ConsumedFailure;
    if (published && authorityCommitted) {
        attachResult = attach_(context->request,
                               std::move(session),
                               std::move(surface),
                               context->process);
    }
    use.reset();
    if (!published || attachResult != AttachResult::Attached) {
        observerGate->released.release();
        context->retireAsync();
        fail(context->request.attempt,
             surfaceCreated && !authorityCommitted
                 ? QStringLiteral("host.launch.admission_revoked")
                 : QStringLiteral("host.launch.attach_failed"));
        return;
    }
    payload->consumed.store(true, std::memory_order_release);
    context->attached.store(true, std::memory_order_release);
    inflight_.erase(serial);
    currentProcess_ = context->process;
    currentRetirement_ = context;
    currentKey_ = context->request.attempt;
    expectedStop_.reset();
    const QString readyAppId = context->request.lease.appId;
    const QString readyVersion = context->request.lease.version;
    const QString readyDirectory = context->request.lease.packageDirectory;
    const quint64 readyActivation = context->request.attempt.activation.value;
    const quint64 readyAttempt = context->request.attempt.attempt.value;
    const quint32 readyProcessId = context->process->processId();
    observerGate->released.release();
    emit ready(readyAppId, readyVersion, readyDirectory,
               readyActivation, readyAttempt, readyProcessId);
}

std::shared_ptr<InstalledPackageWorkerLauncher::ObserverStartGate>
InstalledPackageWorkerLauncher::observeProcess(
    std::shared_ptr<LaunchRetirementContext> context,
    SandboxProcessWaitHandle waitHandle)
{
    auto gate = std::make_shared<ObserverStartGate>();
#ifdef Q_BROWSER_HOST_TESTING
    activeObservers.fetch_add(1, std::memory_order_acq_rel);
#endif
    auto observerTask =
        [context,
         waitHandle = std::move(waitHandle), gate]() mutable {
#ifdef Q_BROWSER_HOST_TESTING
            struct ObserverCount final
            {
                ~ObserverCount()
                {
                    activeObservers.fetch_sub(1, std::memory_order_acq_rel);
                }
            } observerCount;
            static_cast<void>(observerCount);
#endif
            gate->released.acquire();
            for (;;) {
                const SandboxProcessWaitResult waited = waitHandle.wait(100);
                if (waited == SandboxProcessWaitResult::Finished) {
                    std::lock_guard lock(context->resourceMutex);
                    if (context->process != nullptr) {
                        context->observedExitCode.store(
                            context->process->exitCode(),
                            std::memory_order_release);
                    }
                    break;
                }
                if (waited == SandboxProcessWaitResult::Error) {
                    context->observationFailed.store(
                        true, std::memory_order_release);
                    context->requestTerminateNoWait();
                    break;
                }
            }
            context->retireAsync();
        };
    if (!startDetachedLauncherThread(LauncherThreadRole::Observer,
                                     std::move(observerTask))) {
#ifdef Q_BROWSER_HOST_TESTING
        activeObservers.fetch_sub(1, std::memory_order_acq_rel);
#endif
        context->observationFailed.store(true, std::memory_order_release);
        context->requestTerminateNoWait();
        context->retireAsync();
        fail(context->request.attempt,
             QStringLiteral("host.launch.observer_thread_unavailable"));
        return {};
    }
    return gate;
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
        fail(context->request.attempt,
             stableError.isEmpty()
                 ? QStringLiteral("host.launch.cleanup_failed")
                 : stableError);
        return;
    }
    fatalCleanup_.erase(
        std::remove(fatalCleanup_.begin(), fatalCleanup_.end(), context),
        fatalCleanup_.end());
    const bool wasAttached = context->attached.load(std::memory_order_acquire);
    const WorkerAttemptKey key = context->request.attempt;
    const bool expected = expectedStop_.has_value() && *expectedStop_ == key;
    if (currentRetirement_ == context) {
        currentRetirement_.reset();
        currentProcess_.reset();
        currentKey_.reset();
    }
    if (wasAttached
        && !context->fatalCleanupObserved.load(std::memory_order_acquire)) {
        if (context->observationFailed.load(std::memory_order_acquire)) {
            accepting_ = false;
            pendingRequest_.reset();
            fail(key, QStringLiteral("host.launch.process_observer_failed"));
        } else {
            if (!expected) {
                QFile standardError;
                if (qEnvironmentVariableIsSet(
                        "Q_BROWSER_HOST_DIAGNOSTIC_PHASES")
                    && standardError.open(stderr, QIODevice::WriteOnly,
                                          QFileDevice::DontCloseHandle)) {
                    const QByteArray line = QStringLiteral(
                        "qbrowser-host worker exited unexpectedly: code=0x%1\n")
                        .arg(context->observedExitCode.load(
                                 std::memory_order_acquire),
                             8, 16, QLatin1Char('0'))
                        .toUtf8();
                    (void)standardError.write(line);
                    (void)standardError.flush();
                }
                emit unexpectedExit(key.activation.value, key.attempt.value);
            }
            exited_(key, expected || context->serial != serial_);
        }
        expectedStop_.reset();
    }
    if (accepting_ && inflight_.empty() && currentProcess_ == nullptr
        && pendingRequest_.has_value()) {
        const WorkerLaunchRequest pending = *pendingRequest_;
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
        context->requestTerminateNoWait();
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
    return WorkerRetirementManager::instance().retryFatal();
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
