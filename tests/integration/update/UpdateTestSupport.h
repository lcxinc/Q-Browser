#pragma once

#include "Archive.h"
#include "ContentDigest.h"
#include "PackageInstaller.h"
#include "PackageStore.h"
#include "SignatureVerifier.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <Aclapi.h>
#include <qt_windows.h>
#endif

class UpdateTemporaryDir final : public QTemporaryDir
{
public:
    ~UpdateTemporaryDir()
    {
#ifdef Q_OS_WIN
        if (!isValid()) return;
        QStringList paths{path()};
        QDirIterator iterator(path(),
                              QDir::AllEntries | QDir::Hidden | QDir::System
                                  | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) paths.push_back(iterator.next());
        for (const QString &entry : paths) {
            QString native = QDir::toNativeSeparators(entry);
            (void)SetNamedSecurityInfoW(
                reinterpret_cast<LPWSTR>(native.data()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, nullptr, nullptr);
            const auto *pathText = reinterpret_cast<LPCWSTR>(native.utf16());
            const DWORD attributes = GetFileAttributesW(pathText);
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                (void)SetFileAttributesW(pathText,
                                         attributes & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        (void)QDir(path()).removeRecursively();
        setAutoRemove(false);
#endif
    }
};

inline QByteArray updateManifest(const QString &version)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("appId"), QStringLiteral("company.pilot")},
        {QStringLiteral("version"), version},
        {QStringLiteral("entryPoint"), QStringLiteral("qml/Main.qml")},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("minVersion"), QStringLiteral("1.0.0")},
                     {QStringLiteral("maxVersion"), QStringLiteral("1.x")}}},
        {QStringLiteral("imports"), QJsonArray{QStringLiteral("QtQuick")}},
        {QStringLiteral("permissions"), QJsonObject{}},
        {QStringLiteral("limits"),
         QJsonObject{{QStringLiteral("packageBytes"), 1048576},
                     {QStringLiteral("memoryMiB"), 128},
                     {QStringLiteral("processes"), 1}}},
        {QStringLiteral("routes"), QJsonArray{QStringLiteral("/")}}})
        .toJson(QJsonDocument::Compact);
}

inline QString updateSignedPackage(QTemporaryDir &temporary,
                                   const QString &name,
                                   const QString &version,
                                   const QByteArray &privateKey,
                                   const bool corruptSignature = false,
                                   QByteArray qml = QByteArrayLiteral(
                                       "import QtQuick\nItem { width: 320; height: 200 }"))
{
    QVector<ArchiveFile> files{{QByteArrayLiteral("manifest.json"),
                                updateManifest(version)},
                               {QByteArrayLiteral("qml/Main.qml"),
                                std::move(qml)}};
    const ContentDigestResult payload = ContentDigest::payload(files);
    if (!payload.hasValue()) return {};
    files.push_back({QByteArrayLiteral("metadata/content.sha256"), payload.hex()});
    const ContentDigestResult signedDigest = ContentDigest::signedPackage(files);
    if (!signedDigest.hasValue()) return {};
    const SignatureOperationResult signedValue = SignatureVerifier::signPem(
        signedDigest.bytes(), privateKey);
    if (!signedValue.hasValue()) return {};
    QByteArray signature = signedValue.value();
    if (corruptSignature) signature[0] ^= 1;
    files.push_back({QByteArrayLiteral("metadata/signature.ed25519"), signature});
    const QString path = temporary.filePath(name + QStringLiteral(".qapkg"));
    return Archive::createFromFiles(files, path).hasValue() ? path : QString{};
}

inline InstallPolicy updateInstallPolicy()
{
    InstallPolicy policy;
    policy.runtimeVersion = QStringLiteral("1.2.0");
    policy.allowedImports = {QStringLiteral("QtQuick")};
    policy.preflight = [](const Manifest &, const QString &) { return true; };
    return policy;
}
