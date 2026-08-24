#include "NewTabPage.h"

#include "BrowserAddress.h"
#include "BrowserTabModel.h"

#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace {

constexpr qsizetype MaximumRecentRoutes = 16;
// Four candidates per output slot bounds GUI-thread validation work while
// leaving room for invalid and duplicate history entries.
constexpr qsizetype MaximumInspectedRecentRouteCandidates = 64;

struct FixedEntry final
{
    const char *name;
    const char *title;
    const char *address;
};

constexpr std::array<FixedEntry, 7> FixedEntries{{
    {"login", "Login", "app://pilot/login"},
    {"dashboard", "Dashboard", "app://pilot/dashboard"},
    {"orders", "Orders", "app://pilot/orders"},
    {"customers", "Customers", "app://pilot/customers"},
    {"files", "Files", "app://pilot/files"},
    {"settings", "Settings", "app://pilot/settings"},
    {"help", "Help", "app://pilot/web/help"},
}};

std::optional<QString> validatedAppAddress(const QString &address)
{
    const BrowserAddress parsed = BrowserAddress::parse(address);
    if (!parsed.isValid() || parsed.kind() != BrowserAddressKind::App
        || parsed.canonical() != address) {
        return std::nullopt;
    }
    return parsed.canonical();
}

QVector<NewTabEntry> normalizedRecentRoutes(
    const QVector<NewTabEntry> &untrustedRoutes)
{
    QVector<NewTabEntry> normalized;
    normalized.reserve(MaximumRecentRoutes);
    QSet<QString> seenAddresses;
    const qsizetype inspectedCount = std::min(
        untrustedRoutes.size(), MaximumInspectedRecentRouteCandidates);
    for (qsizetype index = 0; index < inspectedCount; ++index) {
        const NewTabEntry &entry = untrustedRoutes.at(index);
        const std::optional<QString> canonicalAddress =
            validatedAppAddress(entry.address);
        if (!canonicalAddress.has_value()
            || seenAddresses.contains(*canonicalAddress)) {
            continue;
        }
        seenAddresses.insert(*canonicalAddress);

        const std::optional<QString> title =
            BrowserTabModel::canonicalTitle(entry.title);
        if (!title.has_value()) continue;

        normalized.append({title->isEmpty() ? QStringLiteral("Untitled route")
                                            : *title,
                           *canonicalAddress});
        if (normalized.size() == MaximumRecentRoutes) break;
    }
    return normalized;
}

QString literalButtonText(const QString &title)
{
    QString literal = title;
    literal.replace(u'&', QStringLiteral("&&"));
    return literal;
}

QLabel *plainLabel(const QString &text,
                   const QString &objectName,
                   const QString &accessibleName,
                   QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setAccessibleName(accessibleName);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    return label;
}

} // namespace

NewTabPage::NewTabPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("new-tab-page"));
    setAccessibleName(QStringLiteral("Q-Browser New Tab"));
    setFocusPolicy(Qt::NoFocus);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 24, 24, 24);
    pageLayout->setSpacing(12);

    pageLayout->addWidget(plainLabel(QStringLiteral("Q-Browser"),
                                     QStringLiteral("new-tab-heading"),
                                     QStringLiteral("Q-Browser New Tab"),
                                     this));
    pageLayout->addWidget(plainLabel(QStringLiteral("Pilot"),
                                     QStringLiteral("new-tab-pilot-heading"),
                                     QStringLiteral("Pilot routes"),
                                     this));

    auto *fixedRoutes = new QWidget(this);
    fixedRoutes->setObjectName(QStringLiteral("new-tab-pilot-routes"));
    fixedRoutes->setAccessibleName(QStringLiteral("Pilot routes"));
    auto *fixedLayout = new QVBoxLayout(fixedRoutes);
    fixedLayout->setContentsMargins(0, 0, 0, 0);
    fixedLayout->setSpacing(6);
    fixedButtons_.reserve(FixedEntries.size());
    for (const FixedEntry &entry : FixedEntries) {
        const QString title = QString::fromLatin1(entry.title);
        QPushButton *const routeButton = createRouteButton(
            title,
            QString::fromLatin1(entry.address),
            QStringLiteral("new-tab-pilot-") + QString::fromLatin1(entry.name),
            QStringLiteral("Open %1").arg(title),
            fixedRoutes);
        if (routeButton != nullptr) {
            fixedLayout->addWidget(routeButton);
            fixedButtons_.append(routeButton);
        }
    }
    pageLayout->addWidget(fixedRoutes);

    pageLayout->addWidget(plainLabel(QStringLiteral("Recent"),
                                     QStringLiteral("new-tab-recent-heading"),
                                     QStringLiteral("Recent routes"),
                                     this));
    auto *recentRoutes = new QWidget(this);
    recentRoutes->setObjectName(QStringLiteral("new-tab-recent-routes"));
    recentRoutes->setAccessibleName(QStringLiteral("Recent routes"));
    recentRoutesLayout_ = new QVBoxLayout(recentRoutes);
    recentRoutesLayout_->setContentsMargins(0, 0, 0, 0);
    recentRoutesLayout_->setSpacing(6);
    pageLayout->addWidget(recentRoutes);
    pageLayout->addStretch(1);

    rebuildFocusOrder();
}

