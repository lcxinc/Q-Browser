#include "SignatureVerifier.h"
#include "SignatureTestHooks.h"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <memory>

namespace
{
using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PKeyContextPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

SignatureError error(SignatureErrorCode code, const char *message)
{
    ERR_clear_error();
    return {code, QString::fromLatin1(message)};
}

void clearSecret(QByteArray &secret) noexcept
{
    if (!secret.isEmpty()) {
        OPENSSL_cleanse(secret.data(), static_cast<size_t>(secret.size()));
        secret.clear();
    }
}

QByteArray bioContents(BIO *bio)
{
    BUF_MEM *buffer = nullptr;
    BIO_get_mem_ptr(bio, &buffer);
    return buffer != nullptr
        ? QByteArray(buffer->data, static_cast<qsizetype>(buffer->length))
        : QByteArray{};
}

class PrivateMemoryBio final
{
public:
    explicit PrivateMemoryBio(BIO *bio) noexcept : m_bio(bio) {}

    ~PrivateMemoryBio()
    {
        if (m_bio == nullptr) {
            return;
        }
        BUF_MEM *buffer = nullptr;
        (void)BIO_get_mem_ptr(m_bio, &buffer);
        if (buffer != nullptr && buffer->data != nullptr && buffer->length > 0U) {
            OPENSSL_cleanse(buffer->data, buffer->length);
        }
#ifdef Q_BROWSER_SIGNATURE_TESTING
        if (qbrowser_signature_testing::signatureTestHooks()
                .afterPrivateBioCleanseBeforeFree) {
            const QByteArrayView view = buffer != nullptr && buffer->data != nullptr
                ? QByteArrayView(
                      buffer->data, static_cast<qsizetype>(buffer->length))
                : QByteArrayView{};
            qbrowser_signature_testing::signatureTestHooks()
                .afterPrivateBioCleanseBeforeFree(view);
        }
#endif
        BIO_free(m_bio);
    }

    PrivateMemoryBio(const PrivateMemoryBio &) = delete;
    PrivateMemoryBio &operator=(const PrivateMemoryBio &) = delete;

    [[nodiscard]] BIO *get() const noexcept { return m_bio; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_bio != nullptr; }

private:
    BIO *m_bio = nullptr;
};

PKeyPtr readPrivatePem(const QByteArray &pem)
{
    ERR_clear_error();
    BioPtr bio(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) {
        return {nullptr, EVP_PKEY_free};
    }
    PKeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key || BIO_ctrl_pending(bio.get()) != 0
        || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        ERR_clear_error();
        return {nullptr, EVP_PKEY_free};
    }
    size_t length = 0;
    if (EVP_PKEY_get_raw_private_key(key.get(), nullptr, &length) != 1 || length != 32U) {
        ERR_clear_error();
        return {nullptr, EVP_PKEY_free};
    }
    ERR_clear_error();
    return key;
}

PKeyPtr readPublicPem(const QByteArray &pem)
{
    ERR_clear_error();
    BioPtr bio(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) {
        return {nullptr, EVP_PKEY_free};
    }
    PKeyPtr key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key || BIO_ctrl_pending(bio.get()) != 0
        || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        ERR_clear_error();
        return {nullptr, EVP_PKEY_free};
    }
    size_t length = 0;
    if (EVP_PKEY_get_raw_public_key(key.get(), nullptr, &length) != 1 || length != 32U) {
        ERR_clear_error();
        return {nullptr, EVP_PKEY_free};
    }
    ERR_clear_error();
    return key;
}

SignatureOperationResult sign(EVP_PKEY *key, const QByteArray &message)
{
    ERR_clear_error();
    MdContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) != 1) {
        return SignatureOperationResult(error(
            SignatureErrorCode::CryptoFailure, "signature operation failed"));
    }
    QByteArray signature(64, Qt::Uninitialized);
    size_t length = static_cast<size_t>(signature.size());
    if (EVP_DigestSign(
            context.get(),
            reinterpret_cast<unsigned char *>(signature.data()),
            &length,
            reinterpret_cast<const unsigned char *>(message.constData()),
            static_cast<size_t>(message.size()))
        != 1
        || length != 64U) {
        OPENSSL_cleanse(signature.data(), static_cast<size_t>(signature.size()));
        return SignatureOperationResult(error(
            SignatureErrorCode::CryptoFailure, "signature operation failed"));
    }
    ERR_clear_error();
    return SignatureOperationResult(std::move(signature));
}
}

Ed25519PublicKey::Ed25519PublicKey(QByteArray raw) : m_raw(std::move(raw)) {}

