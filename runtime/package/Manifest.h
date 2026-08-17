#pragma once

#include "ManifestError.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

struct RuntimeCompatibility
{
    QString minVersion;
    QString maxVersion;
};

struct NetworkPermission
{
    QStringList hosts;
    QStringList methods;
};

enum class StoragePermission
{
    Disabled,
    AppPrivate,
};

enum class ClipboardReadPermission
{
    Disabled,
    UserGesture,
};

enum class FileOpenPermission
{
    Disabled,
    UserBrokered,
};

struct ManifestPermissions
{
    NetworkPermission network;
    StoragePermission storage = StoragePermission::Disabled;
    bool clipboardWrite = false;
    ClipboardReadPermission clipboardRead = ClipboardReadPermission::Disabled;
    FileOpenPermission fileOpen = FileOpenPermission::Disabled;
};

struct ManifestLimits
{
    qint64 packageBytes = 0;
    int memoryMiB = 0;
    int processes = 0;
};

class ManifestParseResult;

class Manifest final
{
public:
    [[nodiscard]] static ManifestParseResult parse(const QByteArray &bytes);

    [[nodiscard]] int schemaVersion() const noexcept;
    [[nodiscard]] const QString &appId() const noexcept;
    [[nodiscard]] const QString &version() const noexcept;
    [[nodiscard]] const QString &entryPoint() const noexcept;
    [[nodiscard]] const RuntimeCompatibility &runtime() const noexcept;
    [[nodiscard]] const QStringList &imports() const noexcept;
    [[nodiscard]] const ManifestPermissions &permissions() const noexcept;
    [[nodiscard]] const ManifestLimits &limits() const noexcept;
    [[nodiscard]] const QStringList &routes() const noexcept;

private:
    Manifest() = default;

    int m_schemaVersion = 0;
    QString m_appId;
    QString m_version;
    QString m_entryPoint;
    RuntimeCompatibility m_runtime;
    QStringList m_imports;
    ManifestPermissions m_permissions;
    ManifestLimits m_limits;
    QStringList m_routes;
};

class ManifestParseResult final
{
public:
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const Manifest &value() const;
    [[nodiscard]] const QVector<ManifestError> &errors() const noexcept;

private:
    friend class Manifest;

    explicit ManifestParseResult(Manifest value);
    explicit ManifestParseResult(QVector<ManifestError> errors);

    std::optional<Manifest> m_value;
    QVector<ManifestError> m_errors;
};
