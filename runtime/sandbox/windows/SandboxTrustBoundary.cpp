#include "SandboxTrustBoundary.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <aclapi.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

class UniqueHandle final
{
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle)
    {
    }
    ~UniqueHandle()
    {
        if (validHandle(handle_)) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    UniqueHandle &operator=(UniqueHandle &&other) noexcept
    {
        if (this != &other) {
            if (validHandle(handle_)) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool isValid() const noexcept { return validHandle(handle_); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct FileIdentity final
{
    DWORD volume = 0;
    DWORD high = 0;
    DWORD low = 0;
    [[nodiscard]] bool operator==(const FileIdentity &) const noexcept = default;
};

struct StablePath final
{
    QString requestedPath;
    QString finalPath;
    FileIdentity identity;
    QByteArray securityState;
    bool directory = false;
    UniqueHandle handle;
};

QString withoutExtendedPrefix(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\?\\UNC\\"),
                          Qt::CaseInsensitive)) {
        return QStringLiteral("\\\\") + native.mid(8);
    }
    if (native.startsWith(QStringLiteral("\\\\?\\"),
                          Qt::CaseInsensitive)) {
        return native.mid(4);
    }
    return native;
}

QString absolutePath(const QString &path)
{
    return QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(withoutExtendedPrefix(path)).absoluteFilePath()));
}

QString pathKey(const QString &path)
{
    return QDir::toNativeSeparators(path).toCaseFolded();
}

bool pathWithinOrEqual(const QString &root, const QString &candidate)
{
    QString foldedRoot = pathKey(root);
    const QString foldedCandidate = pathKey(candidate);
    while (foldedRoot.size() > 1
           && (foldedRoot.endsWith(u'\\') || foldedRoot.endsWith(u'/'))
           && foldedRoot != pathKey(QDir(foldedRoot).rootPath())) {
        foldedRoot.chop(1);
    }
    if (foldedCandidate == foldedRoot) {
        return true;
    }
    return foldedRoot.endsWith(u'\\')
        ? foldedCandidate.startsWith(foldedRoot)
        : foldedCandidate.startsWith(foldedRoot + u'\\');
}

bool strictDescendant(const QString &root, const QString &candidate)
{
    return pathKey(root) != pathKey(candidate)
        && pathWithinOrEqual(root, candidate);
}

bool isVolumeRoot(const QString &path)
{
    return pathKey(absolutePath(path))
        == pathKey(absolutePath(QDir(path).rootPath()));
}

bool isBroadRoot(const QString &path)
{
    if (isVolumeRoot(path)) {
        return true;
    }
    const std::array<QString, 7> knownLocations{
        qEnvironmentVariable("SystemRoot"),
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramFiles(x86)"),
        qEnvironmentVariable("ProgramData"),
        qEnvironmentVariable("USERPROFILE"),
        QCoreApplication::applicationDirPath(),
        QDir::currentPath()};
    return std::any_of(knownLocations.cbegin(),
                       knownLocations.cend(),
                       [&path](const QString &known) {
                           return !known.isEmpty()
                               && pathWithinOrEqual(absolutePath(path),
                                                    absolutePath(known));
                       });
}

