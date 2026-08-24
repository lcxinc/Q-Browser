#pragma once

#include <QObject>
#include <QSet>
#include <QUrl>

#include <memory>

class PilotRequestInterceptor;
class QWebEnginePage;
class QWebEngineProfile;

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
    [[nodiscard]] QWebEngineProfile *profile() const noexcept;
    [[nodiscard]] PilotRequestInterceptor *requestInterceptor() const noexcept;
    [[nodiscard]] qsizetype registeredPageCount() const noexcept;
    [[nodiscard]] bool registerPage(QWebEnginePage *page);
    [[nodiscard]] bool unregisterPage(QWebEnginePage *page);
    [[nodiscard]] bool shutdown();

signals:
    void registeredPageCountChanged(qsizetype count);
    void downloadDenied(QWebEnginePage *page, const QUrl &url);

private:
    void detachProfile();

    std::unique_ptr<PilotRequestInterceptor> interceptor_;
    std::unique_ptr<QWebEngineProfile> profile_;
    QSet<QWebEnginePage *> registeredPages_;
    bool configurationValid_ = false;
    bool acceptingPages_ = true;
    bool shutdown_ = false;
};
