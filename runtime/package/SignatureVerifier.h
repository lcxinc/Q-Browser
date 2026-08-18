#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

enum class SignatureErrorCode
{
    None,
    InvalidPublicKey,
    InvalidPrivateKey,
    InvalidSignatureLength,
    SignatureMismatch,
    CryptoFailure,
};

struct SignatureError final
{
    SignatureErrorCode code = SignatureErrorCode::None;
    QString message;
};

class Ed25519PublicKey final
{
public:
    [[nodiscard]] static std::optional<Ed25519PublicKey> fromRaw(QByteArray raw);
    [[nodiscard]] const QByteArray &bytes() const noexcept;

private:
    explicit Ed25519PublicKey(QByteArray raw);
    QByteArray m_raw;
};

class Ed25519Signature final
{
public:
    [[nodiscard]] static std::optional<Ed25519Signature> fromRaw(QByteArray raw);
    [[nodiscard]] const QByteArray &bytes() const noexcept;

private:
    explicit Ed25519Signature(QByteArray raw);
    QByteArray m_raw;
};

struct SignatureKeyPair final
{
    SignatureKeyPair() = default;
    SignatureKeyPair(QByteArray privatePem, QByteArray publicPem, QByteArray publicRaw);
    ~SignatureKeyPair();
    SignatureKeyPair(const SignatureKeyPair &) = delete;
    SignatureKeyPair &operator=(const SignatureKeyPair &) = delete;
    SignatureKeyPair(SignatureKeyPair &&other) noexcept;
    SignatureKeyPair &operator=(SignatureKeyPair &&other) noexcept;

    QByteArray privateKeyPem;
    QByteArray publicKeyPem;
    QByteArray publicKeyRaw;
};

class SignatureKeyPairResult final
{
public:
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const SignatureKeyPair &value() const noexcept;
    [[nodiscard]] const SignatureError &error() const noexcept;

private:
    friend class SignatureVerifier;
    explicit SignatureKeyPairResult(SignatureKeyPair value);
    explicit SignatureKeyPairResult(SignatureError error);
    std::optional<SignatureKeyPair> m_value;
    SignatureError m_error;
};

class SignatureOperationResult final
{
public:
    explicit SignatureOperationResult(QByteArray value);
    explicit SignatureOperationResult(SignatureError error);
    [[nodiscard]] bool hasValue() const noexcept;
    [[nodiscard]] const QByteArray &value() const noexcept;
    [[nodiscard]] const SignatureError &error() const noexcept;

private:
    std::optional<QByteArray> m_value;
    SignatureError m_error;
};

class SignatureVerificationResult final
{
public:
    [[nodiscard]] bool isVerified() const noexcept;
    [[nodiscard]] const SignatureError &error() const noexcept;

private:
    friend class SignatureVerifier;
    explicit SignatureVerificationResult(bool verified, SignatureError error = {});
    bool m_verified = false;
    SignatureError m_error;
};

class SignatureVerifier final
{
public:
    [[nodiscard]] static SignatureKeyPairResult generateKeyPair();
    [[nodiscard]] static SignatureOperationResult signRaw(
        const QByteArray &message,
        const QByteArray &privateSeed);
    [[nodiscard]] static SignatureOperationResult signPem(
        const QByteArray &message,
        const QByteArray &privateKeyPem);
    [[nodiscard]] static SignatureVerificationResult verify(
        const QByteArray &message,
        const Ed25519PublicKey &publicKey,
        const Ed25519Signature &signature);
    [[nodiscard]] static SignatureVerificationResult verifyPem(
        const QByteArray &message,
        const QByteArray &publicKeyPem,
        const QByteArray &signature);
};
