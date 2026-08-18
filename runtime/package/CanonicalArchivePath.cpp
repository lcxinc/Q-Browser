#include "CanonicalArchivePath.h"

#include <QHash>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <cstring>

namespace qbrowser_archive_detail
{
namespace
{
bool isWindowsDeviceName(const QString &component)
{
    const QString base = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> fixedNames{
        QStringLiteral("CON"),
        QStringLiteral("PRN"),
        QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("CLOCK$")};
    if (fixedNames.contains(base)) {
        return true;
    }
    if (base.size() == 4
        && (base.startsWith(QStringLiteral("COM"))
            || base.startsWith(QStringLiteral("LPT")))) {
        const QChar suffix = base.back();
        return (suffix >= QLatin1Char('1') && suffix <= QLatin1Char('9'))
            || suffix == QChar(0x00B9U) || suffix == QChar(0x00B2U)
            || suffix == QChar(0x00B3U);
    }
    return false;
}
}

bool archivePathBytewiseLess(
    const QByteArray &left,
    const QByteArray &right)
{
    const qsizetype common = std::min(left.size(), right.size());
    const int comparison = std::memcmp(
        left.constData(), right.constData(), static_cast<size_t>(common));
    return comparison < 0 || (comparison == 0 && left.size() < right.size());
}

std::optional<CheckedArchivePath> validateArchivePath(
    const QByteArray &path,
    const ArchiveLimits &limits)
{
    if (path.isEmpty() || path.size() > limits.maximumPathBytes
        || path.contains('\0') || path.contains('\\') || path.startsWith('/')) {
        return std::nullopt;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(path);
    if (decoder.hasError() || decoded.isEmpty()
        || decoded.size() > limits.maximumPathUtf16Units) {
        return std::nullopt;
    }
    const QString normalized = decoded.normalized(QString::NormalizationForm_C);

    const QList<QByteArray> rawComponents = path.split('/');
    const QStringList components = decoded.split(QLatin1Char('/'));
    if (rawComponents.size() != components.size()) {
        return std::nullopt;
    }
    static const QString reserved = QStringLiteral("<>:\"|?*");
    for (qsizetype index = 0; index < components.size(); ++index) {
        const QByteArray &raw = rawComponents.at(index);
        const QString &component = components.at(index);
        if (raw.isEmpty() || raw.size() > limits.maximumComponentBytes
            || component.isEmpty()
            || component.size() > limits.maximumComponentUtf16Units
            || component == QStringLiteral(".")
            || component == QStringLiteral("..")
            || component.endsWith(QLatin1Char('.'))
            || component.endsWith(QLatin1Char(' '))
            || isWindowsDeviceName(component)) {
            return std::nullopt;
        }
        for (const QChar character : component) {
            if (character.unicode() < 0x20U || reserved.contains(character)) {
                return std::nullopt;
            }
        }
    }
    return CheckedArchivePath{normalized.toCaseFolded(), normalized == decoded};
}

std::optional<QByteArray> findArchivePathCollision(
    const QVector<ArchiveCollisionPath> &paths)
{
    QHash<QString, QByteArray> firstPathByKey;
    QSet<QString> keys;
    QVector<QByteArray> candidates;
    for (const ArchiveCollisionPath &path : paths) {
        const auto existing = firstPathByKey.constFind(path.key);
        if (existing != firstPathByKey.cend()) {
            candidates.push_back(
                archivePathBytewiseLess(*existing, path.path)
                    ? path.path
                    : *existing);
        } else {
            firstPathByKey.insert(path.key, path.path);
            keys.insert(path.key);
        }
    }
    for (const ArchiveCollisionPath &path : paths) {
        qsizetype separator = path.key.indexOf(QLatin1Char('/'));
        while (separator >= 0) {
            if (keys.contains(path.key.left(separator))) {
                candidates.push_back(path.path);
                break;
            }
            separator = path.key.indexOf(QLatin1Char('/'), separator + 1);
        }
    }
    if (candidates.isEmpty()) {
        return std::nullopt;
    }
    return *std::min_element(
        candidates.cbegin(),
        candidates.cend(),
        archivePathBytewiseLess);
}

std::optional<QByteArray> firstNonCanonicalArchivePath(
    const QVector<ArchiveCollisionPath> &paths)
{
    std::optional<QByteArray> result;
    for (const ArchiveCollisionPath &path : paths) {
        if (!path.isCanonical
            && (!result.has_value()
                || archivePathBytewiseLess(path.path, *result))) {
            result = path.path;
        }
    }
    return result;
}
}
