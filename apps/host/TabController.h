#pragma once

#include "BrowserTabModel.h"

#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <memory>

class QLabel;
class NewTabPage;
class QStackedWidget;
class QWidget;
class WebSessionProfile;
class WebSurface;
class WorkerSurface;

enum class HostSurfaceKind
{
    Host,
    Worker,
    Web,
    TrustedError,
};

Q_DECLARE_METATYPE(HostSurfaceKind)

class TabController final : public QObject
{
    Q_OBJECT

public:
    TabController(QString tabId,
                  BrowserTabModel *model,
                  QStackedWidget *surfaceStack,
                  WebSessionProfile *webSessionProfile,
                  QObject *parent = nullptr);
    ~TabController() override;

    TabController(const TabController &) = delete;
    TabController &operator=(const TabController &) = delete;

    [[nodiscard]] QString tabId() const;
    [[nodiscard]] quint64 incarnation() const noexcept;
    [[nodiscard]] BrowserTabLifecycle lifecycle() const noexcept;
    [[nodiscard]] HostSurfaceKind surfaceKind() const noexcept;
    [[nodiscard]] QWidget *currentSurface() const noexcept;
    [[nodiscard]] NewTabPage *hostSurface() const noexcept;
    [[nodiscard]] WebSurface *webSurface() const noexcept;
    [[nodiscard]] WorkerSurface *workerSurface() const noexcept;
    [[nodiscard]] QString workerPackageId() const;
    [[nodiscard]] QString trustedErrorText() const;

    void setActive(bool active);
    [[nodiscard]] quint64 beginNavigation();
    [[nodiscard]] bool startHost(quint64 navigationIncarnation);
    [[nodiscard]] bool startWeb(const QUrl &physicalEntry,
                                quint64 navigationIncarnation,
                                bool reloadExisting = false);
    [[nodiscard]] bool startLegacyApp(const QString &packageId,
                                      const QString &entryPoint,
                                      const QVariantMap &parameters,
                                      const QUrl &logicalUrl,
                                      quint64 navigationIncarnation);
    void stop();
    void showTrustedError(const QString &plainText,
                          bool retireWebSurface = false);
    void showTrustedErrorForNavigation(
        quint64 navigationIncarnation,
        const QString &plainText,
        bool retireWebSurface = false);

    [[nodiscard]] bool attachLegacyWorkerSurface(
        std::unique_ptr<WorkerSurface> surface);
    void detachLegacyWorkerSurface();
    [[nodiscard]] bool beginClosing();
    [[nodiscard]] bool retire();

signals:
    void addressActivated(const QString &tabId,
                          quint64 incarnation,
                          const QString &canonicalAddress);
    void workerRouteRequested(const QString &packageId,
                              const QString &entryPoint,
                              const QVariantMap &parameters,
                              const QUrl &appUrl);

private:
    friend class MainWindow;

    Q_SIGNAL void resourceMutationStarted();
    Q_SIGNAL void resourceMutationFinished();

    [[nodiscard]] bool canTransitionResources() const noexcept;
    [[nodiscard]] bool isCurrentNavigation(
        quint64 navigationIncarnation) const noexcept;
    void advanceIncarnation();
    void transitionTo(BrowserTabLifecycle lifecycle);
    void updateVisibility();
    void connectHostSignals();
    void connectWebSignals();
    void withResourceMutation(const std::function<void()> &mutation);
    void enterTrustedError(const QString &plainText,
                           bool retireWebSurface,
                           bool advance);
    void destroyHostSurface();
    void destroyTrustedErrorSurface();
    void destroyWebSurface();

    QString tabId_;
    BrowserTabModel *model_ = nullptr;
    QStackedWidget *surfaceStack_ = nullptr;
    WebSessionProfile *webSessionProfile_ = nullptr;
    NewTabPage *hostSurface_ = nullptr;
    WebSurface *webSurface_ = nullptr;
    WorkerSurface *workerSurface_ = nullptr;
    QWidget *trustedErrorSurface_ = nullptr;
    QLabel *trustedErrorLabel_ = nullptr;
    QWidget *currentSurface_ = nullptr;
    QUrl webEntry_;
    QString workerPackageId_;
    quint64 incarnation_ = 0;
    BrowserTabLifecycle lifecycle_ = BrowserTabLifecycle::Dormant;
    HostSurfaceKind surfaceKind_ = HostSurfaceKind::TrustedError;
    bool active_ = false;
    bool retired_ = false;
    bool resourceMutationInProgress_ = false;
};