std::optional<Ed25519PublicKey> Ed25519PublicKey::fromRaw(QByteArray raw)
{
    return raw.size() == 32 ? std::optional<Ed25519PublicKey>(Ed25519PublicKey(std::move(raw)))
                            : std::nullopt;
}

const QByteArray &Ed25519PublicKey::bytes() const noexcept { return m_raw; }

Ed25519Signature::Ed25519Signature(QByteArray raw) : m_raw(std::move(raw)) {}

std::optional<Ed25519Signature> Ed25519Signature::fromRaw(QByteArray raw)
{
    return raw.size() == 64 ? std::optional<Ed25519Signature>(Ed25519Signature(std::move(raw)))
                            : std::nullopt;
}

const QByteArray &Ed25519Signature::bytes() const noexcept { return m_raw; }

SignatureKeyPair::SignatureKeyPair(
    QByteArray privatePem, QByteArray publicPem, QByteArray publicRaw)
    : privateKeyPem(std::move(privatePem)),
      publicKeyPem(std::move(publicPem)),
      publicKeyRaw(std::move(publicRaw))
{
}

SignatureKeyPair::~SignatureKeyPair()
{
    clearSecret(privateKeyPem);
}

SignatureKeyPair::SignatureKeyPair(SignatureKeyPair &&other) noexcept
    : privateKeyPem(std::move(other.privateKeyPem)),
      publicKeyPem(std::move(other.publicKeyPem)),
      publicKeyRaw(std::move(other.publicKeyRaw))
{
}

SignatureKeyPair &SignatureKeyPair::operator=(SignatureKeyPair &&other) noexcept
{
    if (this != &other) {
        clearSecret(privateKeyPem);
        privateKeyPem = std::move(other.privateKeyPem);
        publicKeyPem = std::move(other.publicKeyPem);
        publicKeyRaw = std::move(other.publicKeyRaw);
    }
    return *this;
}

SignatureKeyPairResult::SignatureKeyPairResult(SignatureKeyPair value)
    : m_value(std::move(value)) {}
SignatureKeyPairResult::SignatureKeyPairResult(SignatureError errorValue)
    : m_error(std::move(errorValue)) {}
bool SignatureKeyPairResult::hasValue() const noexcept { return m_value.has_value(); }
const SignatureKeyPair &SignatureKeyPairResult::value() const noexcept
{
    static const SignatureKeyPair empty;
    return m_value.has_value() ? *m_value : empty;
}
const SignatureError &SignatureKeyPairResult::error() const noexcept { return m_error; }

SignatureOperationResult::SignatureOperationResult(QByteArray value)
    : m_value(std::move(value)) {}
SignatureOperationResult::SignatureOperationResult(SignatureError errorValue)
    : m_error(std::move(errorValue)) {}
bool SignatureOperationResult::hasValue() const noexcept { return m_value.has_value(); }
const QByteArray &SignatureOperationResult::value() const noexcept
{
    static const QByteArray empty;
    return m_value.has_value() ? *m_value : empty;
}
const SignatureError &SignatureOperationResult::error() const noexcept { return m_error; }

SignatureVerificationResult::SignatureVerificationResult(
    bool verified, SignatureError errorValue)
    : m_verified(verified), m_error(std::move(errorValue)) {}
bool SignatureVerificationResult::isVerified() const noexcept { return m_verified; }
const SignatureError &SignatureVerificationResult::error() const noexcept { return m_error; }

SignatureKeyPairResult SignatureVerifier::generateKeyPair()
{
    ERR_clear_error();
    PKeyContextPtr generator(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *generated = nullptr;
    if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1
        || EVP_PKEY_keygen(generator.get(), &generated) != 1) {
        return SignatureKeyPairResult(error(
            SignatureErrorCode::CryptoFailure, "key generation failed"));
    }
    PKeyPtr key(generated, EVP_PKEY_free);
    PrivateMemoryBio privateBio(BIO_new(BIO_s_mem()));
    BioPtr publicBio(BIO_new(BIO_s_mem()), BIO_free);
    QByteArray raw(32, Qt::Uninitialized);
    size_t rawLength = static_cast<size_t>(raw.size());
    if (!privateBio || !publicBio
        || PEM_write_bio_PrivateKey(
               privateBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr)
            != 1) {
        OPENSSL_cleanse(raw.data(), static_cast<size_t>(raw.size()));
        return SignatureKeyPairResult(error(
            SignatureErrorCode::CryptoFailure, "key generation failed"));
    }
#ifdef Q_BROWSER_SIGNATURE_TESTING
    if (qbrowser_signature_testing::signatureTestHooks()
            .failAfterPrivatePemWrite) {
        OPENSSL_cleanse(raw.data(), static_cast<size_t>(raw.size()));
        return SignatureKeyPairResult(error(
            SignatureErrorCode::CryptoFailure, "key generation failed"));
    }
#endif
    if (PEM_write_bio_PUBKEY(publicBio.get(), key.get()) != 1
        || EVP_PKEY_get_raw_public_key(
               key.get(), reinterpret_cast<unsigned char *>(raw.data()), &rawLength) != 1
        || rawLength != 32U) {
        OPENSSL_cleanse(raw.data(), static_cast<size_t>(raw.size()));
        return SignatureKeyPairResult(error(
            SignatureErrorCode::CryptoFailure, "key generation failed"));
    }
    SignatureKeyPair pair(
        bioContents(privateBio.get()), bioContents(publicBio.get()), std::move(raw));
    ERR_clear_error();
    return SignatureKeyPairResult(std::move(pair));
}

