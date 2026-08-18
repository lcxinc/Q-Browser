#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <sddl.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::wstring argumentValue(const int argc,
                           wchar_t **argv,
                           const std::wstring &name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return {};
}

HANDLE handleArgument(const int argc,
                      wchar_t **argv,
                      const std::wstring &name)
{
    const std::wstring value = argumentValue(argc, argv, name);
    if (value.empty()) {
        return nullptr;
    }
    wchar_t *end = nullptr;
    const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 10);
    return end != nullptr && *end == L'\0'
        ? reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parsed))
        : nullptr;
}

bool canReadFile(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    char byte = 0;
    DWORD read = 0;
    const bool ok = ReadFile(file, &byte, 1, &read, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

bool canWriteFile(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr char value[] = "sandbox-write";
    DWORD written = 0;
    const bool ok = WriteFile(file,
                              value,
                              static_cast<DWORD>(sizeof(value) - 1U),
                              &written,
                              nullptr) != FALSE
        && written == sizeof(value) - 1U;
    CloseHandle(file);
    return ok;
}

bool canOpenFileForExecute(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_EXECUTE,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

bool canConnectLoopback(const unsigned short port, int &error)
{
    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        error = startupResult;
        return false;
    }
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        error = WSAGetLastError();
        WSACleanup();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0) {
        error = WSAGetLastError();
        closesocket(socketHandle);
        WSACleanup();
        return false;
    }
    bool connected = connect(socketHandle,
                             reinterpret_cast<const sockaddr *>(&address),
                             sizeof(address)) == 0;
    error = connected ? 0 : WSAGetLastError();
    if (!connected && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS)) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socketHandle, &writable);
        fd_set failed;
        FD_ZERO(&failed);
        FD_SET(socketHandle, &failed);
        timeval timeout{};
        timeout.tv_sec = 1;
        const int selected = select(0, nullptr, &writable, &failed, &timeout);
        int socketError = WSAETIMEDOUT;
        int socketErrorSize = sizeof(socketError);
        if (selected > 0
            && getsockopt(socketHandle,
                          SOL_SOCKET,
                          SO_ERROR,
                          reinterpret_cast<char *>(&socketError),
                          &socketErrorSize) == 0) {
            connected = socketError == 0 && FD_ISSET(socketHandle, &writable);
            error = socketError;
        } else if (selected == 0) {
            error = WSAETIMEDOUT;
        } else {
            error = WSAGetLastError();
        }
    }
    if (!connected && error == 0) {
        error = WSAEACCES;
    }
    closesocket(socketHandle);
    WSACleanup();
    return connected;
}

std::vector<wchar_t> mutableCommandLine(const std::wstring &command)
{
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    return buffer;
}

bool canCreateProcess(const std::wstring &application,
                      const std::wstring &command)
{
    std::vector<wchar_t> commandLine = mutableCommandLine(command);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(application.c_str(),
                                        commandLine.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &process) != FALSE;
    if (created) {
        WaitForSingleObject(process.hProcess, 2000);
        TerminateProcess(process.hProcess, 70);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return created;
}

bool tokenInformationHasEnabledSid(HANDLE token,
                                   TOKEN_INFORMATION_CLASS informationClass,
                                   PSID sid,
                                   bool &isMember)
{
    DWORD bytes = 0;
    GetTokenInformation(token, informationClass, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return false;
    }
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token,
                             informationClass,
                             buffer.data(),
                             bytes,
                             &bytes)) {
        return false;
    }
    const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(buffer.data());
    isMember = false;
    for (DWORD index = 0; index < groups->GroupCount; ++index) {
        const SID_AND_ATTRIBUTES &group = groups->Groups[index];
        if (EqualSid(group.Sid, sid)
            && (group.Attributes & SE_GROUP_ENABLED) != 0U) {
            isMember = true;
            break;
        }
    }
    return true;
}

