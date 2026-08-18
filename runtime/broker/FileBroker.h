#pragma once

#include "CapabilityBroker.h"

#include <QIODevice>
#include <QMutex>

#include <memory>

enum class FileDialogStatus {
    Opened,
    Cancelled,
    Failed,
    TooLarge,
};

struct FileDialogResult {
    FileDialogStatus status = FileDialogStatus::Failed;
    QString name;
    qint64 size = 0;
    std::unique_ptr<QIODevice> stream;

    [[nodiscard]] static FileDialogResult opened(QString name,
                                                 qint64 size,
                                                 std::unique_ptr<QIODevice> stream);
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

    [[nodiscard]] BrokerResult invoke(const QString &operation,
                                      const QJsonObject &payload,
                                      const HostRequestContext &context) override;

private:
    EffectiveFilePolicy policy_;
    FileDialogBackend &backend_;
    QMutex dialogMutex_;
};