SandboxValueResult<bool> noReparseAncestors(const QString &path)
{
    std::vector<QString> ancestors;
    QString current = absolutePath(path);
    while (!current.isEmpty()) {
        ancestors.push_back(current);
        const QString parent = absolutePath(QFileInfo(current).dir().absolutePath());
        if (pathKey(parent) == pathKey(current)) {
            break;
        }
        current = parent;
    }
    std::reverse(ancestors.begin(), ancestors.end());
    for (const QString &ancestor : ancestors) {
        UniqueHandle handle(CreateFileW(
            reinterpret_cast<LPCWSTR>(ancestor.utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (!handle.isValid()) {
            return {std::nullopt,
                    QStringLiteral("sandbox.trust.open_failed"),
                    SandboxNativeError::win32(GetLastError())};
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle.get(),
                                          FileAttributeTagInfo,
                                          &attributes,
                                          sizeof(attributes))) {
            return {std::nullopt,
                    QStringLiteral("sandbox.trust.inspect_failed"),
                    SandboxNativeError::win32(GetLastError())};
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return {std::nullopt,
                    QStringLiteral("sandbox.trust.reparse_ancestor"),
                    SandboxNativeError::win32(ERROR_REPARSE_TAG_INVALID)};
        }
    }
    return {true, {}, {}};
}

SandboxValueResult<StablePath> openStablePath(const QString &path,
                                              const bool directory)
{
    const QFileInfo information(path);
    if (!information.isAbsolute() || path.contains(u'\0')) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.path_invalid"),
                SandboxNativeError::win32(ERROR_INVALID_NAME)};
    }
    auto ancestors = noReparseAncestors(path);
    if (!ancestors.value.has_value()) {
        return {std::nullopt, ancestors.errorCode, ancestors.nativeError};
    }
    const QString normalized = absolutePath(path);
    // A launch config is valid by construction only if the Host can later
    // apply and roll back the exact AppContainer ACE on every retained path.
    const DWORD access = READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES
        | (directory ? 0U : GENERIC_READ | FILE_EXECUTE);
    UniqueHandle handle(CreateFileW(
        reinterpret_cast<LPCWSTR>(normalized.utf16()),
        access,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT
            | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0U),
        nullptr));
    if (!handle.isValid()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.open_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle.get(),
                                      FileAttributeTagInfo,
                                      &attributes,
                                      sizeof(attributes))) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.inspect_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    const bool actualDirectory =
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || actualDirectory != directory
        || (!directory && GetFileType(handle.get()) != FILE_TYPE_DISK)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.target_invalid"),
                SandboxNativeError::win32(
                    (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                            != 0U
                        ? ERROR_REPARSE_TAG_INVALID
                        : directory ? ERROR_DIRECTORY : ERROR_INVALID_HANDLE)};
    }
    BY_HANDLE_FILE_INFORMATION identityInformation{};
    if (!GetFileInformationByHandle(handle.get(), &identityInformation)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.identity_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    const DWORD required = GetFinalPathNameByHandleW(
        handle.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0
        || required > static_cast<DWORD>(std::numeric_limits<int>::max())) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.final_path_failed"),
                SandboxNativeError::win32(
                    required == 0 ? GetLastError() : ERROR_ARITHMETIC_OVERFLOW)};
    }
    std::vector<wchar_t> finalBuffer(static_cast<size_t>(required) + 1U);
    const DWORD length = GetFinalPathNameByHandleW(
        handle.get(),
        finalBuffer.data(),
        static_cast<DWORD>(finalBuffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= finalBuffer.size()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.final_path_failed"),
                SandboxNativeError::win32(
                    length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER)};
    }
    StablePath stable;
    stable.requestedPath = normalized;
    stable.finalPath = QString::fromWCharArray(
        finalBuffer.data(), static_cast<qsizetype>(length));
    stable.identity = {identityInformation.dwVolumeSerialNumber,
                       identityInformation.nFileIndexHigh,
                       identityInformation.nFileIndexLow};
    stable.directory = directory;
    stable.handle = std::move(handle);
    return {std::move(stable), {}, {}};
}

bool trustedSid(PSID sid, PSID currentUser)
{
    if (EqualSid(sid, currentUser)) {
        return true;
    }
    BYTE systemBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD systemBytes = sizeof(systemBuffer);
    BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE]{};
    DWORD administratorsBytes = sizeof(administratorsBuffer);
    return (CreateWellKnownSid(WinLocalSystemSid,
                               nullptr,
                               systemBuffer,
                               &systemBytes)
                && EqualSid(sid, systemBuffer))
        || (CreateWellKnownSid(WinBuiltinAdministratorsSid,
                               nullptr,
                               administratorsBuffer,
                               &administratorsBytes)
                && EqualSid(sid, administratorsBuffer));
}

