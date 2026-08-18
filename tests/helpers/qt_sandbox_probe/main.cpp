#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <qt_windows.h>

#include <sddl.h>

#include <cstdint>
#include <vector>

namespace {

HANDLE handleArgument(const QStringList &arguments, const QString &name)
{
    const qsizetype index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size()) {
        return nullptr;
    }
    bool ok = false;
    const qulonglong value = arguments.at(index + 1).toULongLong(&ok);
    return ok ? reinterpret_cast<HANDLE>(static_cast<quintptr>(value))
              : nullptr;
}

bool writeAll(const HANDLE handle, const QByteArray &bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(bytes.size() - offset);
        if (!WriteFile(handle,
                       bytes.constData() + offset,
                       requested,
                       &written,
                       nullptr)
            || written == 0) {
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

bool sendFrame(const HANDLE handle, const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(
        QJsonDocument::Compact);
    const quint32 size = static_cast<quint32>(payload.size());
    QByteArray frame;
    frame.reserve(payload.size() + 4);
    frame.append(static_cast<char>((size >> 24U) & 0xffU));
    frame.append(static_cast<char>((size >> 16U) & 0xffU));
    frame.append(static_cast<char>((size >> 8U) & 0xffU));
    frame.append(static_cast<char>(size & 0xffU));
    frame.append(payload);
    return writeAll(handle, frame);
}

bool tokenInformationHasSid(const HANDLE token,
                            const TOKEN_INFORMATION_CLASS informationClass,
                            PSID expected)
{
    DWORD bytes = 0;
    GetTokenInformation(token, informationClass, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return false;
    }
    std::vector<unsigned char> storage(bytes);
    if (!GetTokenInformation(token,
                             informationClass,
                             storage.data(),
                             bytes,
                             &bytes)) {
        return false;
    }
    const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(storage.data());
    for (DWORD index = 0; index < groups->GroupCount; ++index) {
        if (EqualSid(groups->Groups[index].Sid, expected)
            && (groups->Groups[index].Attributes & SE_GROUP_ENABLED) != 0U) {
            return true;
        }
    }
    return false;
}

bool queryToken(QJsonObject &result)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD returned = 0;
    DWORD appContainer = 0;
    const bool appContainerQueried = GetTokenInformation(
        token,
        TokenIsAppContainer,
        &appContainer,
        sizeof(appContainer),
        &returned) != FALSE;
    DWORD lessPrivileged = 0;
    const bool lessPrivilegedQueried = GetTokenInformation(
        token,
        TokenIsLessPrivilegedAppContainer,
        &lessPrivileged,
        sizeof(lessPrivileged),
        &returned) != FALSE;

    DWORD capabilityBytes = 0;
    SetLastError(ERROR_SUCCESS);
    const bool capabilitySized = GetTokenInformation(
        token,
        TokenCapabilities,
        nullptr,
        0,
        &capabilityBytes) == FALSE
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER
        && capabilityBytes >= sizeof(TOKEN_GROUPS);
    std::vector<unsigned char> capabilities(capabilityBytes);
    bool capabilitiesQueried = false;
    DWORD capabilityCount = 0;
    QString capabilitySid;
    if (capabilitySized
        && GetTokenInformation(token,
                               TokenCapabilities,
                               capabilities.data(),
                               capabilityBytes,
                               &returned)) {
        capabilitiesQueried = true;
        const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(
            capabilities.data());
        capabilityCount = groups->GroupCount;
        if (capabilityCount == 1) {
            LPWSTR converted = nullptr;
            if (ConvertSidToStringSidW(groups->Groups[0].Sid, &converted)) {
                capabilitySid = QString::fromWCharArray(converted);
                LocalFree(converted);
            }
        }
    }
    PSID allApplicationPackages = nullptr;
    bool allApplicationPackagesMember = false;
    if (ConvertStringSidToSidW(L"S-1-15-2-1", &allApplicationPackages)) {
        allApplicationPackagesMember = tokenInformationHasSid(
            token, TokenGroups, allApplicationPackages)
            || tokenInformationHasSid(
                token, TokenRestrictedSids, allApplicationPackages);
        LocalFree(allApplicationPackages);
    }
    CloseHandle(token);

    result.insert(QStringLiteral("appContainer"),
                  appContainerQueried && appContainer != 0);
    result.insert(QStringLiteral("lessPrivileged"),
                  lessPrivilegedQueried && lessPrivileged != 0);
    result.insert(QStringLiteral("capabilitiesQueried"), capabilitiesQueried);
    result.insert(QStringLiteral("capabilityCount"),
                  static_cast<qint64>(capabilityCount));
    result.insert(QStringLiteral("capabilitySid"), capabilitySid);
    result.insert(QStringLiteral("allApplicationPackagesMember"),
                  allApplicationPackagesMember);
    return appContainerQueried && lessPrivilegedQueried && capabilitiesQueried;
}

} // namespace

int main(int argc, char **argv)
{
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }
    const HANDLE readHandle = handleArgument(
        arguments, QStringLiteral("--qbrowser-ipc-read-handle"));
    const HANDLE writeHandle = handleArgument(
        arguments, QStringLiteral("--qbrowser-ipc-write-handle"));
    if (readHandle == nullptr || writeHandle == nullptr) {
        return 80;
    }
    QJsonObject result{{QStringLiteral("qtVersion"),
                        QString::fromLatin1(qVersion())}};
    result.insert(QStringLiteral("tokenQueried"), queryToken(result));
    if (!sendFrame(writeHandle, result)) {
        return 81;
    }
    char release = 0;
    DWORD read = 0;
    if (!ReadFile(readHandle, &release, 1, &read, nullptr)
        || read != 1 || release != 'r') {
        return 82;
    }
    return 0;
}
