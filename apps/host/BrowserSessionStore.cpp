#include "BrowserSessionStore.h"

#include "HostOwnedStateDirectory.h"

#ifdef Q_OS_WIN
#include "WindowsStableIo.h"
#endif

#include <QChar>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <utility>

namespace
{
constexpr auto SessionLeaf = "browser-session.json";
constexpr auto CorruptLeaf = "browser-session.json.corrupt";
constexpr auto SessionTemporaryLeaf = "browser-session.json.tmp";
constexpr auto CorruptTemporaryLeaf = "browser-session.json.corrupt.tmp";
constexpr quint64 MaximumSessionBytes = 16U * 1024U * 1024U;

BrowserSessionLoadResult ioFailure()
{
    return {BrowserSessionLoadStatus::IoFailure,
            std::nullopt,
            QStringLiteral("host.session.io_failed")};
}

BrowserSessionLoadResult corruptResult()
{
    return {BrowserSessionLoadStatus::Corrupt,
            std::nullopt,
            QStringLiteral("host.session.corrupt")};
}

BrowserSessionSaveResult saveIoFailure()
{
    return {BrowserSessionSaveStatus::IoFailure,
            QStringLiteral("host.session.save_failed")};
}

bool isBidiControl(const char16_t value) noexcept
{
    return value == 0x061c || value == 0x200e || value == 0x200f
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

std::optional<QString> sanitizedTitle(const QString &title)
{
    QString result;
    result.reserve(BrowserTabModel::MaxTitleCodeUnits);
    bool prefixComplete = false;
    for (qsizetype index = 0; index < title.size(); ++index) {
        const QChar value = title.at(index);
        if (value.isHighSurrogate()) {
            if (index + 1 >= title.size()
                || !title.at(index + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            if (!prefixComplete
                && result.size() + 2 <= BrowserTabModel::MaxTitleCodeUnits) {
                result.append(value);
                result.append(title.at(index + 1));
                prefixComplete = result.size()
                    == BrowserTabModel::MaxTitleCodeUnits;
            } else if (!prefixComplete) {
                prefixComplete = true;
            }
            ++index;
            continue;
        }
        if (value.isLowSurrogate()) return std::nullopt;
        if (value.category() == QChar::Other_Control
            || isBidiControl(value.unicode())) {
            continue;
        }
        if (!prefixComplete) {
            result.append(value);
            prefixComplete = result.size()
                == BrowserTabModel::MaxTitleCodeUnits;
        }
    }
    return result;
}

bool isValidId(const QString &id) noexcept
{
    if (id.size() != 32) return false;
    return std::ranges::all_of(id, [](const QChar value) {
        return (value >= u'0' && value <= u'9')
            || (value >= u'a' && value <= u'f');
    });
}

bool isValidKind(const BrowserTabKind kind) noexcept
{
    switch (kind) {
    case BrowserTabKind::Host:
    case BrowserTabKind::App:
    case BrowserTabKind::Web:
    case BrowserTabKind::TrustedError:
        return true;
    }
    return false;
}

std::optional<QStringView> kindText(const BrowserTabKind kind) noexcept
{
    switch (kind) {
    case BrowserTabKind::Host:
        return u"host";
    case BrowserTabKind::App:
        return u"app";
    case BrowserTabKind::Web:
        return u"web";
    case BrowserTabKind::TrustedError:
        return u"trusted-error";
    }
    return std::nullopt;
}

std::optional<BrowserTabKind> parseKind(const QString &kind) noexcept
{
    if (kind == QLatin1String("host")) return BrowserTabKind::Host;
    if (kind == QLatin1String("app")) return BrowserTabKind::App;
    if (kind == QLatin1String("web")) return BrowserTabKind::Web;
    if (kind == QLatin1String("trusted-error")) {
        return BrowserTabKind::TrustedError;
    }
    return std::nullopt;
}

std::optional<BrowserAddress> parseLogicalAddress(const QString &text)
{
    BrowserAddress parsed = BrowserAddress::parse(text);
    if (text.startsWith(QLatin1String("app://"))) {
        constexpr qsizetype authorityStart = 6;
        qsizetype authorityEnd = text.size();
        for (qsizetype index = authorityStart; index < text.size(); ++index) {
            const QChar value = text.at(index);
            if (value == u'/' || value == u'?' || value == u'#') {
                authorityEnd = index;
                break;
            }
        }
        parsed = BrowserAddress::parse(
            text, QStringView(text).sliced(
                      authorityStart, authorityEnd - authorityStart));
    }
    return parsed.isValid() && parsed.canonical() == text
        ? std::optional<BrowserAddress>(std::move(parsed))
        : std::nullopt;
}

std::optional<BrowserWindowSnapshot> normalizedSnapshot(
    const BrowserWindowSnapshot &snapshot,
    const bool requireCanonicalTitles)
{
    const QRect geometry = snapshot.geometry;
    if (geometry.x() < -1'000'000 || geometry.x() > 1'000'000
        || geometry.y() < -1'000'000 || geometry.y() > 1'000'000
        || geometry.width() < 320 || geometry.width() > 32'768
        || geometry.height() < 320 || geometry.height() > 32'768
        || snapshot.tabs.isEmpty()
        || static_cast<int>(snapshot.tabs.size())
            > BrowserTabModel::MaxOpenTabs
        || !isValidId(snapshot.activeTabId)) {
        return std::nullopt;
    }

    BrowserWindowSnapshot normalized = snapshot;
    QSet<QString> ids;
    bool activeFound = false;
    for (BrowserTabSnapshot &tab : normalized.tabs) {
        if (!isValidId(tab.id) || ids.contains(tab.id)
            || !isValidKind(tab.kind) || tab.history.isEmpty()
            || static_cast<int>(tab.history.size())
                > BrowserTabModel::MaxHistoryEntries
            || tab.historyIndex < 0
            || tab.historyIndex >= static_cast<int>(tab.history.size())
            || tab.history.at(tab.historyIndex) != tab.address) {
            return std::nullopt;
        }
        ids.insert(tab.id);
        activeFound = activeFound || tab.id == normalized.activeTabId;

        const std::optional<QString> title = sanitizedTitle(tab.title);
        if (!title.has_value()
            || (requireCanonicalTitles && *title != tab.title)) {
            return std::nullopt;
        }
        tab.title = *title;
        if (!parseLogicalAddress(tab.address).has_value()) {
            return std::nullopt;
        }
        for (const QString &entry : tab.history) {
            if (!parseLogicalAddress(entry).has_value()) {
                return std::nullopt;
            }
        }
    }
    return activeFound
        ? std::optional<BrowserWindowSnapshot>(std::move(normalized))
        : std::nullopt;
}

void appendJsonString(QByteArray &bytes, const QStringView value)
{
    QString escaped = value.toString();
    escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'"', QStringLiteral("\\\""));
    bytes.append('"');
    bytes.append(escaped.toUtf8());
    bytes.append('"');
}

std::optional<QByteArray> serializedSnapshot(
    const BrowserWindowSnapshot &snapshot)
{
    QByteArray bytes;
    bytes.reserve(1024);
    bytes.append("{\"version\":1,\"window\":{\"x\":");
    bytes.append(QByteArray::number(snapshot.geometry.x()));
    bytes.append(",\"y\":");
    bytes.append(QByteArray::number(snapshot.geometry.y()));
    bytes.append(",\"width\":");
    bytes.append(QByteArray::number(snapshot.geometry.width()));
    bytes.append(",\"height\":");
    bytes.append(QByteArray::number(snapshot.geometry.height()));
    bytes.append("},\"activeTabId\":");
    appendJsonString(bytes, snapshot.activeTabId);
    bytes.append(",\"tabs\":[");
    for (qsizetype tabIndex = 0; tabIndex < snapshot.tabs.size(); ++tabIndex) {
        if (tabIndex != 0) bytes.append(',');
        const BrowserTabSnapshot &tab = snapshot.tabs.at(tabIndex);
        const std::optional<QStringView> persistedKind = kindText(tab.kind);
        if (!persistedKind.has_value()) return std::nullopt;
        bytes.append("{\"id\":");
        appendJsonString(bytes, tab.id);
        bytes.append(",\"kind\":");
        appendJsonString(bytes, *persistedKind);
        bytes.append(",\"title\":");
        appendJsonString(bytes, tab.title);
        bytes.append(",\"address\":");
        appendJsonString(bytes, tab.address);
        bytes.append(",\"history\":[");
        for (qsizetype historyIndex = 0;
             historyIndex < tab.history.size(); ++historyIndex) {
            if (historyIndex != 0) bytes.append(',');
            appendJsonString(bytes, tab.history.at(historyIndex));
        }
        bytes.append("],\"historyIndex\":");
        bytes.append(QByteArray::number(tab.historyIndex));
        bytes.append('}');
    }
    bytes.append("]}\n");
    return bytes;
}

template<size_t Size>
bool hasExactKeys(const QJsonObject &object,
                  const std::array<QStringView, Size> &keys)
{
    if (object.size() != static_cast<qsizetype>(Size)) return false;
    return std::ranges::all_of(keys, [&object](const QStringView key) {
        return object.contains(key);
    });
}

std::optional<int> exactInteger(const QJsonValue &value,
                                const int minimum,
                                const int maximum)
{
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number
        || number < static_cast<double>(minimum)
        || number > static_cast<double>(maximum)) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

std::optional<BrowserWindowSnapshot> parseSnapshot(const QByteArray &bytes)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    constexpr std::array rootKeys{
        QStringView(u"version"),
        QStringView(u"window"),
        QStringView(u"activeTabId"),
        QStringView(u"tabs"),
    };
    if (!hasExactKeys(root, rootKeys)
        || exactInteger(root.value(QLatin1String("version")), 1, 1)
            != std::optional<int>(1)
        || !root.value(QLatin1String("window")).isObject()
        || !root.value(QLatin1String("activeTabId")).isString()
        || !root.value(QLatin1String("tabs")).isArray()) {
        return std::nullopt;
    }

    const QJsonObject window = root.value(QLatin1String("window")).toObject();
    constexpr std::array windowKeys{
        QStringView(u"x"),
        QStringView(u"y"),
        QStringView(u"width"),
        QStringView(u"height"),
    };
    if (!hasExactKeys(window, windowKeys)) return std::nullopt;
    const std::optional<int> x = exactInteger(
        window.value(QLatin1String("x")), -1'000'000, 1'000'000);
    const std::optional<int> y = exactInteger(
        window.value(QLatin1String("y")), -1'000'000, 1'000'000);
    const std::optional<int> width = exactInteger(
        window.value(QLatin1String("width")), 320, 32'768);
    const std::optional<int> height = exactInteger(
        window.value(QLatin1String("height")), 320, 32'768);
    if (!x || !y || !width || !height) return std::nullopt;

    const QJsonArray tabs = root.value(QLatin1String("tabs")).toArray();
    if (tabs.isEmpty()
        || tabs.size() > static_cast<qsizetype>(BrowserTabModel::MaxOpenTabs)) {
        return std::nullopt;
    }
    BrowserWindowSnapshot snapshot;
    snapshot.geometry = QRect(*x, *y, *width, *height);
    snapshot.activeTabId = root.value(
        QLatin1String("activeTabId")).toString();
    snapshot.tabs.reserve(tabs.size());

    constexpr std::array tabKeys{
        QStringView(u"id"),
        QStringView(u"kind"),
        QStringView(u"title"),
        QStringView(u"address"),
        QStringView(u"history"),
        QStringView(u"historyIndex"),
    };
    for (const QJsonValue &tabValue : tabs) {
        if (!tabValue.isObject()) return std::nullopt;
        const QJsonObject object = tabValue.toObject();
        if (!hasExactKeys(object, tabKeys)
            || !object.value(QLatin1String("id")).isString()
            || !object.value(QLatin1String("kind")).isString()
            || !object.value(QLatin1String("title")).isString()
            || !object.value(QLatin1String("address")).isString()
            || !object.value(QLatin1String("history")).isArray()) {
            return std::nullopt;
        }
        const std::optional<BrowserTabKind> kind = parseKind(
            object.value(QLatin1String("kind")).toString());
        const QJsonArray historyValues = object.value(
            QLatin1String("history")).toArray();
        const std::optional<int> historyIndex = exactInteger(
            object.value(QLatin1String("historyIndex")),
            0,
            BrowserTabModel::MaxHistoryEntries - 1);
        if (!kind || !historyIndex || historyValues.isEmpty()
            || historyValues.size()
                > static_cast<qsizetype>(BrowserTabModel::MaxHistoryEntries)) {
            return std::nullopt;
        }

        BrowserTabSnapshot tab;
        tab.id = object.value(QLatin1String("id")).toString();
        tab.kind = *kind;
        tab.title = object.value(QLatin1String("title")).toString();
        tab.address = object.value(QLatin1String("address")).toString();
        tab.history.reserve(historyValues.size());
        for (const QJsonValue &historyValue : historyValues) {
            if (!historyValue.isString()) return std::nullopt;
            tab.history.append(historyValue.toString());
        }
        tab.historyIndex = *historyIndex;
        snapshot.tabs.append(std::move(tab));
    }

    std::optional<BrowserWindowSnapshot> normalized = normalizedSnapshot(
        snapshot, true);
    if (!normalized.has_value()) return std::nullopt;
    const std::optional<QByteArray> canonical = serializedSnapshot(*normalized);
    return canonical.has_value() && *canonical == bytes
        ? normalized
        : std::nullopt;
}

#ifdef Q_OS_WIN
using qbrowser_archive_detail::WindowsStableDirectoryTree;
using qbrowser_archive_detail::WindowsStableFile;
using qbrowser_archive_detail::WindowsStableFileOpenStatus;
using qbrowser_archive_detail::WindowsStableReadStatus;

bool stableRoot(const std::shared_ptr<const HostOwnedStateDirectory> &authority,
                const WindowsStableDirectoryTree &tree)
{
    return authority && authority->revalidate() && tree.isStable()
        && tree.isSameRootIdentityAt(authority->canonicalPath())
        && tree.rootHasRestrictedTrustAcl();
}

bool openStableRoot(
    const std::shared_ptr<const HostOwnedStateDirectory> &authority,
    WindowsStableDirectoryTree &tree)
{
    return authority && authority->revalidate()
        && tree.openSharedRoot(authority->canonicalPath())
        && stableRoot(authority, tree);
}

bool stableLeaf(const WindowsStableFile &file,
                const QString &path,
                const WindowsStableDirectoryTree &tree,
                const std::shared_ptr<const HostOwnedStateDirectory> &authority)
{
    return stableRoot(authority, tree) && file.isOpen()
        && file.isStableWithin(tree) && file.isSameIdentityAt(path)
        && file.hasSingleLink() && file.hasRestrictedTrustAcl();
}

void invokePrimaryReadHook(const QString &path)
{
#ifdef Q_BROWSER_HOST_TESTING
    const auto hooks = qbrowser_host_testing::browserSessionStoreTestHooks();
    if (hooks.afterPrimaryLeafOpenedBeforeRead) {
        hooks.afterPrimaryLeafOpenedBeforeRead(path);
    }
#else
    Q_UNUSED(path);
#endif
}

void invokeCorruptUseHook(const QString &path)
{
#ifdef Q_BROWSER_HOST_TESTING
    const auto hooks = qbrowser_host_testing::browserSessionStoreTestHooks();
    if (hooks.afterCorruptLeafOpenedBeforeUse) {
        hooks.afterCorruptLeafOpenedBeforeUse(path);
    }
#else
    Q_UNUSED(path);
#endif
}

bool removeSafeTemporary(
    const QString &path,
    const WindowsStableDirectoryTree &tree,
    const std::shared_ptr<const HostOwnedStateDirectory> &authority)
{
    WindowsStableFile temporary;
    const WindowsStableFileOpenStatus status = temporary.openReadDeleteLocked(
        path, tree);
    if (status == WindowsStableFileOpenStatus::Missing) {
        return stableRoot(authority, tree);
    }
    return status == WindowsStableFileOpenStatus::Opened
        && stableLeaf(temporary, path, tree, authority)
        && temporary.deleteOwned() && stableRoot(authority, tree);
}

bool verifyPublishedBytes(
    const QString &path,
    const QByteArray &expected,
    const WindowsStableDirectoryTree &tree,
    const std::shared_ptr<const HostOwnedStateDirectory> &authority,
    const bool corruptLeaf)
{
    WindowsStableFile published;
    if (published.openReadDeleteLocked(path, tree)
        != WindowsStableFileOpenStatus::Opened
        || !stableLeaf(published, path, tree, authority)) {
        return false;
    }
    if (corruptLeaf) invokeCorruptUseHook(path);
    if (!stableLeaf(published, path, tree, authority)) return false;
    QByteArray readBack;
    if (published.readBoundedIncludingEmpty(MaximumSessionBytes, readBack)
            != WindowsStableReadStatus::Read
        || readBack != expected) {
        return false;
    }
    return stableLeaf(published, path, tree, authority);
}

bool writeNewCorruptCopy(
    const QString &corruptPath,
    const QString &temporaryPath,
    const QByteArray &bytes,
    const WindowsStableDirectoryTree &tree,
    const std::shared_ptr<const HostOwnedStateDirectory> &authority)
{
    if (static_cast<quint64>(bytes.size()) > MaximumSessionBytes
        || !removeSafeTemporary(temporaryPath, tree, authority)) {
        return false;
    }
    WindowsStableFile temporary;
    if (!temporary.createRestrictedOutput(temporaryPath, tree)
        || !temporary.writeAll(bytes.constData(), static_cast<size_t>(bytes.size()))
        || !temporary.flush()
        || !stableLeaf(temporary, temporaryPath, tree, authority)) {
        if (temporary.isOpen()) (void)temporary.deleteOwned();
        return false;
    }
    QByteArray readBack;
    if (temporary.readBoundedIncludingEmpty(MaximumSessionBytes, readBack)
            != WindowsStableReadStatus::Read
        || readBack != bytes || !stableLeaf(
               temporary, temporaryPath, tree, authority)
        || !temporary.publishAtomic(corruptPath, tree, false)) {
        if (temporary.isOpen()) (void)temporary.deleteOwned();
        return false;
    }
    return verifyPublishedBytes(corruptPath, bytes, tree, authority, true);
}

bool preserveAndRemoveCorrupt(
    WindowsStableFile &primary,
    const QString &primaryPath,
    const std::optional<QByteArray> &bytes,
    const WindowsStableDirectoryTree &tree,
    const std::shared_ptr<const HostOwnedStateDirectory> &authority)
{
    const QDir root(authority->canonicalPath());
    const QString corruptPath = root.filePath(
        QString::fromLatin1(CorruptLeaf));
    const QString temporaryPath = root.filePath(
        QString::fromLatin1(CorruptTemporaryLeaf));

    WindowsStableFile existingCorrupt;
    const WindowsStableFileOpenStatus corruptStatus =
        existingCorrupt.openReadDeleteLocked(corruptPath, tree);
    if (corruptStatus == WindowsStableFileOpenStatus::Failure) return false;
    if (corruptStatus == WindowsStableFileOpenStatus::Opened) {
        if (!stableLeaf(existingCorrupt, corruptPath, tree, authority)) {
            return false;
        }
        invokeCorruptUseHook(corruptPath);
        if (!stableLeaf(existingCorrupt, corruptPath, tree, authority)) {
            return false;
        }
        QByteArray ignored;
        if (existingCorrupt.readBoundedIncludingEmpty(MaximumSessionBytes, ignored)
                != WindowsStableReadStatus::Read
            || !stableLeaf(existingCorrupt, corruptPath, tree, authority)) {
            return false;
        }
    } else if (bytes.has_value()
               && !writeNewCorruptCopy(
                   corruptPath, temporaryPath, *bytes, tree, authority)) {
        return false;
    }

    if (!stableLeaf(primary, primaryPath, tree, authority)
        || !primary.deleteOwned() || !stableRoot(authority, tree)) {
        return false;
    }
    WindowsStableFile absenceCheck;
    return absenceCheck.openReadDeleteLocked(primaryPath, tree)
        == WindowsStableFileOpenStatus::Missing;
}

bool saveAtomically(
    const QByteArray &bytes,
    const QString &targetPath,
    const QString &temporaryPath,
    const WindowsStableDirectoryTree &tree,
    const std::shared_ptr<const HostOwnedStateDirectory> &authority)
{
    WindowsStableFile existingTarget;
    const WindowsStableFileOpenStatus targetStatus =
        existingTarget.openReadDeleteLocked(targetPath, tree);
    if (targetStatus == WindowsStableFileOpenStatus::Failure
        || (targetStatus == WindowsStableFileOpenStatus::Opened
            && !stableLeaf(existingTarget, targetPath, tree, authority))
        || !removeSafeTemporary(temporaryPath, tree, authority)) {
        return false;
    }

    WindowsStableFile temporary;
    if (!temporary.createRestrictedOutput(temporaryPath, tree)
        || !temporary.writeAll(bytes.constData(), static_cast<size_t>(bytes.size()))
        || !temporary.flush()
        || !stableLeaf(temporary, temporaryPath, tree, authority)) {
        if (temporary.isOpen()) (void)temporary.deleteOwned();
        return false;
    }
    QByteArray readBack;
    if (temporary.readBoundedIncludingEmpty(MaximumSessionBytes, readBack)
            != WindowsStableReadStatus::Read
        || readBack != bytes) {
        (void)temporary.deleteOwned();
        return false;
    }

#ifdef Q_BROWSER_HOST_TESTING
    const auto hooks = qbrowser_host_testing::browserSessionStoreTestHooks();
    if (hooks.beforeAtomicPublish) hooks.beforeAtomicPublish(targetPath);
    if (hooks.failAtomicPublish) {
        (void)temporary.deleteOwned();
        return false;
    }
#endif
    if (!stableLeaf(temporary, temporaryPath, tree, authority)
        || (targetStatus == WindowsStableFileOpenStatus::Opened
            && !stableLeaf(existingTarget, targetPath, tree, authority))
        || (targetStatus == WindowsStableFileOpenStatus::Missing
            && !stableRoot(authority, tree))
        || !temporary.publishAtomic(
            targetPath,
            tree,
            targetStatus == WindowsStableFileOpenStatus::Opened)) {
        if (temporary.isOpen()) (void)temporary.deleteOwned();
        return false;
    }
#ifdef Q_BROWSER_HOST_TESTING
    const auto afterHooks = qbrowser_host_testing::browserSessionStoreTestHooks();
    if (afterHooks.afterAtomicPublishBeforeVerify) {
        afterHooks.afterAtomicPublishBeforeVerify(targetPath);
    }
#endif
    return verifyPublishedBytes(targetPath, bytes, tree, authority, false);
}
#endif
}

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
namespace
{
BrowserSessionStoreTestHooks currentHooks;
std::mutex hooksMutex;
}

void setBrowserSessionStoreTestHooks(BrowserSessionStoreTestHooks hooks)
{
    std::lock_guard lock(hooksMutex);
    currentHooks = std::move(hooks);
}

void resetBrowserSessionStoreTestHooks()
{
    std::lock_guard lock(hooksMutex);
    currentHooks = {};
}

BrowserSessionStoreTestHooks browserSessionStoreTestHooks()
{
    std::lock_guard lock(hooksMutex);
    return currentHooks;
}
}
#endif

BrowserSessionStore::BrowserSessionStore(
    std::shared_ptr<const HostOwnedStateDirectory> stateDirectory)
    : stateDirectory_(std::move(stateDirectory))
{
}

BrowserSessionLoadResult BrowserSessionStore::load()
{
#ifndef Q_OS_WIN
    return ioFailure();
#else
    WindowsStableDirectoryTree tree;
    if (!openStableRoot(stateDirectory_, tree)) return ioFailure();
    const QDir root(stateDirectory_->canonicalPath());
    const QString primaryPath = root.filePath(QString::fromLatin1(SessionLeaf));

    WindowsStableFile primary;
    const WindowsStableFileOpenStatus openStatus =
        primary.openReadDeleteLocked(primaryPath, tree);
    if (openStatus == WindowsStableFileOpenStatus::Missing) {
        return stableRoot(stateDirectory_, tree)
            ? BrowserSessionLoadResult{
                  BrowserSessionLoadStatus::Missing, std::nullopt, {}}
            : ioFailure();
    }
    if (openStatus != WindowsStableFileOpenStatus::Opened
        || !stableLeaf(primary, primaryPath, tree, stateDirectory_)) {
        return ioFailure();
    }
    invokePrimaryReadHook(primaryPath);
    if (!stableLeaf(primary, primaryPath, tree, stateDirectory_)) {
        return ioFailure();
    }

    QByteArray bytes;
    const WindowsStableReadStatus readStatus = primary.readBoundedIncludingEmpty(
        MaximumSessionBytes, bytes);
    if (readStatus == WindowsStableReadStatus::Failure
        || !stableLeaf(primary, primaryPath, tree, stateDirectory_)) {
        return ioFailure();
    }
    if (readStatus == WindowsStableReadStatus::TooLarge) {
        return preserveAndRemoveCorrupt(
                   primary,
                   primaryPath,
                   std::nullopt,
                   tree,
                   stateDirectory_)
            ? corruptResult()
            : ioFailure();
    }

    std::optional<BrowserWindowSnapshot> parsed = parseSnapshot(bytes);
    if (!parsed.has_value()) {
        return preserveAndRemoveCorrupt(
                   primary,
                   primaryPath,
                   bytes,
                   tree,
                   stateDirectory_)
            ? corruptResult()
            : ioFailure();
    }
    return {BrowserSessionLoadStatus::Loaded, std::move(parsed), {}};
#endif
}

BrowserSessionSaveResult BrowserSessionStore::save(
    const BrowserWindowSnapshot &snapshot)
{
    const std::optional<BrowserWindowSnapshot> normalized = normalizedSnapshot(
        snapshot, false);
    const std::optional<QByteArray> bytes = normalized.has_value()
        ? serializedSnapshot(*normalized)
        : std::nullopt;
    if (!normalized.has_value() || !bytes.has_value()
        || static_cast<quint64>(bytes->size()) > MaximumSessionBytes) {
        return {BrowserSessionSaveStatus::InvalidSnapshot,
                QStringLiteral("host.session.invalid_snapshot")};
    }
#ifndef Q_OS_WIN
    return saveIoFailure();
#else
    WindowsStableDirectoryTree tree;
    if (!openStableRoot(stateDirectory_, tree)) return saveIoFailure();
    const QDir root(stateDirectory_->canonicalPath());
    const QString targetPath = root.filePath(QString::fromLatin1(SessionLeaf));
    const QString temporaryPath = root.filePath(
        QString::fromLatin1(SessionTemporaryLeaf));
    return saveAtomically(
               *bytes, targetPath, temporaryPath, tree, stateDirectory_)
        ? BrowserSessionSaveResult{BrowserSessionSaveStatus::Saved, {}}
        : saveIoFailure();
#endif
}

BrowserSessionResolveResult BrowserSessionStore::validateAndResolve(
    const BrowserWindowSnapshot &raw,
    const RestoredAddressResolver &resolver) const
{
    std::optional<BrowserWindowSnapshot> resolved = normalizedSnapshot(raw, true);
    if (!resolved.has_value() || !resolver) {
        return {std::nullopt, QStringLiteral("host.session.resolve_invalid")};
    }
    for (BrowserTabSnapshot &tab : resolved->tabs) {
        const std::optional<BrowserAddress> address = parseLogicalAddress(
            tab.address);
        if (!address.has_value()) {
            return {std::nullopt,
                    QStringLiteral("host.session.resolve_invalid")};
        }
        const std::optional<BrowserTabKind> currentKind = resolver(*address);
        if (!currentKind.has_value()) {
            tab.kind = BrowserTabKind::TrustedError;
        } else if (!isValidKind(*currentKind)) {
            return {std::nullopt,
                    QStringLiteral("host.session.resolve_invalid")};
        } else {
            tab.kind = *currentKind;
        }
    }
    return {std::move(resolved), {}};
}