SandboxValueResult<QByteArray> trustedSecurityState(const StablePath &path)
{
    HANDLE tokenRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.token_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    UniqueHandle token(tokenRaw);
    DWORD userBytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || userBytes == 0) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.token_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    std::vector<unsigned char> userBuffer(userBytes);
    if (!GetTokenInformation(token.get(),
                             TokenUser,
                             userBuffer.data(),
                             userBytes,
                             &userBytes)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.token_failed"),
                SandboxNativeError::win32(GetLastError())};
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(userBuffer.data());
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = GetSecurityInfo(
        path.handle.get(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (securityResult != ERROR_SUCCESS || descriptor == nullptr
        || owner == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        return {std::nullopt,
                QStringLiteral("sandbox.trust.security_failed"),
                SandboxNativeError::win32(
                    securityResult != ERROR_SUCCESS
                        ? securityResult
                        : ERROR_INVALID_SECURITY_DESCR)};
    }
    bool safe = trustedSid(owner, user->User.Sid);
    GENERIC_MAPPING fileMapping{FILE_GENERIC_READ,
                                FILE_GENERIC_WRITE,
                                FILE_GENERIC_EXECUTE,
                                FILE_ALL_ACCESS};
    constexpr DWORD dangerous = FILE_WRITE_DATA | FILE_APPEND_DATA
        | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD
        | WRITE_DAC | WRITE_OWNER;
    for (DWORD index = 0; safe && index < dacl->AceCount; ++index) {
        void *rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) {
            safe = false;
            break;
        }
        const auto *header = static_cast<const ACE_HEADER *>(rawAce);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
            PSID sid = const_cast<DWORD *>(&ace->SidStart);
            DWORD mappedMask = ace->Mask;
            MapGenericMask(&mappedMask, &fileMapping);
            if ((mappedMask & dangerous) != 0U
                && !trustedSid(sid, user->User.Sid)) {
                safe = false;
            }
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
            safe = false;
        }
    }
    if (!safe) {
        LocalFree(descriptor);
        return {std::nullopt,
                QStringLiteral("sandbox.trust.root_not_host_owned"),
                SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
    }
    const DWORD descriptorBytes = GetSecurityDescriptorLength(descriptor);
    if (descriptorBytes == 0) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        return {std::nullopt,
                QStringLiteral("sandbox.trust.security_failed"),
                SandboxNativeError::win32(error != ERROR_SUCCESS
                                              ? error
                                              : ERROR_INVALID_SECURITY_DESCR)};
    }
    const QByteArray evidence(static_cast<const char *>(descriptor),
                              static_cast<qsizetype>(descriptorBytes));
    LocalFree(descriptor);
    return {evidence, {}, {}};
}

SandboxValueResult<bool> revalidateStable(const StablePath &stable)
{
    if (!stable.handle.isValid()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.identity_changed"),
                SandboxNativeError::win32(ERROR_INVALID_HANDLE)};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandleEx(stable.handle.get(),
                                      FileAttributeTagInfo,
                                      &attributes,
                                      sizeof(attributes))
        || !GetFileInformationByHandle(stable.handle.get(), &information)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.identity_changed"),
                SandboxNativeError::win32(GetLastError())};
    }
    const FileIdentity identity{information.dwVolumeSerialNumber,
                                information.nFileIndexHigh,
                                information.nFileIndexLow};
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || identity != stable.identity) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.identity_changed"),
                SandboxNativeError::win32(ERROR_FILE_INVALID)};
    }
    auto reopened = openStablePath(stable.requestedPath, stable.directory);
    if (!reopened.value.has_value()
        || reopened.value->identity != stable.identity
        || pathKey(reopened.value->finalPath) != pathKey(stable.finalPath)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.identity_changed"),
                reopened.value.has_value()
                    ? SandboxNativeError::win32(ERROR_FILE_INVALID)
                    : reopened.nativeError};
    }
    auto security = trustedSecurityState(stable);
    if (!security.value.has_value()) {
        return {std::nullopt, security.errorCode, security.nativeError};
    }
    if (*security.value != stable.securityState) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.security_changed"),
                SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
    }
    return {true, {}, {}};
}

SandboxValueResult<StablePath> openTrustedPath(const QString &path,
                                               const bool directory)
{
    auto stable = openStablePath(path, directory);
    if (!stable.value.has_value()) {
        return stable;
    }
    auto trusted = trustedSecurityState(*stable.value);
    if (!trusted.value.has_value()) {
        return {std::nullopt, trusted.errorCode, trusted.nativeError};
    }
    stable.value->securityState = std::move(*trusted.value);
    return stable;
}

} // namespace

struct SandboxTrustState final
{
    StablePath packageRoot;
    StablePath tempRoot;
    std::vector<StablePath> runtimeRoots;
    std::vector<StablePath> runtimeClosure;
};

struct SandboxLaunchState final
{
    std::shared_ptr<const SandboxTrustState> trust;
    StablePath package;
    StablePath temp;
    StablePath executable;
    QStringList runtimeResources;
};

