#include "AclGrant.h"

#include <QDir>
#include <QFileInfo>

#include <aclapi.h>

#include <limits>
#include <utility>
#include <vector>

namespace {

bool validHandle(const HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

QString absoluteNativePath(const QString &path)
{
    return QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

bool inspectTarget(const HANDLE target,
                   bool &directory,
                   QString &finalPath,
                   DWORD &error)
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(target,
                                      FileAttributeTagInfo,
                                      &attributes,
                                      sizeof(attributes))) {
        error = GetLastError();
        return false;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        error = ERROR_REPARSE_TAG_INVALID;
        return false;
    }
    directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (!directory && GetFileType(target) != FILE_TYPE_DISK) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    const DWORD required = GetFinalPathNameByHandleW(
        target, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0
        || required > static_cast<DWORD>(std::numeric_limits<int>::max())) {
        error = required == 0 ? GetLastError() : ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1U);
    const DWORD length = GetFinalPathNameByHandleW(
        target,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    finalPath = QString::fromWCharArray(buffer.data(), length);
    error = ERROR_SUCCESS;
    return true;
}

DWORD accessMask(const SandboxPathAccess access, const bool directory)
{
    DWORD mask = FILE_GENERIC_READ;
    if (access == SandboxPathAccess::ReadExecute) {
        mask |= FILE_GENERIC_EXECUTE;
    }
    if (access == SandboxPathAccess::ReadWrite) {
        mask |= FILE_GENERIC_WRITE | DELETE;
        if (directory) {
            mask |= FILE_DELETE_CHILD;
        }
    }
    return mask;
}

DWORD restoreDescriptor(const HANDLE target, const QByteArray &security) noexcept
{
    if (!validHandle(target)) {
        return ERROR_INVALID_HANDLE;
    }
    if (security.isEmpty()) {
        return ERROR_INVALID_SECURITY_DESCR;
    }
    auto *descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<char *>(security.constData()));
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor,
                                   &present,
                                   &dacl,
                                   &defaulted)) {
        const DWORD error = GetLastError();
        return error != ERROR_SUCCESS ? error : ERROR_INVALID_SECURITY_DESCR;
    }
    return SetSecurityInfo(target,
                           SE_FILE_OBJECT,
                           DACL_SECURITY_INFORMATION,
                           nullptr,
                           nullptr,
                           present != FALSE ? dacl : nullptr,
                           nullptr);
}

} // namespace

AclGrant::~AclGrant()
{
    closeBestEffort();
}

AclGrant::AclGrant(AclGrant &&other) noexcept
    : target_(std::exchange(other.target_, INVALID_HANDLE_VALUE)),
      originalSecurity_(std::move(other.originalSecurity_)),
      finalPath_(std::move(other.finalPath_))
{
}

AclGrant &AclGrant::operator=(AclGrant &&other) noexcept
{
    if (this != &other) {
        closeBestEffort();
        target_ = std::exchange(other.target_, INVALID_HANDLE_VALUE);
        originalSecurity_ = std::move(other.originalSecurity_);
        finalPath_ = std::move(other.finalPath_);
    }
    return *this;
}

