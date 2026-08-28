#pragma once

#include "CapabilityBroker.h"

#include <QByteArray>
#include <QIODevice>
#include <QMutex>

#include <memory>
#include <optional>

enum class FileDialogStatus {
    Opened,
    Cancelled,
    Failed,
    TooLarge,
    Busy,
};

class PreparedFileRequest final
{
public:
    ~PreparedFileRequest() = default;
    PreparedFileRequest(const PreparedFileRequest &) = delete;
    PreparedFileRequest &operator=(const PreparedFileRequest &) = delete;
    PreparedFileRequest(PreparedFileRequest &&) noexcept = default;
    PreparedFileRequest &operator=(PreparedFileRequest &&) noexcept = default;

    [[nodiscard]] const QString &operation() const noexcept;
    [[nodiscard]] const QJsonObject &payload() const noexcept;
    [[nodiscard]] const QString &appIdentity() const noexcept;
    [[nodiscard]] const QString &requestId() const noexcept;
    [[nodiscard]] qint64 maximumBytes() const noexcept;

private:
    friend class FileBroker;
    PreparedFileRequest(QString operation,
                        QJsonObject payload,
                        QString appIdentity,
                        QString requestId,
                        qint64 maximumBytes);

    QString operation_;
    QJsonObject payload_;
    QString appIdentity_;
    QString requestId_;
    qint64 maximumBytes_ = 0;
};

struct PreparedFileRequestResult final
{
    PreparedFileRequestResult() = default;
    PreparedFileRequestResult(const PreparedFileRequestResult &) = delete;
    PreparedFileRequestResult &operator=(const PreparedFileRequestResult &) = delete;
    PreparedFileRequestResult(PreparedFileRequestResult &&) noexcept = default;
    PreparedFileRequestResult &operator=(PreparedFileRequestResult &&) noexcept = default;

    std::optional<PreparedFileRequest> request;
    BrokerResult rejection;
};

// Value-only attestation produced while the selected stable file is still
// owned by the dialog lane. No path, handle, or QIODevice crosses this seam.
struct FileDialogSelection final
{
    FileDialogStatus status = FileDialogStatus::Failed;
    QString name;
    qint64 declaredSize = 0;
    QByteArray contentBase64;
    qint64 approvedMaximumBytes = 0;
    QByteArray identityBeforeRead;
    QByteArray identityAfterRead;
};

struct FileDialogResult {
    FileDialogStatus status = FileDialogStatus::Failed;
    QString name;
    qint64 size = 0;
    std::unique_ptr<QIODevice> stream;
    QByteArray identityBeforeRead;
    QByteArray identityAfterRead;

    [[nodiscard]] static FileDialogResult opened(QString name,
                                                 qint64 size,
                                                 std::unique_ptr<QIODevice> stream,
                                                 QByteArray identityBeforeRead = {},
                                                 QByteArray identityAfterRead = {});
    [[nodiscard]] static FileDialogResult error(FileDialogStatus status);
};

class FileDialogBackend
{
public:
    virtual ~FileDialogBackend() = default;
    [[nodiscard]] virtual FileDialogResult openFile(qint64 maximumBytes) = 0;
};

class QtFileDialogBackend final : public FileDialogBackend
{
public:
    [[nodiscard]] FileDialogResult openFile(qint64 maximumBytes) override;
};

class FileBroker final : public CapabilityService
{
public:
    FileBroker(EffectiveFilePolicy policy, FileDialogBackend &backend);

    [[nodiscard]] PreparedFileRequestResult prepareFileRequest(
        const QString &operation,
        const QJsonObject &payload,
        const HostRequestContext &context) const;
    [[nodiscard]] BrokerResult completeFileRequest(
        const PreparedFileRequest &request,
        const FileDialogSelection &selection) const;

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveFilePolicy policy_;
    FileDialogBackend &backend_;
    QMutex dialogMutex_;
};
