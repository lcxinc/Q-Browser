#pragma once

#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QUrl>

#include <memory>

class PilotRequestInterceptor;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineUrlRequestInterceptor;
class WebSurface;

class WebSessionProfile final : public QObject
{
    Q_OBJECT

public:
    static constexpr qsizetype maximumPageCount = 16;

    explicit WebSessionProfile(QUrl mockOrigin, QObject *parent = nullptr);
    ~WebSessionProfile() override;

    WebSessionProfile(const WebSessionProfile &) = delete;
    WebSessionProfile &operator=(const WebSessionProfile &) = delete;

    [[nodiscard]] bool isConfigurationValid() const noexcept;
    [[nodiscard]] qsizetype registeredPageCount() const noexcept;
    [[nodiscard]] bool shutdown();

#ifdef Q_BROWSER_WEBENGINE_TESTING
    [[nodiscard]] QWebEngineProfile *profile() const noexcept;
    [[nodiscard]] PilotRequestInterceptor *requestInterceptor() const noexcept;
    [[nodiscard]] bool registerPage(QWebEnginePage *page);
    [[nodiscard]] bool unregisterPage(QWebEnginePage *page);
#endif

signals:
    void registeredPageCountChanged(qsizetype count);
    void downloadDenied(QWebEnginePage *page, const QUrl &url);

private:
    friend class WebSurface;

    Q_SIGNAL void retirementRequested();

    [[nodiscard]] QWebEngineProfile *profileHandle() const noexcept;
    [[nodiscard]] PilotRequestInterceptor *interceptorHandle() const noexcept;
    [[nodiscard]] bool registerPageInternal(QWebEnginePage *page);
    [[nodiscard]] bool retirePageInternal(
        std::unique_ptr<QWebEnginePage> &page);
    void detachProfile();

    std::unique_ptr<PilotRequestInterceptor> interceptor_;
    std::unique_ptr<QWebEngineUrlRequestInterceptor> requestFilter_;
    std::unique_ptr<QWebEngineProfile> profile_;
    QHash<QWebEnginePage *, QMetaObject::Connection> registeredPages_;
    QWebEnginePage *retiringPage_ = nullptr;
    bool configurationValid_ = false;
    bool acceptingPages_ = true;
    bool shutdown_ = false;
};
