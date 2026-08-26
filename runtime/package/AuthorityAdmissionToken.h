#pragma once

#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>
#include <thread>

class AppRuntimeCoordinator;
struct AuthorityAdmissionState;

class AuthorityAdmissionToken final
{
public:
    class UseGuard final
    {
    public:
        ~UseGuard();
        UseGuard(const UseGuard &) = delete;
        UseGuard &operator=(const UseGuard &) = delete;
        UseGuard(UseGuard &&other) noexcept;
        UseGuard &operator=(UseGuard &&other) noexcept;

        // This callback is the final non-blocking in-memory publication or
        // owned-buffer submission. It must not wait, yield, call lifecycle, or
        // invoke application-controlled code.
        [[nodiscard]] bool publishIfStillAdmitted(
            const std::function<bool()> &nonBlockingCommit);

    private:
        friend class AuthorityAdmissionToken;
        explicit UseGuard(std::shared_ptr<AuthorityAdmissionState> state);
        void release() noexcept;

        std::shared_ptr<AuthorityAdmissionState> state_;
        bool active_ = false;
    };

    class RevocationTicket final
    {
    public:
        RevocationTicket(const RevocationTicket &) = default;
        RevocationTicket &operator=(const RevocationTicket &) = default;
        RevocationTicket(RevocationTicket &&) noexcept = default;
        RevocationTicket &operator=(RevocationTicket &&) noexcept = default;

        [[nodiscard]] bool waitUntil(qint64 monotonicDeadlineMs) const;
        void waitUntilDrained() const;
        [[nodiscard]] bool isDrained() const noexcept;
        [[nodiscard]] bool waitAllowedOnCurrentThread() const noexcept;

    private:
        friend class AuthorityAdmissionToken;
        explicit RevocationTicket(
            std::shared_ptr<AuthorityAdmissionState> state,
            std::thread::id waitForbiddenThread);

        std::shared_ptr<AuthorityAdmissionState> state_;
        std::thread::id waitForbiddenThread_;
    };

    AuthorityAdmissionToken();
    ~AuthorityAdmissionToken();
    AuthorityAdmissionToken(const AuthorityAdmissionToken &) = delete;
    AuthorityAdmissionToken &operator=(const AuthorityAdmissionToken &) = delete;
    AuthorityAdmissionToken(AuthorityAdmissionToken &&) = delete;
    AuthorityAdmissionToken &operator=(AuthorityAdmissionToken &&) = delete;

    [[nodiscard]] std::optional<UseGuard> tryAcquireUse();
    [[nodiscard]] RevocationTicket beginRevoke();

private:
    friend class AppRuntimeCoordinator;

    std::shared_ptr<AuthorityAdmissionState> state_;
};
