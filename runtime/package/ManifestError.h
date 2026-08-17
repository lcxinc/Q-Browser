#pragma once

#include <QMetaType>
#include <QString>

enum class ManifestErrorCode
{
    InvalidJson,
    RootNotObject,
    MissingRequiredField,
    UnknownField,
    WrongType,
    UnsupportedSchemaVersion,
    InvalidAppId,
    InvalidSemanticVersion,
    InvalidRuntimeRange,
    InvalidEntryPoint,
    UnsupportedImport,
    DuplicateImport,
    InvalidNetworkHost,
    DuplicateNetworkHost,
    UnsupportedNetworkMethod,
    DuplicateNetworkMethod,
    InvalidStoragePermission,
    InvalidClipboardReadPermission,
    InvalidFileOpenPermission,
    ProcessPermissionDenied,
    InvalidLimit,
    InvalidRoute,
    DuplicateRoute,
    ResourceLimitExceeded,
};

Q_DECLARE_METATYPE(ManifestErrorCode)

struct ManifestError
{
    ManifestErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const ManifestError &, const ManifestError &) = default;
};

[[nodiscard]] QString manifestErrorCodeName(ManifestErrorCode code);
