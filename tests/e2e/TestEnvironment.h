#pragma once

#include "HostApplication.h"
#include "WorkerTestEnvironment.h"

#include <QProcess>
#include <QTemporaryDir>

#include <memory>

class TestEnvironment final
{
public:
    TestEnvironment();
    ~TestEnvironment();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] QString error() const;
    [[nodiscard]] HostApplication *host() const noexcept;
    [[nodiscard]] QString mockOrigin() const;
    [[nodiscard]] QString createPackage(const QString &version,
                                        const QByteArray &mainQml = {});
    [[nodiscard]] QString createTamperedPackage(const QString &version);
    [[nodiscard]] bool start(const QString &version = QStringLiteral("1.0.0"));
    [[nodiscard]] bool install(const QString &packagePath);
    [[nodiscard]] bool waitForReady(const QString &version, int timeoutMs = 60'000);
    [[nodiscard]] bool waitForFailure(int previousCount, int timeoutMs = 15'000);
    [[nodiscard]] bool shutdown();
    [[nodiscard]] int failureCount() const noexcept;
    [[nodiscard]] QString lastFailure() const;
    [[nodiscard]] int tamperCanaryCount() const noexcept;
    [[nodiscard]] QString currentVersionDirectory() const;
    [[nodiscard]] QString lastKnownGoodVersionDirectory() const;
    [[nodiscard]] quint32 currentWorkerProcessId() const noexcept;
    [[nodiscard]] QStringList readyVersions() const;

private:
    bool startMockApi();
    bool prepareTrustKey();

    WorkerTestEnvironment workerEnvironment_;
    QString appId_;
    QTemporaryDir packages_;
    QTemporaryDir trust_;
    QTemporaryDir telemetry_;
    QProcess mockApi_;
    QByteArray privateKey_;
    QByteArray publicKey_;
    QString publicKeyPath_;
    QString mockOrigin_;
    QString error_;
    std::unique_ptr<HostApplication> host_;
    QStringList readyVersions_;
    int failureCount_ = 0;
    int tamperCanaryCount_ = 0;
    int heartbeatCount_ = 0;
    quint32 currentWorkerProcessId_ = 0;
    QString lastFailure_;
    QString cleanupError_;
    bool shutdown_ = false;
};
