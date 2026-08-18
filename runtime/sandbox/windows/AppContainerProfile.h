#pragma once

#include "SandboxError.h"

#include <QString>

#include <qt_windows.h>

#include <optional>

class AppContainerProfile final
{
public:
    AppContainerProfile() = default;
    ~AppContainerProfile();

    AppContainerProfile(const AppContainerProfile &) = delete;
    AppContainerProfile &operator=(const AppContainerProfile &) = delete;
    AppContainerProfile(AppContainerProfile &&other) noexcept;
    AppContainerProfile &operator=(AppContainerProfile &&other) noexcept;

    static std::optional<QString> deterministicName(const QString &appId);
    static SandboxValueResult<AppContainerProfile> createOrOpen(
        const QString &appId);

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool wasCreated() const noexcept;
    [[nodiscard]] const QString &name() const noexcept;
    [[nodiscard]] PSID sid() const noexcept;
    [[nodiscard]] SandboxValueResult<QString> sidString() const;

private:
    AppContainerProfile(QString name, PSID sid, bool created) noexcept;
    void reset() noexcept;

    QString name_;
    PSID sid_ = nullptr;
    bool created_ = false;
};
