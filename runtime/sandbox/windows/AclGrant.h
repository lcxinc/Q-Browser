#pragma once

#include "SandboxError.h"

#include <QByteArray>
#include <QString>

#include <qt_windows.h>

#include <optional>

enum class SandboxPathAccess {
    ReadOnly,
    ReadExecute,
    ReadWrite,
};

class AclGrant final
{
public:
    AclGrant() = default;
    ~AclGrant();

    AclGrant(const AclGrant &) = delete;
    AclGrant &operator=(const AclGrant &) = delete;
    AclGrant(AclGrant &&other) noexcept;
    AclGrant &operator=(AclGrant &&other) noexcept;

    static SandboxValueResult<AclGrant> apply(const QString &path,
                                              PSID appContainerSid,
                                              SandboxPathAccess access,
                                              bool inheritToChildren);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const QString &finalPath() const noexcept;
    bool restore() noexcept;

private:
    AclGrant(HANDLE target,
             QByteArray originalSecurity,
             QString finalPath) noexcept;
    void close() noexcept;

    HANDLE target_ = INVALID_HANDLE_VALUE;
    QByteArray originalSecurity_;
    QString finalPath_;
};