namespace {

SandboxValueResult<bool> revalidateTrustState(const SandboxTrustState &state)
{
    const StablePath *fixed[] = {&state.packageRoot, &state.tempRoot};
    for (const StablePath *path : fixed) {
        auto checked = revalidateStable(*path);
        if (!checked.value.has_value()) {
            return checked;
        }
    }
    for (const StablePath &path : state.runtimeRoots) {
        auto checked = revalidateStable(path);
        if (!checked.value.has_value()) {
            return checked;
        }
    }
    for (const StablePath &path : state.runtimeClosure) {
        auto checked = revalidateStable(path);
        if (!checked.value.has_value()) {
            return checked;
        }
    }
    return {true, {}, {}};
}

} // namespace

SandboxLaunchConfig::SandboxLaunchConfig(
    std::shared_ptr<const SandboxLaunchState> state,
    SandboxLaunchRequest request) noexcept
    : state_(std::move(state)), request_(std::move(request))
{
}

bool SandboxLaunchConfig::isValid() const noexcept
{
    return state_ != nullptr && state_->trust != nullptr;
}

const QString &SandboxLaunchConfig::appId() const noexcept
{
    return request_.appId;
}

const QString &SandboxLaunchConfig::executablePath() const noexcept
{
    return request_.executablePath;
}

const QString &SandboxLaunchConfig::packageDirectory() const noexcept
{
    return request_.packageDirectory;
}

const QString &SandboxLaunchConfig::tempDirectory() const noexcept
{
    return request_.tempDirectory;
}

const QStringList &SandboxLaunchConfig::runtimeResources() const noexcept
{
    static const QStringList empty;
    return state_ != nullptr ? state_->runtimeResources : empty;
}

const QStringList &SandboxLaunchConfig::arguments() const noexcept
{
    return request_.arguments;
}

const SandboxResourceLimits &SandboxLaunchConfig::resourceLimits() const noexcept
{
    return request_.resourceLimits;
}

SandboxValueResult<bool> SandboxLaunchConfig::revalidateTrust() const
{
    if (!isValid()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.config_invalid"),
                SandboxNativeError::win32(ERROR_INVALID_PARAMETER)};
    }
    auto roots = revalidateTrustState(*state_->trust);
    if (!roots.value.has_value()) {
        return roots;
    }
    const StablePath *selected[] = {
        &state_->package, &state_->temp, &state_->executable};
    for (const StablePath *path : selected) {
        auto checked = revalidateStable(*path);
        if (!checked.value.has_value()) {
            return checked;
        }
    }
    return {true, {}, {}};
}

SandboxValueResult<SandboxTrustBoundary> SandboxTrustBoundary::create(
    const SandboxApprovedRoots &roots)
{
    if (roots.immutableRuntimeRoots.isEmpty()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.runtime_roots_empty"),
                SandboxNativeError::win32(ERROR_INVALID_PARAMETER)};
    }
    QStringList requestedRoots{roots.packageStoreRoot, roots.sandboxTempRoot};
    requestedRoots.append(roots.immutableRuntimeRoots);
    for (const QString &root : requestedRoots) {
        if (root.isEmpty() || isBroadRoot(root)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.trust.root_too_broad"),
                    SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
        }
    }

    auto state = std::make_shared<SandboxTrustState>();
    auto package = openTrustedPath(roots.packageStoreRoot, true);
    if (!package.value.has_value()) {
        return {std::nullopt, package.errorCode, package.nativeError};
    }
    auto temp = openTrustedPath(roots.sandboxTempRoot, true);
    if (!temp.value.has_value()) {
        return {std::nullopt, temp.errorCode, temp.nativeError};
    }
    if (isBroadRoot(package.value->finalPath)
        || isBroadRoot(temp.value->finalPath)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.root_too_broad"),
                SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
    }
    state->packageRoot = std::move(*package.value);
    state->tempRoot = std::move(*temp.value);
    for (const QString &runtimeRoot : roots.immutableRuntimeRoots) {
        auto opened = openTrustedPath(runtimeRoot, true);
        if (!opened.value.has_value()) {
            return {std::nullopt, opened.errorCode, opened.nativeError};
        }
        if (isBroadRoot(opened.value->finalPath)) {
            return {std::nullopt,
                    QStringLiteral("sandbox.trust.root_too_broad"),
                    SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
        }
        state->runtimeRoots.push_back(std::move(*opened.value));
    }

    std::vector<const StablePath *> stableRoots{
        &state->packageRoot, &state->tempRoot};
    for (const StablePath &root : state->runtimeRoots) {
        stableRoots.push_back(&root);
    }
    for (size_t left = 0; left < stableRoots.size(); ++left) {
        for (size_t right = left + 1; right < stableRoots.size(); ++right) {
            if (pathWithinOrEqual(stableRoots[left]->finalPath,
                                  stableRoots[right]->finalPath)
                || pathWithinOrEqual(stableRoots[right]->finalPath,
                                     stableRoots[left]->finalPath)) {
                return {std::nullopt,
                        QStringLiteral("sandbox.trust.roots_overlap"),
                        SandboxNativeError::win32(ERROR_INVALID_PARAMETER)};
            }
        }
    }

    for (const StablePath &runtimeRoot : state->runtimeRoots) {
        QDirIterator iterator(runtimeRoot.requestedPath,
                              QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
                                  | QDir::Hidden | QDir::System,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString child = iterator.next();
            const bool directory = iterator.fileInfo().isDir();
            auto opened = openTrustedPath(child, directory);
            if (!opened.value.has_value()) {
                return {std::nullopt, opened.errorCode, opened.nativeError};
            }
            if (!strictDescendant(runtimeRoot.finalPath,
                                  opened.value->finalPath)) {
                return {std::nullopt,
                        QStringLiteral("sandbox.trust.runtime_escape"),
                        SandboxNativeError::win32(ERROR_ACCESS_DENIED)};
            }
            state->runtimeClosure.push_back(std::move(*opened.value));
        }
    }
    return {SandboxTrustBoundary(std::move(state)), {}, {}};
}

