#pragma once

#include <QUrl>
#include <QWebEngineUrlRequestInterceptor>

class QWebEngineUrlRequestInfo;

class PilotRequestInterceptor final : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    explicit PilotRequestInterceptor(QUrl mockOrigin, QObject *parent = nullptr);

    [[nodiscard]] bool isConfigurationValid() const noexcept;
    [[nodiscard]] bool isAllowed(const QUrl &url) const;
    [[nodiscard]] QUrl mockOrigin() const;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

signals:
    void requestBlocked(const QUrl &url);

private:
    QUrl mockOrigin_;
    bool configurationValid_ = false;
};
