#pragma once

#include "CapabilityBroker.h"

#include <QByteArray>

#include <optional>

struct SelectedFile {
    QString name;
    QByteArray content;
};

class FileDialogBackend
{
public:
    virtual ~FileDialogBackend() = default;
    [[nodiscard]] virtual std::optional<SelectedFile> openFile(qint64 maximumBytes) = 0;
};

class QtFileDialogBackend final : public FileDialogBackend
{
public:
    [[nodiscard]] std::optional<SelectedFile> openFile(qint64 maximumBytes) override;
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
};