bool SandboxTrustBoundary::isValid() const noexcept
{
    return state_ != nullptr;
}

SandboxValueResult<SandboxLaunchConfig>
SandboxTrustBoundary::makeLaunchConfig(
    const SandboxLaunchRequest &request) const
{
    if (!isValid()) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.boundary_invalid"),
                SandboxNativeError::win32(ERROR_INVALID_HANDLE)};
    }
    auto roots = revalidateTrustState(*state_);
    if (!roots.value.has_value()) {
        return {std::nullopt, roots.errorCode, roots.nativeError};
    }
    auto package = openTrustedPath(request.packageDirectory, true);
    if (!package.value.has_value()
        || !strictDescendant(state_->packageRoot.finalPath,
                             package.value->finalPath)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.package_outside_root"),
                package.value.has_value()
                    ? SandboxNativeError::win32(ERROR_ACCESS_DENIED)
                    : package.nativeError};
    }
    auto temp = openTrustedPath(request.tempDirectory, true);
    if (!temp.value.has_value()
        || !strictDescendant(state_->tempRoot.finalPath,
                             temp.value->finalPath)) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.temp_outside_root"),
                temp.value.has_value()
                    ? SandboxNativeError::win32(ERROR_ACCESS_DENIED)
                    : temp.nativeError};
    }
    auto executable = openTrustedPath(request.executablePath, false);
    bool executableTrusted = false;
    if (executable.value.has_value()) {
        executableTrusted = std::any_of(
            state_->runtimeRoots.cbegin(),
            state_->runtimeRoots.cend(),
            [&executable](const StablePath &runtime) {
                return strictDescendant(runtime.finalPath,
                                        executable.value->finalPath);
            });
    }
    if (!executable.value.has_value() || !executableTrusted) {
        return {std::nullopt,
                QStringLiteral("sandbox.trust.executable_outside_runtime"),
                executable.value.has_value()
                    ? SandboxNativeError::win32(ERROR_ACCESS_DENIED)
                    : executable.nativeError};
    }

    auto launchState = std::make_shared<SandboxLaunchState>();
    launchState->trust = state_;
    launchState->package = std::move(*package.value);
    launchState->temp = std::move(*temp.value);
    launchState->executable = std::move(*executable.value);
    for (const StablePath &runtime : state_->runtimeRoots) {
        launchState->runtimeResources.append(runtime.requestedPath);
    }
    for (const StablePath &runtime : state_->runtimeClosure) {
        launchState->runtimeResources.append(runtime.requestedPath);
    }
    SandboxLaunchRequest normalized = request;
    normalized.executablePath = launchState->executable.requestedPath;
    normalized.packageDirectory = launchState->package.requestedPath;
    normalized.tempDirectory = launchState->temp.requestedPath;
    return {SandboxLaunchConfig(std::move(launchState),
                                std::move(normalized)),
            {},
            {}};
}

SandboxTrustBoundary::SandboxTrustBoundary(
    std::shared_ptr<const SandboxTrustState> state) noexcept
    : state_(std::move(state))
{
}