bool tokenHasEnabledSid(HANDLE token,
                        const wchar_t *sidText,
                        bool &isMember,
                        bool &groupsQueried,
                        bool &restrictedSidsQueried)
{
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText, &sid)) {
        return false;
    }
    bool regularMember = false;
    bool restrictedMember = false;
    groupsQueried = tokenInformationHasEnabledSid(
        token, TokenGroups, sid, regularMember);
    restrictedSidsQueried = tokenInformationHasEnabledSid(
        token, TokenRestrictedSids, sid, restrictedMember);
    LocalFree(sid);
    isMember = groupsQueried && restrictedSidsQueried
        && (regularMember || restrictedMember);
    return groupsQueried && restrictedSidsQueried;
}

bool tokenIsAppContainer(DWORD &capabilityCount,
                         bool &capabilitiesQueried,
                         std::wstring &sidText,
                         bool &lessPrivileged,
                         DWORD &lessPrivilegedError,
                         bool &tokenGroupsQueried,
                         bool &tokenRestrictedSidsQueried,
                         bool &allApplicationPackagesQueried,
                         bool &allApplicationPackagesMember,
                         bool &allRestrictedApplicationPackagesMember)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD appContainer = 0;
    DWORD size = 0;
    const bool queried = GetTokenInformation(token,
                                             TokenIsAppContainer,
                                             &appContainer,
                                             sizeof(appContainer),
                                             &size) != FALSE;
    DWORD lessPrivilegedValue = 0;
    DWORD lessPrivilegedSize = 0;
    const bool lessPrivilegedQueried = GetTokenInformation(
        token,
        TokenIsLessPrivilegedAppContainer,
        &lessPrivilegedValue,
        sizeof(lessPrivilegedValue),
        &lessPrivilegedSize) != FALSE;
    lessPrivilegedError = lessPrivilegedQueried ? ERROR_SUCCESS : GetLastError();
    allApplicationPackagesQueried = tokenHasEnabledSid(
        token,
        L"S-1-15-2-1",
        allApplicationPackagesMember,
        tokenGroupsQueried,
        tokenRestrictedSidsQueried);
    bool restrictedGroupsQueried = false;
    bool restrictedSidsQueried = false;
    (void)tokenHasEnabledSid(token,
                             L"S-1-15-2-2",
                             allRestrictedApplicationPackagesMember,
                             restrictedGroupsQueried,
                             restrictedSidsQueried);
    // TokenIsLessPrivilegedAppContainer is unavailable on some supported
    // Windows builds. LPAC's defining access-token property is that it does
    // not satisfy ALL APPLICATION PACKAGES, so use that property only as the
    // down-level fallback after the direct query reports unsupported.
    lessPrivileged = lessPrivilegedQueried
        ? lessPrivilegedValue != 0
        : lessPrivilegedError == ERROR_INVALID_PARAMETER
            && allApplicationPackagesQueried
            && !allApplicationPackagesMember;
    DWORD capabilitiesSize = 0;
    SetLastError(ERROR_SUCCESS);
    const bool capabilitiesSized = GetTokenInformation(
        token, TokenCapabilities, nullptr, 0, &capabilitiesSize) == FALSE
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER
        && capabilitiesSize >= sizeof(DWORD);
    std::vector<unsigned char> capabilities(capabilitiesSize);
    if (capabilitiesSized
        && GetTokenInformation(token,
                               TokenCapabilities,
                               capabilities.data(),
                               capabilitiesSize,
                               &capabilitiesSize)) {
        capabilitiesQueried = true;
        capabilityCount = reinterpret_cast<TOKEN_GROUPS *>(capabilities.data())->GroupCount;
    }
    DWORD sidSize = 0;
    GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &sidSize);
    std::vector<unsigned char> sidInformation(sidSize);
    if (sidSize > 0
        && GetTokenInformation(token,
                               TokenAppContainerSid,
                               sidInformation.data(),
                               sidSize,
                               &sidSize)) {
        const auto *information = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(
            sidInformation.data());
        LPWSTR converted = nullptr;
        if (information->TokenAppContainer != nullptr
            && ConvertSidToStringSidW(information->TokenAppContainer, &converted)) {
            sidText = converted;
            LocalFree(converted);
        }
    }
    CloseHandle(token);
    return queried && appContainer != 0;
}

