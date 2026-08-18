#include "FileBroker.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

std::optional<SelectedFile> QtFileDialogBackend::openFile(const qint64 maximumBytes)
{
    const QString path = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Open file"));
    if (path.isEmpty()) {
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximumBytes) {
        return SelectedFile{QFileInfo(path).fileName(), QByteArray(maximumBytes + 1, '\0')};
    }
    const QByteArray content = file.read(maximumBytes + 1);
    return SelectedFile{QFileInfo(path).fileName(), content};
}

FileBroker::FileBroker(EffectiveFilePolicy policy, FileDialogBackend &backend)
    : policy_(policy), backend_(backend)
{
}

BrokerResult FileBroker::invoke(const QString &operation,
                                const QJsonObject &payload,
                                const HostRequestContext &context)
{
    Q_UNUSED(context)
    if (operation != QStringLiteral("open") || !payload.isEmpty()) {
        return BrokerResult::failure(QStringLiteral("file.invalid_request"),
                                     QStringLiteral("File request is invalid."));
    }
    if (!policy_.open) {
        return BrokerResult::failure(QStringLiteral("capability.denied"),
                                     QStringLiteral("Capability is not permitted."));
    }
    const std::optional<SelectedFile> selection = backend_.openFile(policy_.maximumBytes);
    if (!selection.has_value()) {
        return BrokerResult::failure(QStringLiteral("file.cancelled"),
                                     QStringLiteral("No file was selected."));
    }
    if (selection->content.size() > policy_.maximumBytes) {
        return BrokerResult::failure(QStringLiteral("file.too_large"),
                                     QStringLiteral("Selected file is too large."));
    }
    return BrokerResult::success(
        QJsonObject{{QStringLiteral("name"), selection->name},
                    {QStringLiteral("size"), selection->content.size()},
                    {QStringLiteral("contentBase64"),
                     QString::fromLatin1(selection->content.toBase64(QByteArray::Base64Encoding))}});
}
