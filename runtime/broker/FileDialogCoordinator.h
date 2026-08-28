#pragma once

#include "FileBroker.h"

#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

class FileDialogCoordinatorControl;

class FileDialogOperationToken final
{
public:
    FileDialogOperationToken(const FileDialogOperationToken &) = default;
    FileDialogOperationToken &operator=(const FileDialogOperationToken &) = default;
    FileDialogOperationToken(FileDialogOperationToken &&) noexcept = default;
    FileDialogOperationToken &operator=(FileDialogOperationToken &&) noexcept = default;

    [[nodiscard]] bool operator==(
        const FileDialogOperationToken &) const noexcept = default;

private:
    friend class FileDialogCoordinator;
    friend class FileDialogCoordinatorControl;
    explicit FileDialogOperationToken(quint64 value) noexcept;

    quint64 value_ = 0;
};

class FileDialogCancellationHandle final
{
public:
    FileDialogCancellationHandle(const FileDialogCancellationHandle &) = default;
    FileDialogCancellationHandle &operator=(
        const FileDialogCancellationHandle &) = default;
    FileDialogCancellationHandle(FileDialogCancellationHandle &&) noexcept = default;
    FileDialogCancellationHandle &operator=(
        FileDialogCancellationHandle &&) noexcept = default;

    [[nodiscard]] bool cancel() const noexcept;

private:
    friend class FileDialogCoordinatorControl;
    FileDialogCancellationHandle(
        std::weak_ptr<FileDialogCoordinatorControl> control,
        FileDialogOperationToken token) noexcept;

    std::weak_ptr<FileDialogCoordinatorControl> control_;
    FileDialogOperationToken token_;
};

struct FileDialogOperation final
{
    FileDialogOperationToken token;
    FileDialogCancellationHandle cancellation;
};

class FileDialogOpenRequest final
{
public:
    using OwnerPredicate = std::function<bool()>;

    FileDialogOpenRequest(const PreparedFileRequest &prepared,
                          OwnerPredicate ownerPredicate);

private:
    friend class FileDialogCoordinatorControl;

    qint64 maximumBytes_ = 0;
    OwnerPredicate ownerPredicate_;
};

using FileDialogCompletion =
    std::function<void(const FileDialogOperationToken &, FileDialogSelection)>;

class FileDialogCoordinator final
{
public:
    FileDialogCoordinator();
    ~FileDialogCoordinator();
    FileDialogCoordinator(const FileDialogCoordinator &) = delete;
    FileDialogCoordinator &operator=(const FileDialogCoordinator &) = delete;
    FileDialogCoordinator(FileDialogCoordinator &&) = delete;
    FileDialogCoordinator &operator=(FileDialogCoordinator &&) = delete;

    [[nodiscard]] std::optional<FileDialogOperation> openAsync(
        FileDialogOpenRequest request,
        FileDialogCompletion completion);
    [[nodiscard]] bool cancel(const FileDialogOperationToken &token) noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<FileDialogCoordinatorControl> control_;
};
