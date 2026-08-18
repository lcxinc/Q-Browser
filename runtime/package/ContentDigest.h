#pragma once

#include "Archive.h"

#include <QByteArray>

#include <optional>

enum class ContentDigestErrorCode
{
    None,
    InvalidPath,
    DuplicatePath,
    MissingMetadata,
    InvalidMetadata,
    DigestMismatch,
};

struct ContentDigestError final
{
    ContentDigestErrorCode code = ContentDigestErrorCode::None;
    QByteArray path;
    QString message;
};

class ContentDigestResult final
{
public:
    explicit ContentDigestResult(QByteArray digest);
    explicit ContentDigestResult(ContentDigestError error);
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const QByteArray &bytes() const noexcept;
    [[nodiscard]] QByteArray hex() const;
    [[nodiscard]] const ContentDigestError &error() const noexcept;

private:
    std::optional<QByteArray> m_digest;
    ContentDigestError m_error;
};

class ContentDigestValidation final
{
public:
    explicit ContentDigestValidation(QByteArray digest);
    explicit ContentDigestValidation(ContentDigestError error);
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const QByteArray &digest() const noexcept;
    [[nodiscard]] const ContentDigestError &error() const noexcept;

private:
    std::optional<QByteArray> m_digest;
    ContentDigestError m_error;
};

class ContentDigest final
{
public:
    // Payload excludes both generated metadata files.
    [[nodiscard]] static ContentDigestResult payload(
        const QVector<ArchiveFile> &files);
    // Signed package excludes only the signature, and therefore commits to
    // metadata/content.sha256 without creating a circular hash.
    [[nodiscard]] static ContentDigestResult signedPackage(
        const QVector<ArchiveFile> &files);
    [[nodiscard]] static ContentDigestValidation validatePayload(
        const QVector<ArchiveFile> &files);
};
