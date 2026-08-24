#include "NewTabPage.h"

#include "BrowserAddress.h"

#include <QChar>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

#include <array>
#include <optional>

namespace {

constexpr qsizetype MaximumRecentRoutes = 16;
constexpr qsizetype MaximumTitleCodeUnits = 256;

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

bool isBidiControl(const char16_t value) noexcept
{
    return value == 0x061c || value == 0x200e || value == 0x200f
        || (value >= 0x202a && value <= 0x202e)
        || (value >= 0x2066 && value <= 0x206f);
}

std::optional<QString> sanitizedTitle(const QString &title)
{
    QString result;
    result.reserve(MaximumTitleCodeUnits);
    bool prefixComplete = false;
    for (qsizetype index = 0; index < title.size(); ++index) {
        const QChar value = title.at(index);
        if (value.isHighSurrogate()) {
            if (index + 1 >= title.size()
                || !title.at(index + 1).isLowSurrogate()) {
                return std::nullopt;
            }
            if (!prefixComplete
                && result.size() + 2 <= MaximumTitleCodeUnits) {
                result.append(value);
                result.append(title.at(index + 1));
                prefixComplete = result.size() == MaximumTitleCodeUnits;
            } else if (!prefixComplete) {
                prefixComplete = true;
            }
            ++index;
            continue;
        }
        if (value.isLowSurrogate()) {
            return std::nullopt;
        }
        if (value.category() == QChar::Other_Control
            || isBidiControl(value.unicode())) {
            continue;
        }
        if (!prefixComplete) {
            result.append(value);
            prefixComplete = result.size() == MaximumTitleCodeUnits;
        }
    }
    return result;
}

std::optional<QString> validatedAppAddress(const QString &address)
{
    const BrowserAddress parsed = BrowserAddress::parse(address);
    if (!parsed.isValid() || parsed.kind() != BrowserAddressKind::App
        || parsed.canonical() != address) {
        return std::nullopt;
    }
    return parsed.canonical();
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

void NewTabPage::setRecentRoutes(const QVector<NewTabEntry> &validatedRoutes)
{
    for (QPushButton *routeButton : recentButtons_) {
        recentRoutesLayout_->removeWidget(routeButton);
        delete routeButton;
    }
    recentButtons_.clear();

    QSet<QString> acceptedAddresses;
    for (const NewTabEntry &entry : validatedRoutes) {
        const std::optional<QString> canonicalAddress = validatedAppAddress(entry.address);
        const std::optional<QString> title = sanitizedTitle(entry.title);
        if (!canonicalAddress.has_value() || !title.has_value()
            || acceptedAddresses.contains(*canonicalAddress)) {
            continue;
        }

        const QString displayTitle = title->isEmpty()
            ? QStringLiteral("Untitled route") : *title;
        const qsizetype index = recentButtons_.size();
        QPushButton *const routeButton = createRouteButton(
            displayTitle,
            *canonicalAddress,
            QStringLiteral("new-tab-recent-%1").arg(index),
            QStringLiteral("Open recent route: %1").arg(displayTitle),
            recentRoutesLayout_->parentWidget());
        if (routeButton == nullptr) {
            continue;
        }
        acceptedAddresses.insert(*canonicalAddress);
        recentRoutesLayout_->addWidget(routeButton);
        recentButtons_.append(routeButton);
        if (recentButtons_.size() == MaximumRecentRoutes) {
            break;
        }
    }

    rebuildFocusOrder();
}

QPushButton *NewTabPage::createRouteButton(const QString &title,
                                           const QString &canonicalAddress,
                                           const QString &objectName,
                                           const QString &accessibleName,
                                           QWidget *parent)
{
    const std::optional<QString> validatedAddress = validatedAppAddress(canonicalAddress);
    if (!validatedAddress.has_value()) {
        return nullptr;
    }

    auto *routeButton = new QPushButton(title, parent);
    routeButton->setObjectName(objectName);
    routeButton->setAccessibleName(accessibleName);
    routeButton->setFocusPolicy(Qt::StrongFocus);
    connect(routeButton, &QPushButton::clicked, this,
            [this, address = *validatedAddress] {
                // The owning window resolves current route/package authority after
                // this syntax-only presentation boundary emits the logical address.
                const std::optional<QString> revalidated = validatedAppAddress(address);
                if (revalidated.has_value()) {
                    emit addressActivated(*revalidated);
                }
            });
    return routeButton;
}

void NewTabPage::rebuildFocusOrder()
{
    QVector<QPushButton *> focusOrder = fixedButtons_;
    focusOrder.append(recentButtons_);
    for (qsizetype index = 0; index + 1 < focusOrder.size(); ++index) {
        QWidget::setTabOrder(focusOrder.at(index), focusOrder.at(index + 1));
    }
}