NewTabPage::~NewTabPage()
{
    recentRoutesRebuildInProgress_ = true;
    const QList<QPushButton *> childButtons = findChildren<QPushButton *>();
    for (QPushButton *const routeButton : childButtons) {
        QObject::disconnect(routeButton, nullptr, this, nullptr);
    }
}

void NewTabPage::setRecentRoutes(const QVector<NewTabEntry> &validatedRoutes)
{
    QVector<NewTabEntry> normalized = normalizedRecentRoutes(validatedRoutes);
    if (recentRoutesRebuildInProgress_) {
        pendingRecentRoutes_ = std::move(normalized);
        recentRoutesUpdatePending_ = true;
        schedulePendingRecentRoutes();
        return;
    }

    pendingRecentRoutes_.clear();
    recentRoutesUpdatePending_ = false;
    applyRecentRoutes(normalized);
}

void NewTabPage::applyRecentRoutes(
    const QVector<NewTabEntry> &normalizedRoutes)
{
    Q_ASSERT(!recentRoutesRebuildInProgress_);
    QScopedValueRollback<bool> rebuildGuard(
        recentRoutesRebuildInProgress_, true);

    QVector<QPointer<QPushButton>> previousButtons =
        std::exchange(recentButtons_, {});
    reusableRecentButtons_.erase(
        std::remove_if(reusableRecentButtons_.begin(),
                       reusableRecentButtons_.end(),
                       [](const QPointer<QPushButton> &candidate) {
                           return candidate.isNull();
                       }),
        reusableRecentButtons_.end());
    for (const QPointer<QPushButton> &candidate : previousButtons) {
        if (candidate.isNull()) continue;

        recentRoutesLayout_->removeWidget(candidate.data());
        QObject::disconnect(
            candidate.data(), &QPushButton::clicked, this, nullptr);
        candidate->hide();
        if (candidate.isNull()) continue;
        candidate->setEnabled(false);
        if (candidate.isNull()) continue;
        candidate->setFocusPolicy(Qt::NoFocus);
        if (candidate.isNull()) continue;
        candidate->setShortcut(QKeySequence());
        if (candidate.isNull()) continue;
        candidate->setText(QString());
        if (candidate.isNull()) continue;
        candidate->setAccessibleName(QString());
        if (candidate.isNull()) continue;
        candidate->setObjectName(QString());
        if (!candidate.isNull()) reusableRecentButtons_.append(candidate);
    }

    for (const NewTabEntry &entry : normalizedRoutes) {
        const qsizetype index = recentButtons_.size();
        const QPointer<QPushButton> routeButton = acquireRecentButton();
        if (!configureRouteButton(
                routeButton.data(),
                entry.title,
                entry.address,
                QStringLiteral("new-tab-recent-%1").arg(index),
                QStringLiteral("Open recent route: %1").arg(entry.title))) {
            if (!routeButton.isNull()) {
                reusableRecentButtons_.append(routeButton);
            }
            continue;
        }
        recentRoutesLayout_->addWidget(routeButton.data());
        recentButtons_.append(routeButton);
    }

    Q_ASSERT(recentButtons_.size() + reusableRecentButtons_.size()
             <= MaximumRecentRoutes);
    rebuildFocusOrder();
}

