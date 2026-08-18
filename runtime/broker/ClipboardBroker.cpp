#include "ClipboardBroker.h"

#include "UserGestureGrantStore.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
[[nodiscard]] bool clipboardTextFits(const QString &text, const qint64 maximumBytes)
{
    return text.size() <= (maximumBytes / 2) - 1;
}

[[nodiscard]] BrokerResult clipboardFailure(const ClipboardStatus status)
{
    if (status == ClipboardStatus::TooLarge) {
        return BrokerResult::failure(QStringLiteral("clipboard.too_large"),
                                     QStringLiteral("Clipboard content is too large."));
    }
    return BrokerResult::failure(QStringLiteral("clipboard.failed"),
                                 QStringLiteral("Clipboard is unavailable."));
}
}

ClipboardReadResult ClipboardReadResult::error(const ClipboardStatus status)
{
    return ClipboardReadResult{status, {}};
}

ClipboardReadResult QtClipboardBackend::readText(const qint64 maximumBytes)
{
#ifdef Q_OS_WIN
    if (maximumBytes < 2 || OpenClipboard(nullptr) == FALSE) {
        return ClipboardReadResult::error(ClipboardStatus::Unavailable);
    }
    struct ClipboardCloser final
    {
        ~ClipboardCloser() { CloseClipboard(); }
    } closer;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) {
        return ClipboardReadResult{ClipboardStatus::Success, {}};
    }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        return ClipboardReadResult::error(ClipboardStatus::Unavailable);
    }
    const SIZE_T dataBytes = GlobalSize(data);
    if (dataBytes < sizeof(wchar_t)
        || dataBytes > static_cast<SIZE_T>(maximumBytes)
        || (dataBytes % sizeof(wchar_t)) != 0U) {
        return ClipboardReadResult::error(ClipboardStatus::TooLarge);
    }
    const auto *characters = static_cast<const wchar_t *>(GlobalLock(data));
    if (characters == nullptr) {
        return ClipboardReadResult::error(ClipboardStatus::Unavailable);
    }
    struct GlobalUnlocker final
    {
        HANDLE value;
        ~GlobalUnlocker() { GlobalUnlock(value); }
    } unlocker{data};
    const qsizetype capacity = static_cast<qsizetype>(dataBytes / sizeof(wchar_t));
    qsizetype length = 0;
    while (length < capacity && characters[length] != L'\0') {
        ++length;
    }
    if (length == capacity) {
        return ClipboardReadResult::error(ClipboardStatus::TooLarge);
    }
    return ClipboardReadResult{ClipboardStatus::Success,
                               QString::fromWCharArray(characters, length)};
#else
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return ClipboardReadResult::error(ClipboardStatus::Unavailable);
    }
    const QString text = clipboard->text();
    return clipboardTextFits(text, maximumBytes)
               ? ClipboardReadResult{ClipboardStatus::Success, text}
               : ClipboardReadResult::error(ClipboardStatus::TooLarge);
#endif
}

ClipboardStatus QtClipboardBackend::writeText(const QString &text,
                                              const qint64 maximumBytes)
{
    if (!clipboardTextFits(text, maximumBytes)) {
        return ClipboardStatus::TooLarge;
    }
#ifdef Q_OS_WIN
    if (OpenClipboard(nullptr) == FALSE) {
        return ClipboardStatus::Unavailable;
    }
    struct ClipboardCloser final
    {
        ~ClipboardCloser() { CloseClipboard(); }
    } closer;
    if (EmptyClipboard() == FALSE) {
        return ClipboardStatus::Unavailable;
    }
    const SIZE_T bytes = static_cast<SIZE_T>(text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return ClipboardStatus::Unavailable;
    }
    auto *destination = static_cast<wchar_t *>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return ClipboardStatus::Unavailable;
    }
    if (!text.isEmpty()) {
        memcpy(destination, text.utf16(), static_cast<size_t>(text.size()) * sizeof(wchar_t));
    }
    destination[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        return ClipboardStatus::Unavailable;
    }
    return ClipboardStatus::Success;
#else
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return ClipboardStatus::Unavailable;
    }
    clipboard->setText(text);
    return ClipboardStatus::Success;
#endif
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
        const ClipboardReadResult result = backend_.readText(maximumClipboardBytes());
        if (result.status != ClipboardStatus::Success) {
            return clipboardFailure(result.status);
        }
        if (!clipboardTextFits(result.text, maximumClipboardBytes())) {
            return clipboardFailure(ClipboardStatus::TooLarge);
        }
        return BrokerResult::success(QJsonObject{{QStringLiteral("text"), result.text}});
    }

    if (operation != QStringLiteral("write") || payload.size() != 1
        || !payload.value(QStringLiteral("text")).isString()) {
        return BrokerResult::failure(QStringLiteral("clipboard.invalid_request"),
                                     QStringLiteral("Clipboard request is invalid."));
    }
    if (!policy_.write) {
        return BrokerResult::failure(QStringLiteral("capability.denied"),
                                     QStringLiteral("Capability is not permitted."));
    }
    const QString text = payload.value(QStringLiteral("text")).toString();
    if (!clipboardTextFits(text, maximumClipboardBytes())) {
        return clipboardFailure(ClipboardStatus::TooLarge);
    }
    const ClipboardStatus status = backend_.writeText(text, maximumClipboardBytes());
    if (status != ClipboardStatus::Success) {
        return clipboardFailure(status);
    }
    return BrokerResult::success();
}