SandboxValueResult<AclGrant> AclGrant::apply(
    const QString &path,
    PSID appContainerSid,
    const SandboxPathAccess access,
    const bool inheritToChildren)
{
    if (path.isEmpty()) {
        return {std::nullopt,
                QStringLiteral("sandbox.acl.invalid_path"),
                SandboxNativeError::win32(ERROR_INVALID_PARAMETER)};
    }
    if (appContainerSid == nullptr
        || IsValidSid(appContainerSid) == FALSE) {
        return {std::nullopt,
                QStringLiteral("sandbox.acl.invalid_sid"),
                SandboxNativeError::win32(ERROR_INVALID_SID)};
    }
    const QString normalized = absoluteNativePath(path);
    HANDLE target = CreateFileW(
        reinterpret_cast<LPCWSTR>(normalized.utf16()),
        READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    bool directory = false;
    QString finalPath;
    DWORD inspectError = ERROR_SUCCESS;
    if (!validHandle(target)) {
        const DWORD openError = GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.acl.open_failed"),
                SandboxNativeError::win32(openError)};
    }
    if (!inspectTarget(target, directory, finalPath, inspectError)
        || (inheritToChildren && !directory)) {
        if (validHandle(target)) {
            CloseHandle(target);
        }
        return {std::nullopt,
                QStringLiteral("sandbox.acl.target_invalid"),
                SandboxNativeError::win32(
                    inspectError != ERROR_SUCCESS
                        ? inspectError
                        : ERROR_DIRECTORY)};
    }

    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = GetSecurityInfo(target,
                                                 SE_FILE_OBJECT,
                                                 DACL_SECURITY_INFORMATION,
                                                 nullptr,
                                                 nullptr,
                                                 &existingDacl,
                                                 nullptr,
                                                 &descriptor);
    if (securityResult != ERROR_SUCCESS || descriptor == nullptr) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        CloseHandle(target);
        return {std::nullopt,
                QStringLiteral("sandbox.acl.read_security_failed"),
                SandboxNativeError::win32(
                    securityResult != ERROR_SUCCESS
                        ? securityResult
                        : ERROR_INVALID_SECURITY_DESCR)};
    }
    const DWORD descriptorLength = GetSecurityDescriptorLength(descriptor);
    if (descriptorLength == 0) {
        LocalFree(descriptor);
        CloseHandle(target);
        return {std::nullopt,
                QStringLiteral("sandbox.acl.read_security_failed"),
                SandboxNativeError::win32(ERROR_INVALID_SECURITY_DESCR)};
    }
    const QByteArray originalSecurity(static_cast<const char *>(descriptor),
                                      descriptorLength);

    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = accessMask(access, directory);
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = inheritToChildren
        ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
        : NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(appContainerSid);

    PACL updatedDacl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(
        1, &entry, existingDacl, &updatedDacl);
    const DWORD applyResult = aclResult == ERROR_SUCCESS
        ? SetSecurityInfo(target,
                          SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION,
                          nullptr,
                          nullptr,
                          updatedDacl,
                          nullptr)
        : aclResult;
    if (updatedDacl != nullptr) {
        LocalFree(updatedDacl);
    }
    LocalFree(descriptor);
    if (applyResult != ERROR_SUCCESS) {
        CloseHandle(target);
        return {std::nullopt,
                QStringLiteral("sandbox.acl.apply_failed"),
                SandboxNativeError::win32(applyResult)};
    }
    return {AclGrant(target, originalSecurity, finalPath), {}, {}};
}

bool AclGrant::isValid() const noexcept
{
    return validHandle(target_) && !originalSecurity_.isEmpty();
}

const QString &AclGrant::finalPath() const noexcept
{
    return finalPath_;
}

SandboxValueResult<bool> AclGrant::restore() noexcept
{
    if (!isValid()) {
        return {std::nullopt,
                QStringLiteral("sandbox.acl.restore_failed"),
                SandboxNativeError::win32(ERROR_INVALID_HANDLE)};
    }
    const DWORD restored = restoreDescriptor(target_, originalSecurity_);
    if (restored != ERROR_SUCCESS) {
        return {std::nullopt,
                QStringLiteral("sandbox.acl.restore_failed"),
                SandboxNativeError::win32(restored)};
    }
    originalSecurity_.clear();
    return {true, {}, {}};
}

SandboxValueResult<bool> AclGrant::close() noexcept
{
    if (!validHandle(target_)) {
        return {true, {}, {}};
    }
    if (!originalSecurity_.isEmpty()) {
        const auto restored = restore();
        if (!restored.value.has_value()) {
            return restored;
        }
    }
    if (!CloseHandle(target_)) {
        const DWORD error = GetLastError();
        return {std::nullopt,
                QStringLiteral("sandbox.acl.close_failed"),
                SandboxNativeError::win32(error)};
    }
    target_ = INVALID_HANDLE_VALUE;
    finalPath_.clear();
    return {true, {}, {}};
}

AclGrant::AclGrant(HANDLE target,
                   QByteArray originalSecurity,
                   QString finalPath) noexcept
    : target_(target),
      originalSecurity_(std::move(originalSecurity)),
      finalPath_(std::move(finalPath))
{
}

void AclGrant::closeBestEffort() noexcept
{
    (void)close();
}