std::string narrowAscii(const std::wstring &value)
{
    std::string converted;
    converted.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            return {};
        }
        converted.push_back(static_cast<char>(character));
    }
    return converted;
}

bool writeAll(const HANDLE handle, const char *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const DWORD request = static_cast<DWORD>(size - offset);
        DWORD written = 0;
        if (!WriteFile(handle, data + offset, request, &written, nullptr)
            || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool sendJsonFrame(const HANDLE handle, const std::string &json)
{
    const uint32_t length = static_cast<uint32_t>(json.size());
    const char header[] = {
        static_cast<char>((length >> 24U) & 0xffU),
        static_cast<char>((length >> 16U) & 0xffU),
        static_cast<char>((length >> 8U) & 0xffU),
        static_cast<char>(length & 0xffU)};
    return writeAll(handle, header, sizeof(header))
        && writeAll(handle, json.data(), json.size());
}

std::string jsonBoolean(const bool value)
{
    return value ? "true" : "false";
}

} // namespace

int wmain(const int argc, wchar_t **argv)
{
    for (int index = 1; index < argc; ++index) {
        if (std::wstring(argv[index]) == L"--child-smoke") {
            return 0;
        }
        if (std::wstring(argv[index]) == L"--wait-forever") {
            Sleep(INFINITE);
        }
    }

    const std::wstring packageFile = argumentValue(argc, argv, L"--package-file");
    const std::wstring tempFile = argumentValue(argc, argv, L"--temp-file");
    const std::wstring loopbackPort = argumentValue(argc, argv, L"--loopback-port");
    const std::wstring selfPath = argumentValue(argc, argv, L"--self-path");
    const std::wstring privateFile = argumentValue(argc, argv, L"--private-file");
    const HANDLE ipcWrite = handleArgument(argc,
                                           argv,
                                           L"--qbrowser-ipc-write-handle");
    const HANDLE ipcRead = handleArgument(argc,
                                          argv,
                                          L"--qbrowser-ipc-read-handle");
    if (packageFile.empty() || tempFile.empty() || loopbackPort.empty()
        || selfPath.empty() || privateFile.empty() || ipcWrite == nullptr
        || ipcRead == nullptr) {
        return 80;
    }
    wchar_t environmentSecret[2]{};
    const bool environmentSecretPresent = GetEnvironmentVariableW(
        L"Q_BROWSER_SANDBOX_SENTINEL_SECRET",
        environmentSecret,
        static_cast<DWORD>(sizeof(environmentSecret)
                           / sizeof(environmentSecret[0]))) != 0;

    wchar_t *portEnd = nullptr;
    const unsigned long portValue = std::wcstoul(loopbackPort.c_str(), &portEnd, 10);
    int networkError = 0;
    const bool packageRead = canReadFile(packageFile);
    const bool tempWrite = canWriteFile(tempFile);
    const bool packageExecuteOpen = canOpenFileForExecute(packageFile);
    const bool tempExecuteOpen = canOpenFileForExecute(tempFile);
    const bool runtimeExecuteOpen = canOpenFileForExecute(selfPath);
    const bool privateRead = canReadFile(privateFile);
    const bool windowsRead = canReadFile(L"C:\\Windows\\win.ini");
    const bool networkAttempted = portEnd != nullptr && *portEnd == L'\0'
        && portValue > 0 && portValue <= 65535U;
    const bool loopbackConnect = networkAttempted
        && canConnectLoopback(static_cast<unsigned short>(portValue), networkError);
    const bool cmdCreate = canCreateProcess(
        L"C:\\Windows\\System32\\cmd.exe",
        L"\"C:\\Windows\\System32\\cmd.exe\" /d /c exit 0");
    const bool selfCreate = canCreateProcess(
        selfPath, L"\"" + selfPath + L"\" --child-smoke");
    DWORD capabilityCount = 0;
    bool capabilitiesQueried = false;
    std::wstring sidText;
    bool lessPrivileged = false;
    DWORD lessPrivilegedError = ERROR_SUCCESS;
    bool allApplicationPackagesMember = false;
    bool allRestrictedApplicationPackagesMember = false;
    bool tokenGroupsQueried = false;
    bool tokenRestrictedSidsQueried = false;
    bool allApplicationPackagesQueried = false;
    const bool appContainer = tokenIsAppContainer(
        capabilityCount,
        capabilitiesQueried,
        sidText,
        lessPrivileged,
        lessPrivilegedError,
        tokenGroupsQueried,
        tokenRestrictedSidsQueried,
        allApplicationPackagesQueried,
        allApplicationPackagesMember,
        allRestrictedApplicationPackagesMember);
    BOOL inJob = FALSE;
    const bool jobBound = IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) != FALSE
        && inJob != FALSE;

    const std::string json = std::string("{")
        + "\"packageRead\":" + jsonBoolean(packageRead)
        + ",\"tempWrite\":" + jsonBoolean(tempWrite)
        + ",\"packageExecuteOpen\":" + jsonBoolean(packageExecuteOpen)
        + ",\"tempExecuteOpen\":" + jsonBoolean(tempExecuteOpen)
        + ",\"runtimeExecuteOpen\":" + jsonBoolean(runtimeExecuteOpen)
        + ",\"privateRead\":" + jsonBoolean(privateRead)
        + ",\"windowsRead\":" + jsonBoolean(windowsRead)
        + ",\"networkAttempted\":" + jsonBoolean(networkAttempted)
        + ",\"loopbackConnect\":" + jsonBoolean(loopbackConnect)
        + ",\"networkError\":" + std::to_string(networkError)
        + ",\"cmdCreate\":" + jsonBoolean(cmdCreate)
        + ",\"selfCreate\":" + jsonBoolean(selfCreate)
        + ",\"appContainer\":" + jsonBoolean(appContainer)
        + ",\"lessPrivileged\":" + jsonBoolean(lessPrivileged)
        + ",\"lessPrivilegedError\":" + std::to_string(lessPrivilegedError)
        + ",\"allApplicationPackagesMember\":"
        + jsonBoolean(allApplicationPackagesMember)
        + ",\"tokenGroupsQueried\":" + jsonBoolean(tokenGroupsQueried)
        + ",\"tokenRestrictedSidsQueried\":"
        + jsonBoolean(tokenRestrictedSidsQueried)
        + ",\"allApplicationPackagesQueried\":"
        + jsonBoolean(allApplicationPackagesQueried)
        + ",\"allRestrictedApplicationPackagesMember\":"
        + jsonBoolean(allRestrictedApplicationPackagesMember)
        + ",\"capabilitiesQueried\":" + jsonBoolean(capabilitiesQueried)
        + ",\"capabilityCount\":" + std::to_string(capabilityCount)
        + ",\"environmentSecretPresent\":"
        + jsonBoolean(environmentSecretPresent)
        + ",\"jobBound\":" + jsonBoolean(jobBound)
        + ",\"appContainerSid\":\"" + narrowAscii(sidText) + "\"}";
    if (!sendJsonFrame(ipcWrite, json)) {
        return 81;
    }
    char release = 0;
    DWORD read = 0;
    return ReadFile(ipcRead, &release, sizeof(release), &read, nullptr) != FALSE
            && read == sizeof(release)
        ? 0
        : 83;
}
