#include "ClipboardBroker.h"

#include "UserGestureGrantStore.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QSet>

QString QtClipboardBackend::readText()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    return clipboard == nullptr ? QString{} : clipboard->text();
}

bool QtClipboardBackend::writeText(const QString &text)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setText(text);
    return true;
}

ClipboardBroker::ClipboardBroker(EffectiveClipboardPolicy policy,
                                 ClipboardBackend &backend,
                                 UserGestureGrantStore &grants)
    : policy_(policy), backend_(backend), grants_(grants)
{
}

BrokerResult ClipboardBroker::invoke(const QString &operation,
                                     const QJsonObject &payload,
                                     const HostRequestContext &context)
{
    if (operation == QStringLiteral("read")) {
        if (!payload.isEmpty()) {
            return BrokerResult::failure(QStringLiteral("clipboard.invalid_request"),
                                         QStringLiteral("Clipboard request is invalid."));
        }
        if (!policy_.readWithUserGesture) {
            return BrokerResult::failure(QStringLiteral("capability.denied"),
                                         QStringLiteral("Capability is not permitted."));
        }
        if (!grants_.consume(context.appIdentity, context.requestId)) {
            return BrokerResult::failure(QStringLiteral("clipboard.gesture_required"),
                                         QStringLiteral("A user gesture is required."));
        }
        return BrokerResult::success(
            QJsonObject{{QStringLiteral("text"), backend_.readText()}});
    }

    if (operation != QStringLiteral("write") || payload.size() != 1
        || !payload.value(QStringLiteral("text")).isString()
        || payload.value(QStringLiteral("text")).toString().size() > 65536) {
        return BrokerResult::failure(QStringLiteral("clipboard.invalid_request"),
                                     QStringLiteral("Clipboard request is invalid."));
    }
    if (!policy_.write) {
        return BrokerResult::failure(QStringLiteral("capability.denied"),
                                     QStringLiteral("Capability is not permitted."));
    }
    if (!backend_.writeText(payload.value(QStringLiteral("text")).toString())) {
        return BrokerResult::failure(QStringLiteral("clipboard.failed"),
                                     QStringLiteral("Clipboard is unavailable."));
    }
    return BrokerResult::success();
}
