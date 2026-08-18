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
                   QString &finalPath)
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(target,
                                      FileAttributeTagInfo,
                                      &attributes,
                                      sizeof(attributes))
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }
    directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (!directory && GetFileType(target) != FILE_TYPE_DISK) {
        return false;
    }
    const DWORD required = GetFinalPathNameByHandleW(
        target, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0
        || required > static_cast<DWORD>(std::numeric_limits<int>::max())) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1U);
    const DWORD length = GetFinalPathNameByHandleW(
        target,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    finalPath = QString::fromWCharArray(buffer.data(), length);
    return true;
}

DWORD accessMask(const SandboxPathAccess access, const bool directory)
{
    DWORD mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    if (access == SandboxPathAccess::ReadWrite) {
        mask |= FILE_GENERIC_WRITE | DELETE;
        if (directory) {
            mask |= FILE_DELETE_CHILD;
        }
    }
    return mask;
}

bool restoreDescriptor(const HANDLE target, const QByteArray &security) noexcept
{
    if (!validHandle(target) || security.isEmpty()) {
        return false;
    }
    auto *descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<char *>(security.constData()));
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    return GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)
            != FALSE
        && SetSecurityInfo(target,
                           SE_FILE_OBJECT,
                           DACL_SECURITY_INFORMATION,
                           nullptr,
                           nullptr,
                           present != FALSE ? dacl : nullptr,
                           nullptr) == ERROR_SUCCESS;
}

} // namespace

AclGrant::~AclGrant()
{
    close();
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
        close();
        target_ = std::exchange(other.target_, INVALID_HANDLE_VALUE);
        originalSecurity_ = std::move(other.originalSecurity_);
        finalPath_ = std::move(other.finalPath_);
    }
    return *this;
}

std::optional<AclGrant> AclGrant::apply(const QString &path,
                                        PSID appContainerSid,
                                        const SandboxPathAccess access,
                                        const bool inheritToChildren)
{
    if (path.isEmpty() || appContainerSid == nullptr
        || IsValidSid(appContainerSid) == FALSE) {
        return std::nullopt;
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
    if (!validHandle(target) || !inspectTarget(target, directory, finalPath)
        || (inheritToChildren && !directory)) {
        if (validHandle(target)) {
            CloseHandle(target);
        }
        return std::nullopt;
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
        return std::nullopt;
    }
    const DWORD descriptorLength = GetSecurityDescriptorLength(descriptor);
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
        return std::nullopt;
    }
    return AclGrant(target, originalSecurity, finalPath);
}

bool AclGrant::isValid() const noexcept
{
    return validHandle(target_) && !originalSecurity_.isEmpty();
}

const QString &AclGrant::finalPath() const noexcept
{
    return finalPath_;
}

bool AclGrant::restore() noexcept
{
    if (!isValid()) {
        return false;
    }
    const bool restored = restoreDescriptor(target_, originalSecurity_);
    if (restored) {
        originalSecurity_.clear();
    }
    return restored;
}

AclGrant::AclGrant(HANDLE target,
                   QByteArray originalSecurity,
                   QString finalPath) noexcept
    : target_(target),
      originalSecurity_(std::move(originalSecurity)),
      finalPath_(std::move(finalPath))
{
}

void AclGrant::close() noexcept
{
    if (validHandle(target_)) {
        if (!originalSecurity_.isEmpty()) {
            (void)restoreDescriptor(target_, originalSecurity_);
        }
        CloseHandle(target_);
    }
    target_ = INVALID_HANDLE_VALUE;
    originalSecurity_.clear();
    finalPath_.clear();
}
