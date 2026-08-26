#include "AuthorityAdmissionToken.h"

#include <QCoreApplication>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

struct AuthorityAdmissionState final
{
    std::mutex publicationMutex;
    std::condition_variable drained;
    std::thread::id revocationThread;
    quint64 activeUses = 0;
    bool closed = false;
};

namespace {

bool waitingAllowed(const AuthorityAdmissionState &state,
                    const std::thread::id waitForbiddenThread) noexcept
{
    const std::thread::id current = std::this_thread::get_id();
    if (state.revocationThread == current || waitForbiddenThread == current) {
        return false;
    }
    const QCoreApplication *const application = QCoreApplication::instance();
    return application == nullptr
        || application->thread() != QThread::currentThread();
}

} // namespace

AuthorityAdmissionToken::UseGuard::UseGuard(
    std::shared_ptr<AuthorityAdmissionState> state)
    : state_(std::move(state)), active_(true)
{
}

AuthorityAdmissionToken::UseGuard::~UseGuard()
{
    release();
}

AuthorityAdmissionToken::UseGuard::UseGuard(UseGuard &&other) noexcept
    : state_(std::move(other.state_)),
      active_(std::exchange(other.active_, false))
{
}

AuthorityAdmissionToken::UseGuard &
AuthorityAdmissionToken::UseGuard::operator=(UseGuard &&other) noexcept
{
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

bool AuthorityAdmissionToken::UseGuard::publishIfStillAdmitted(
    const std::function<bool()> &nonBlockingCommit)
{
    if (!active_ || state_ == nullptr || !nonBlockingCommit) return false;
    std::lock_guard lock(state_->publicationMutex);
    if (state_->closed) return false;
    try {
        return nonBlockingCommit();
    } catch (...) {
        return false;
    }
}

void AuthorityAdmissionToken::UseGuard::release() noexcept
{
    if (!active_ || state_ == nullptr) return;
    {
        std::lock_guard lock(state_->publicationMutex);
        Q_ASSERT(state_->activeUses > 0);
        --state_->activeUses;
        if (state_->closed && state_->activeUses == 0) {
            state_->drained.notify_all();
        }
    }
    active_ = false;
    state_.reset();
}

AuthorityAdmissionToken::RevocationTicket::RevocationTicket(
    std::shared_ptr<AuthorityAdmissionState> state,
    const std::thread::id waitForbiddenThread)
    : state_(std::move(state)),
      waitForbiddenThread_(waitForbiddenThread)
{
}

bool AuthorityAdmissionToken::RevocationTicket::waitUntil(
    const qint64 monotonicDeadlineMs) const
{
    if (state_ == nullptr) return true;
    Q_ASSERT_X(waitAllowedOnCurrentThread(),
               "AuthorityAdmissionToken::RevocationTicket::waitUntil",
               "Revocation waits are forbidden on GUI and revoking lifecycle threads");
    if (!waitAllowedOnCurrentThread()) return false;
    const auto deadline = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(monotonicDeadlineMs));
    std::unique_lock lock(state_->publicationMutex);
    return state_->drained.wait_until(
        lock, deadline, [this] { return state_->activeUses == 0; });
}

void AuthorityAdmissionToken::RevocationTicket::waitUntilDrained() const
{
    if (state_ == nullptr) return;
    Q_ASSERT_X(waitAllowedOnCurrentThread(),
               "AuthorityAdmissionToken::RevocationTicket::waitUntilDrained",
               "Revocation waits are forbidden on GUI and revoking lifecycle threads");
    if (!waitAllowedOnCurrentThread()) return;
    std::unique_lock lock(state_->publicationMutex);
    state_->drained.wait(lock, [this] { return state_->activeUses == 0; });
}

bool AuthorityAdmissionToken::RevocationTicket::isDrained() const noexcept
{
    if (state_ == nullptr) return true;
    std::lock_guard lock(state_->publicationMutex);
    return state_->activeUses == 0;
}

bool AuthorityAdmissionToken::RevocationTicket::waitAllowedOnCurrentThread()
    const noexcept
{
    if (state_ == nullptr) return true;
    std::lock_guard lock(state_->publicationMutex);
    return waitingAllowed(*state_, waitForbiddenThread_);
}

AuthorityAdmissionToken::AuthorityAdmissionToken()
    : state_(std::make_shared<AuthorityAdmissionState>())
{
}

AuthorityAdmissionToken::~AuthorityAdmissionToken()
{
    if (state_ == nullptr) return;
    std::lock_guard lock(state_->publicationMutex);
    state_->closed = true;
    if (state_->revocationThread == std::thread::id{}) {
        state_->revocationThread = std::this_thread::get_id();
    }
    if (state_->activeUses == 0) state_->drained.notify_all();
}

std::optional<AuthorityAdmissionToken::UseGuard>
AuthorityAdmissionToken::tryAcquireUse()
{
    std::lock_guard lock(state_->publicationMutex);
    if (state_->closed) return std::nullopt;
    ++state_->activeUses;
    return UseGuard(state_);
}

AuthorityAdmissionToken::RevocationTicket
AuthorityAdmissionToken::beginRevoke()
{
    const std::thread::id revokingThread = std::this_thread::get_id();
    std::lock_guard lock(state_->publicationMutex);
    if (!state_->closed) {
        state_->closed = true;
        state_->revocationThread = revokingThread;
    }
    if (state_->activeUses == 0) state_->drained.notify_all();
    return RevocationTicket(state_, revokingThread);
}