void NewTabPage::schedulePendingRecentRoutes()
{
    if (recentRoutesDispatchScheduled_) return;
    recentRoutesDispatchScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        recentRoutesDispatchScheduled_ = false;
        if (!recentRoutesUpdatePending_) return;

        QVector<NewTabEntry> pending = std::move(pendingRecentRoutes_);
        pendingRecentRoutes_.clear();
        recentRoutesUpdatePending_ = false;
        if (recentRoutesRebuildInProgress_) {
            pendingRecentRoutes_ = std::move(pending);
            recentRoutesUpdatePending_ = true;
            schedulePendingRecentRoutes();
            return;
        }
        applyRecentRoutes(pending);
    });
}

QPushButton *NewTabPage::createRouteButton(const QString &title,
                                           const QString &canonicalAddress,
                                           const QString &objectName,
                                           const QString &accessibleName,
                                           QWidget *parent)
{
    const std::optional<QString> validatedAddress =
        validatedAppAddress(canonicalAddress);
    if (!validatedAddress.has_value()) {
        return nullptr;
    }

    const QPointer<QPushButton> routeButton = new QPushButton(parent);
    const bool configured = configureRouteButton(
        routeButton.data(),
        title,
        *validatedAddress,
        objectName,
        accessibleName);
    Q_ASSERT(configured);
    if (!configured) {
        delete routeButton.data();
        return nullptr;
    }
    return routeButton.data();
}

bool NewTabPage::configureRouteButton(QPushButton *routeButton,
                                      const QString &title,
                                      const QString &canonicalAddress,
                                      const QString &objectName,
                                      const QString &accessibleName)
{
    const std::optional<QString> validatedAddress =
        validatedAppAddress(canonicalAddress);
    if (routeButton == nullptr || !validatedAddress.has_value()) return false;

    const QPointer<QPushButton> guardedButton = routeButton;
    QObject::disconnect(
        guardedButton.data(), &QPushButton::clicked, this, nullptr);
    guardedButton->setText(literalButtonText(title));
    if (guardedButton.isNull()) return false;
    guardedButton->setShortcut(QKeySequence());
    if (guardedButton.isNull()) return false;
    guardedButton->setAccessibleName(accessibleName);
    if (guardedButton.isNull()) return false;
    guardedButton->setEnabled(true);
    if (guardedButton.isNull()) return false;
    guardedButton->setFocusPolicy(Qt::StrongFocus);
    if (guardedButton.isNull()) return false;
    guardedButton->show();
    if (guardedButton.isNull()) return false;
    connect(guardedButton.data(), &QPushButton::clicked, this,
            [this, address = *validatedAddress] {
                // The owning window resolves current route/package authority after
                // this syntax-only presentation boundary emits the logical address.
                const std::optional<QString> revalidated = validatedAppAddress(address);
                if (revalidated.has_value()) {
                    emit addressActivated(*revalidated);
                }
            });
    guardedButton->setObjectName(objectName);
    if (guardedButton.isNull()) return false;
    return true;
}

QPushButton *NewTabPage::acquireRecentButton()
{
    while (!reusableRecentButtons_.isEmpty()) {
        const QPointer<QPushButton> candidate =
            reusableRecentButtons_.takeLast();
        if (!candidate.isNull()) return candidate.data();
    }
    auto *routeButton = new QPushButton(recentRoutesLayout_->parentWidget());
    connect(routeButton, &QObject::destroyed, this,
            [this](QObject *destroyedObject) {
                forgetDestroyedRecentButton(destroyedObject);
            });
    return routeButton;
}

void NewTabPage::forgetDestroyedRecentButton(QObject *destroyedObject)
{
    const auto isDestroyed = [destroyedObject](
                                 const QPointer<QPushButton> &candidate) {
        return candidate.isNull() || candidate.data() == destroyedObject;
    };
    recentButtons_.erase(
        std::remove_if(recentButtons_.begin(), recentButtons_.end(), isDestroyed),
        recentButtons_.end());
    reusableRecentButtons_.erase(
        std::remove_if(reusableRecentButtons_.begin(),
                       reusableRecentButtons_.end(),
                       isDestroyed),
        reusableRecentButtons_.end());
}

void NewTabPage::rebuildFocusOrder()
{
    QVector<QPushButton *> focusOrder = fixedButtons_;
    for (const QPointer<QPushButton> &candidate : recentButtons_) {
        if (!candidate.isNull()) focusOrder.append(candidate.data());
    }
    for (qsizetype index = 0; index + 1 < focusOrder.size(); ++index) {
        QWidget::setTabOrder(focusOrder.at(index), focusOrder.at(index + 1));
    }
}
