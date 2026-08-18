#pragma once

#include "ActivationState.h"

#include <QByteArray>
#include <QString>

enum class PackageStoreError
{
    None,
    InvalidArgument,
    UnsafeStore,
    SourceUnavailable,
    VersionExists,
    CandidateCommitFailed,
    StateUnavailable,
    InvalidState,
    VersionUnavailable,
    StateCommitFailed,
    RollbackUnavailable,
};

struct PackageStoreResult final
{
    PackageStoreError error = PackageStoreError::None;
    QString path;
    QString message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == PackageStoreError::None;
    }
};

struct ActivationStateResult final
{
    PackageStoreError error = PackageStoreError::None;
    ActivationState state;
    QString message;

    [[nodiscard]] bool hasValue() const noexcept
    {
        return error == PackageStoreError::None;
    }
};

class PackageStore final
{
public:
    explicit PackageStore(QString storeRoot);

    [[nodiscard]] const QString &root() const noexcept;
    [[nodiscard]] QString appRoot(const QString &appId) const;
    [[nodiscard]] QString versionPath(
        const QString &appId,
        const QString &versionDirectory) const;

    [[nodiscard]] PackageStoreResult commitCandidate(
        const QString &appId,
        const QString &version,
        const QByteArray &digestHex,
        const QString &candidateRoot,
        const QByteArray &expectedSignedDigest = {}) const;
    [[nodiscard]] ActivationStateResult activationState(
        const QString &appId) const;
    [[nodiscard]] PackageStoreResult activate(
        const QString &appId,
        const QString &versionDirectory) const;
    [[nodiscard]] PackageStoreResult markCurrentLastKnownGood(
        const QString &appId) const;
    [[nodiscard]] PackageStoreResult rollback(const QString &appId) const;
    [[nodiscard]] PackageStoreResult resolveCurrent(const QString &appId) const;

private:
    [[nodiscard]] PackageStoreResult writeState(
        const QString &appId,
        const ActivationState &state) const;
    [[nodiscard]] bool stateTargetIsValid(
        const QString &appId,
        const QString &target,
        bool allowEmpty) const;
    [[nodiscard]] bool ensureAppDirectories(const QString &appId) const;

    QString m_root;
};
