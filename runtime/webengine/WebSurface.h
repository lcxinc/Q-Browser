#pragma once

#include <QMetaObject>
#include <QString>
#include <QUrl>
#include <QWebEnginePage>
#include <QWidget>

#include <memory>

class PilotRequestInterceptor;
class QWebEngineLoadingInfo;
class QWebEngineProfile;
class QWebEngineView;
class WebSessionProfile;

class WebSurface final : public QWidget
{
    Q_OBJECT

public:
    explicit WebSurface(WebSessionProfile &session,
                        QUrl registeredMainFrameEntry,
                        QWidget *parent = nullptr);
    ~WebSurface() override;

    WebSurface(const WebSurface &) = delete;
    WebSurface &operator=(const WebSurface &) = delete;

    [[nodiscard]] static QUrl trustedErrorUrl();
    [[nodiscard]] static bool isChromiumSandboxConfigurationSafe(
        const QStringList &arguments,
        const QByteArray &disableSandboxEnvironment,
        const QByteArray &chromiumFlagsEnvironment);

    [[nodiscard]] bool isConfigurationValid() const noexcept;
    [[nodiscard]] bool navigate(const QUrl &url);
    [[nodiscard]] bool reload();
    void stop();
    void setTabActive(bool active);
    [[nodiscard]] bool shutdown();
    [[nodiscard]] QString title() const;
    [[nodiscard]] int loadProgress() const noexcept;
    [[nodiscard]] bool isLoading() const noexcept;

#ifdef Q_BROWSER_WEBENGINE_TESTING
    [[nodiscard]] QUrl currentUrl() const;
    [[nodiscard]] QUrl registeredMainFrameEntry() const;
    [[nodiscard]] QWebEngineProfile *profile() const noexcept;
    [[nodiscard]] QWebEnginePage *page() const noexcept;
    [[nodiscard]] QWebEngineView *view() const noexcept;
    [[nodiscard]] PilotRequestInterceptor *requestInterceptor() const noexcept;
    [[nodiscard]] quint64 navigationIncarnationForTesting() const noexcept;
    [[nodiscard]] QString trustedTitleForTesting(const QString &physicalTitle) const;
#endif

signals:
    void navigationFinished(const QUrl &url, bool success);
    void titleChanged(const QString &title);
    void loadingChanged(bool loading);
    void loadProgressChanged(int progress);
    void rendererFailed(QWebEnginePage::RenderProcessTerminationStatus status,
                        int exitCode);
    void popupDenied();
    void downloadDenied(const QUrl &url);
    void permissionDenied(const QUrl &origin);
    void fileSelectionDenied(bool directorySelection);

private:
    void beginNavigation(const QUrl &url);
    void observeLoadingChange(const QWebEngineLoadingInfo &information);
    void handleLoadingChange(const QWebEngineLoadingInfo &information,
                             quint64 incarnation);
    void setLoading(bool loading);
    void setLoadProgress(int progress);
    void updateTitle(const QString &physicalTitle);
    [[nodiscard]] QString trustedTitle(const QString &physicalTitle) const;
    void handleDeniedMainFrameNavigation(const QUrl &url);
    void loadTrustedError();
    void freezeWhenRecommended();

    WebSessionProfile *session_ = nullptr;
    QUrl registeredMainFrameEntry_;
    std::unique_ptr<QWebEnginePage> page_;
    std::unique_ptr<QWebEngineView> view_;
    QMetaObject::Connection recommendedStateConnection_;
    QMetaObject::Connection sessionRetirementConnection_;
    QString physicalOriginHost_;
    QString title_ = QStringLiteral("Restricted web");
    int loadProgress_ = 0;
    quint64 navigationIncarnation_ = 0;
    quint64 activeLoadIncarnation_ = 0;
    quint64 stopRequestedIncarnation_ = 0;
    quint64 terminalPendingIncarnation_ = 0;
    QUrl expectedNavigationUrl_;
    bool configurationValid_ = false;
    bool awaitingLoadStart_ = false;
    bool awaitingDifferentDocument_ = false;
    bool activeLoadStarted_ = false;
    bool titleUpdatesAllowed_ = false;
    bool loading_ = false;
    bool tabActive_ = false;
    bool shutdown_ = false;
    bool shutdownSucceeded_ = true;
};
