#include "ContentDigest.h"
#include "CanonicalArchivePath.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QtEndian>

#include <algorithm>

namespace
{
constexpr char PayloadDomain[] = "Q-Browser qapkg payload v1\0";
constexpr char SignedDomain[] = "Q-Browser qapkg signed v1\0";
constexpr auto ContentPath = "metadata/content.sha256";
constexpr auto SignaturePath = "metadata/signature.ed25519";

void addLength(QCryptographicHash &hash, quint64 length)
{
    uchar bytes[sizeof(quint64)]{};
    qToBigEndian<quint64>(length, bytes);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char *>(bytes),
        static_cast<qsizetype>(sizeof(bytes))));
}

ContentDigestResult calculate(
    const QVector<ArchiveFile> &files,
    bool payload)
{
    QVector<ArchiveFile> included;
    QVector<qbrowser_archive_detail::ArchiveCollisionPath> paths;
    paths.reserve(files.size());
    for (const ArchiveFile &entry : files) {
        const auto checked = qbrowser_archive_detail::validateArchivePath(
            entry.path);
        if (!checked.has_value() || !checked->isCanonical) {
            return ContentDigestResult({ContentDigestErrorCode::InvalidPath,
                                        entry.path,
                                        QStringLiteral("digest path is invalid")});
        }
        paths.push_back({checked->collisionKey, entry.path, true});
        if (entry.path == SignaturePath
            || (payload && entry.path == ContentPath)) {
            continue;
        }
        included.push_back(entry);
    }
    if (const auto collision =
            qbrowser_archive_detail::findArchivePathCollision(paths);
        collision.has_value()) {
        return ContentDigestResult({ContentDigestErrorCode::DuplicatePath,
                                    *collision,
                                    QStringLiteral("digest path is duplicated")});
    }
    std::sort(
        included.begin(), included.end(), [](const ArchiveFile &left,
                                             const ArchiveFile &right) {
            return qbrowser_archive_detail::archivePathBytewiseLess(
                left.path, right.path);
        });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const char *domain = payload ? PayloadDomain : SignedDomain;
    const qsizetype domainSize = payload
        ? static_cast<qsizetype>(sizeof(PayloadDomain) - 1U)
        : static_cast<qsizetype>(sizeof(SignedDomain) - 1U);
    hash.addData(QByteArrayView(domain, domainSize));
    for (const ArchiveFile &entry : included) {
        addLength(hash, static_cast<quint64>(entry.path.size()));
        hash.addData(entry.path);
        addLength(hash, static_cast<quint64>(entry.contents.size()));
        hash.addData(entry.contents);
    }
    return ContentDigestResult(hash.result());
}
}

ContentDigestResult::ContentDigestResult(QByteArray digest)
    : m_digest(std::move(digest))
{
}

ContentDigestResult::ContentDigestResult(ContentDigestError error)
    : m_error(std::move(error))
{
}

bool ContentDigestResult::hasValue() const noexcept
{
    return m_digest.has_value();
}

const QByteArray &ContentDigestResult::bytes() const noexcept
{
    static const QByteArray empty;
    return m_digest.has_value() ? *m_digest : empty;
}

QByteArray ContentDigestResult::hex() const
{
    return m_digest.has_value() ? m_digest->toHex() : QByteArray{};
}

const ContentDigestError &ContentDigestResult::error() const noexcept
{
    return m_error;
}

ContentDigestValidation::ContentDigestValidation(QByteArray digest)
    : m_digest(std::move(digest))
{
}

ContentDigestValidation::ContentDigestValidation(ContentDigestError error)
    : m_error(std::move(error))
{
}

bool ContentDigestValidation::isValid() const noexcept
{
    return m_digest.has_value();
}

const QByteArray &ContentDigestValidation::digest() const noexcept
{
    static const QByteArray empty;
    return m_digest.has_value() ? *m_digest : empty;
}

const ContentDigestError &ContentDigestValidation::error() const noexcept
{
    return m_error;
}

ContentDigestResult ContentDigest::payload(const QVector<ArchiveFile> &files)
{
    return calculate(files, true);
}

ContentDigestResult ContentDigest::signedPackage(const QVector<ArchiveFile> &files)
{
    return calculate(files, false);
}

ContentDigestValidation ContentDigest::validatePayload(
    const QVector<ArchiveFile> &files)
{
    const ArchiveFile *metadata = nullptr;
    for (const ArchiveFile &entry : files) {
        if (entry.path == ContentPath) {
            metadata = &entry;
            break;
        }
    }
    if (metadata == nullptr) {
        return ContentDigestValidation({ContentDigestErrorCode::MissingMetadata,
                                        QByteArray(ContentPath),
                                        QStringLiteral("content digest is missing")});
    }
    if (metadata->contents.size() != 64
        || std::any_of(
            metadata->contents.cbegin(), metadata->contents.cend(), [](char value) {
                return !((value >= '0' && value <= '9')
                         || (value >= 'a' && value <= 'f'));
            })) {
        return ContentDigestValidation({ContentDigestErrorCode::InvalidMetadata,
                                        QByteArray(ContentPath),
                                        QStringLiteral("content digest metadata is invalid")});
    }
    const ContentDigestResult calculated = payload(files);
    if (!calculated.hasValue()) {
        return ContentDigestValidation(calculated.error());
    }
    if (metadata->contents != calculated.hex()) {
        return ContentDigestValidation({ContentDigestErrorCode::DigestMismatch,
                                        QByteArray(ContentPath),
                                        QStringLiteral("content digest does not match")});
    }
    return ContentDigestValidation(calculated.bytes());
}
