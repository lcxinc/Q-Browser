#pragma once

#include <QUrl>
#include <QWidget>

#include <memory>

class PilotRequestInterceptor;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

class WebSurface final : public QWidget
{
    Q_OBJECT

public:
    explicit WebSurface(const QUrl &mockOrigin, QWidget *parent = nullptr);
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
    [[nodiscard]] QUrl currentUrl() const;
    [[nodiscard]] QWebEngineProfile *profile() const noexcept;
    [[nodiscard]] QWebEnginePage *page() const noexcept;
    [[nodiscard]] QWebEngineView *view() const noexcept;
    [[nodiscard]] PilotRequestInterceptor *requestInterceptor() const noexcept;

signals:
    void navigationFinished(const QUrl &url, bool success);
    void popupDenied();
    void downloadDenied(const QUrl &url);
    void permissionDenied(const QUrl &origin);

private:
    void loadTrustedError();

    std::unique_ptr<QWebEngineProfile> profile_;
    std::unique_ptr<PilotRequestInterceptor> interceptor_;
    std::unique_ptr<QWebEnginePage> page_;
    std::unique_ptr<QWebEngineView> view_;
    bool configurationValid_ = false;
    bool loadingTrustedError_ = false;
};
