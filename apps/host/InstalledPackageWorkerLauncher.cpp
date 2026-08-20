#include "InstalledPackageWorkerLauncher.h"

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

void retireProcess(
    std::shared_ptr<SandboxProcess> process,
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree,
    QString tempDirectory)
{
    if (process == nullptr) return;
    QThread *const observer = QThread::create(
        [process = std::move(process), tempTree = std::move(tempTree),
         tempDirectory = std::move(tempDirectory)]() mutable {
            process->requestTerminateNoWait(ERROR_PROCESS_ABORTED);
            while (!process->waitForFinished(100)) {}
            (void)process->close();
            if (tempTree != nullptr)
                (void)cleanupOwnedWorkerTemp(tempDirectory, *tempTree);
        });
    QObject::connect(observer, &QThread::finished,
                     observer, &QObject::deleteLater);
    observer->start();
}
}

struct InstalledPackageWorkerLauncher::ReadyPayload final
{
    UpdateLaunchRequest request;
    std::unique_ptr<IpcSession> session;
    std::shared_ptr<SandboxProcess> process;
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree;
    QString tempDirectory;
    QString windowHandle;
};

InstalledPackageWorkerLauncher::InstalledPackageWorkerLauncher(
    SandboxTrustBoundary boundary,
    QString workerExecutable,
    QString sandboxTempRoot,
    QUrl apiOrigin,
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
    , attach_(std::move(attach))
    , stop_(std::move(stop))
    , exited_(std::move(exited))
    , failed_(std::move(failed))
{
    if (!boundary_.isValid() || workerExecutable_.isEmpty()
        || sandboxTempRoot_.isEmpty() || !apiOrigin_.isValid()
        || apiOrigin_.scheme() != QStringLiteral("http")
        || apiOrigin_.host() != QStringLiteral("127.0.0.1")
        || apiOrigin_.port() <= 0 || !attach_ || !stop_ || !exited_
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
    if (currentProcess_ != nullptr) {
        pendingRequest_ = request;
        stopCurrent();
        return true;
    }
    ++serial_;
    const quint64 launchSerial = serial_;
    const auto ownedTemp = createOwnedWorkerTemp(sandboxTempRoot_);
    if (!ownedTemp.has_value()) {
        fail(request.key, QStringLiteral("host.launch.temp_unavailable"));
        return false;
    }
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
        (void)cleanupOwnedWorkerTemp(ownedTemp->path, *ownedTemp->tree);
        fail(request.key, configured.errorCode.isEmpty()
                              ? QStringLiteral("host.launch.trust_rejected")
                              : configured.errorCode);
        return false;
    }
    QPointer<InstalledPackageWorkerLauncher> guard(this);
    QThread *const thread = QThread::create(
        [guard, request, nonce, launchSerial,
         tempDirectory = ownedTemp->path, tempTree = ownedTemp->tree,
         config = std::move(*configured.value)]() mutable {
            auto payload = std::make_shared<ReadyPayload>();
            payload->request = request;
            payload->tempDirectory = tempDirectory;
            payload->tempTree = tempTree;
            WinPipePair pair = WinPipeTransport::createHostPair();
            if (!pair.isValid()) {
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key] {
                    if (guard) guard->fail(key, QStringLiteral("host.launch.pipe_failed"));
                }, Qt::QueuedConnection);
                (void)cleanupOwnedWorkerTemp(tempDirectory, *tempTree);
                return;
            }
            WinPipeTransport hostPipe = pair.takeHost();
            SandboxLaunchResult launched = SandboxLauncher::launch(
                config, pair.takeWorkerEnds());
            if (!launched.process.has_value()) {
                const QString error = launched.errorCode.isEmpty()
                    ? QStringLiteral("host.launch.process_failed")
                    : launched.errorCode;
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                (void)cleanupOwnedWorkerTemp(tempDirectory, *tempTree);
                return;
            }
            payload->process = std::make_shared<SandboxProcess>(
                std::move(*launched.process));
            payload->session = std::make_unique<IpcSession>(
                std::move(hostPipe), IpcRole::Host,
                HostLaunchContext{nonce, request.appId});
            const ExpectedMessage handshake = receiveExpected(
                *payload->session, ProtocolType::Handshake, 15'000);
            const ExpectedMessage surface = handshake.succeeded
                ? receiveExpected(*payload->session, ProtocolType::SurfaceReady, 15'000)
                : handshake;
            const ExpectedMessage ready = surface.succeeded
                ? receiveExpected(*payload->session, ProtocolType::Ready, 15'000)
                : surface;
            if (!ready.succeeded || surface.windowHandle.isEmpty()) {
                payload->process->terminate(ERROR_PROCESS_ABORTED);
                (void)payload->process->close();
                (void)cleanupOwnedWorkerTemp(tempDirectory, *tempTree);
                const QString error = ready.stableError.isEmpty()
                    ? QStringLiteral("host.launch.handshake_failed")
                    : ready.stableError;
                if (guard) QMetaObject::invokeMethod(guard, [guard, key = request.key, error] {
                    if (guard) guard->fail(key, error);
                }, Qt::QueuedConnection);
                return;
            }
            payload->windowHandle = surface.windowHandle;
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
    if (!accepting_ || serial != serial_ || payload == nullptr
        || payload->process == nullptr || payload->session == nullptr) {
        if (payload != nullptr && payload->process != nullptr)
            retireProcess(std::move(payload->process), std::move(payload->tempTree),
                          std::move(payload->tempDirectory));
        return;
    }
    std::unique_ptr<WorkerSurface> surface(WorkerSurface::create(
        payload->windowHandle, payload->process->nativeProcessHandle(),
        payload->request.key.attempt));
    if (surface == nullptr
        || attach_(std::move(payload->session), std::move(surface),
                   payload->process, payload->request.key)
            != AttachResult::Attached) {
        retireProcess(std::move(payload->process), std::move(payload->tempTree),
                      std::move(payload->tempDirectory));
        fail(payload->request.key, QStringLiteral("host.launch.attach_failed"));
        return;
    }
    currentProcess_ = payload->process;
    currentKey_ = payload->request.key;
    expectedStop_.reset();
    emit ready(payload->request.appId, payload->request.packageVersion,
               payload->request.packageDirectory,
               payload->request.key.activation.value,
               payload->request.key.attempt.value,
               payload->process->processId());
    observeProcess(payload->request.key, payload->process,
                   std::move(payload->tempTree),
                   std::move(payload->tempDirectory), serial);
}

void InstalledPackageWorkerLauncher::observeProcess(
    const WorkerAttemptKey key,
    std::shared_ptr<SandboxProcess> process,
    std::shared_ptr<qbrowser_archive_detail::WindowsStableDirectoryTree> tempTree,
    QString tempDirectory,
    const quint64 serial)
{
    QPointer<InstalledPackageWorkerLauncher> guard(this);
    QThread *const observer = QThread::create(
        [guard, key, process = std::move(process),
         tempTree = std::move(tempTree), tempDirectory = std::move(tempDirectory),
         serial]() mutable {
        while (!process->waitForFinished(100)) {}
        (void)process->close();
        const bool cleaned = tempTree != nullptr
            && cleanupOwnedWorkerTemp(tempDirectory, *tempTree);
        if (!cleaned) {
            if (guard) QMetaObject::invokeMethod(
                guard, [guard, key] {
                    if (!guard) return;
                    guard->accepting_ = false;
                    guard->pendingRequest_.reset();
                    if (guard->currentKey_.has_value()
                        && *guard->currentKey_ == key) {
                        guard->currentKey_.reset();
                        guard->currentProcess_.reset();
                    }
                    guard->fail(
                        key, QStringLiteral("host.launch.temp_cleanup_failed"));
                }, Qt::QueuedConnection);
            return;
        }
        if (guard) QMetaObject::invokeMethod(guard, [guard, key, serial] {
            if (!guard) return;
            const bool expected = guard->expectedStop_.has_value()
                && *guard->expectedStop_ == key;
            if (guard->currentKey_.has_value() && *guard->currentKey_ == key) {
                guard->currentKey_.reset();
                guard->currentProcess_.reset();
            }
            if (!expected) emit guard->unexpectedExit(
                key.activation.value, key.attempt.value);
            guard->exited_(key, expected || serial != guard->serial_);
            guard->expectedStop_.reset();
            if (guard->accepting_ && guard->pendingRequest_.has_value()) {
                const UpdateLaunchRequest pending = *guard->pendingRequest_;
                guard->pendingRequest_.reset();
                (void)guard->requestLaunch(pending);
            }
        }, Qt::QueuedConnection);
    });
    connect(observer, &QThread::finished, observer, &QObject::deleteLater);
    observer->start();
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
}

void InstalledPackageWorkerLauncher::cancel() noexcept
{
    if (!accepting_) return;
    accepting_ = false;
    ++serial_;
    pendingRequest_.reset();
    stopCurrent();
}

bool InstalledPackageWorkerLauncher::isAccepting() const noexcept
{
    return accepting_;
}

void InstalledPackageWorkerLauncher::fail(
    const WorkerAttemptKey key,
    const QString &stableError)
{
    if (failed_) failed_(key, stableError.isEmpty()
                                  ? QStringLiteral("host.launch.failed")
                                  : stableError);
}