SignatureOperationResult SignatureVerifier::signRaw(
    const QByteArray &message, const QByteArray &privateSeed)
{
    if (privateSeed.size() != 32) {
        return SignatureOperationResult(error(
            SignatureErrorCode::InvalidPrivateKey, "private key is invalid"));
    }
    QByteArray privateCopy = privateSeed;
    ERR_clear_error();
    PKeyPtr key(EVP_PKEY_new_raw_private_key_ex(
                    nullptr,
                    "ED25519",
                    nullptr,
                    reinterpret_cast<const unsigned char *>(privateCopy.constData()),
                    static_cast<size_t>(privateCopy.size())),
                EVP_PKEY_free);
    OPENSSL_cleanse(privateCopy.data(), static_cast<size_t>(privateCopy.size()));
    if (!key) {
        return SignatureOperationResult(error(
            SignatureErrorCode::CryptoFailure, "signature operation failed"));
    }
    return sign(key.get(), message);
}

SignatureOperationResult SignatureVerifier::signPem(
    const QByteArray &message, const QByteArray &privateKeyPem)
{
    PKeyPtr key = readPrivatePem(privateKeyPem);
    return key ? sign(key.get(), message)
               : SignatureOperationResult(error(
                     SignatureErrorCode::InvalidPrivateKey, "private key is invalid"));
}

SignatureVerificationResult SignatureVerifier::verify(
    const QByteArray &message,
    const Ed25519PublicKey &publicKey,
    const Ed25519Signature &signature)
{
    ERR_clear_error();
    PKeyPtr key(EVP_PKEY_new_raw_public_key_ex(
                    nullptr,
                    "ED25519",
                    nullptr,
                    reinterpret_cast<const unsigned char *>(publicKey.bytes().constData()),
                    static_cast<size_t>(publicKey.bytes().size())),
                EVP_PKEY_free);
    MdContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !context
        || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return SignatureVerificationResult(false, error(
            SignatureErrorCode::CryptoFailure, "signature verification failed"));
    }
    const int result = EVP_DigestVerify(
        context.get(),
        reinterpret_cast<const unsigned char *>(signature.bytes().constData()),
        static_cast<size_t>(signature.bytes().size()),
        reinterpret_cast<const unsigned char *>(message.constData()),
        static_cast<size_t>(message.size()));
    if (result == 1) {
        ERR_clear_error();
        return SignatureVerificationResult(true);
    }
    if (result == 0) {
        return SignatureVerificationResult(false, error(
            SignatureErrorCode::SignatureMismatch, "signature does not match"));
    }
    return SignatureVerificationResult(false, error(
        SignatureErrorCode::CryptoFailure, "signature verification failed"));
}

SignatureVerificationResult SignatureVerifier::verifyPem(
    const QByteArray &message,
    const QByteArray &publicKeyPem,
    const QByteArray &signature)
{
    if (signature.size() != 64) {
        return SignatureVerificationResult(false, error(
            SignatureErrorCode::InvalidSignatureLength, "signature length is invalid"));
    }
    PKeyPtr key = readPublicPem(publicKeyPem);
    if (!key) {
        return SignatureVerificationResult(false, error(
            SignatureErrorCode::InvalidPublicKey, "public key is invalid"));
    }
    QByteArray raw(32, Qt::Uninitialized);
    size_t length = static_cast<size_t>(raw.size());
    if (EVP_PKEY_get_raw_public_key(
            key.get(), reinterpret_cast<unsigned char *>(raw.data()), &length) != 1
        || length != 32U) {
        return SignatureVerificationResult(false, error(
            SignatureErrorCode::CryptoFailure, "signature verification failed"));
    }
    const auto publicKey = Ed25519PublicKey::fromRaw(std::move(raw));
    const auto parsedSignature = Ed25519Signature::fromRaw(signature);
    return verify(message, *publicKey, *parsedSignature);
}
