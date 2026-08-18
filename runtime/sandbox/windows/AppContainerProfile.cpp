#include "AppContainerProfile.h"

#include <QCryptographicHash>

#include <sddl.h>
#include <userenv.h>

#include <utility>

namespace {

bool validAppId(const QString &appId)
{
    if (appId.isEmpty() || appId.size() > 128 || appId.startsWith(u'.')
        || appId.endsWith(u'.')) {
        return false;
    }
    bool previousSeparator = false;
    for (const QChar character : appId) {
        const bool alphanumeric = (character >= u'a' && character <= u'z')
            || (character >= u'0' && character <= u'9');
        const bool separator = character == u'.' || character == u'-';
        if (!alphanumeric && !separator) {
            return false;
        }
        if (separator && previousSeparator) {
            return false;
        }
        previousSeparator = separator;
    }
    return true;
}

} // namespace

AppContainerProfile::~AppContainerProfile()
{
    reset();
}

AppContainerProfile::AppContainerProfile(AppContainerProfile &&other) noexcept
    : name_(std::move(other.name_)),
      sid_(std::exchange(other.sid_, nullptr)),
      created_(std::exchange(other.created_, false))
{
}

AppContainerProfile &AppContainerProfile::operator=(
    AppContainerProfile &&other) noexcept
{
    if (this != &other) {
        reset();
        name_ = std::move(other.name_);
        sid_ = std::exchange(other.sid_, nullptr);
        created_ = std::exchange(other.created_, false);
    }
    return *this;
}

std::optional<QString> AppContainerProfile::deterministicName(
    const QString &appId)
{
    if (!validAppId(appId)) {
        return std::nullopt;
    }
    const QByteArray digest = QCryptographicHash::hash(
        appId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("QBrowser.Mvp.")
        + QString::fromLatin1(digest.first(48));
}

std::optional<AppContainerProfile> AppContainerProfile::createOrOpen(
    const QString &appId)
{
    const auto profileName = deterministicName(appId);
    if (!profileName.has_value()) {
        return std::nullopt;
    }

    PSID profileSid = nullptr;
    const HRESULT created = CreateAppContainerProfile(
        reinterpret_cast<PCWSTR>(profileName->utf16()),
        reinterpret_cast<PCWSTR>(profileName->utf16()),
        L"Q-Browser isolated QML worker",
        nullptr,
        0,
        &profileSid);
    if (SUCCEEDED(created) && profileSid != nullptr && IsValidSid(profileSid)) {
        return AppContainerProfile(*profileName, profileSid, true);
    }
    if (profileSid != nullptr) {
        FreeSid(profileSid);
        profileSid = nullptr;
    }
    if (created != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)
        || FAILED(DeriveAppContainerSidFromAppContainerName(
            reinterpret_cast<PCWSTR>(profileName->utf16()), &profileSid))
        || profileSid == nullptr || !IsValidSid(profileSid)) {
        if (profileSid != nullptr) {
            FreeSid(profileSid);
        }
        return std::nullopt;
    }
    return AppContainerProfile(*profileName, profileSid, false);
}

bool AppContainerProfile::isValid() const noexcept
{
    return sid_ != nullptr && IsValidSid(sid_) != FALSE && !name_.isEmpty();
}

bool AppContainerProfile::wasCreated() const noexcept
{
    return created_;
}

const QString &AppContainerProfile::name() const noexcept
{
    return name_;
}

PSID AppContainerProfile::sid() const noexcept
{
    return sid_;
}

QString AppContainerProfile::sidString() const
{
    if (!isValid()) {
        return {};
    }
    LPWSTR converted = nullptr;
    if (!ConvertSidToStringSidW(sid_, &converted) || converted == nullptr) {
        return {};
    }
    const QString value = QString::fromWCharArray(converted);
    LocalFree(converted);
    return value;
}

AppContainerProfile::AppContainerProfile(QString name,
                                         PSID sid,
                                         const bool created) noexcept
    : name_(std::move(name)), sid_(sid), created_(created)
{
}

void AppContainerProfile::reset() noexcept
{
    if (sid_ != nullptr) {
        FreeSid(sid_);
    }
    sid_ = nullptr;
    created_ = false;
    name_.clear();
}
