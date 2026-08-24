#pragma once

#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

struct NewTabEntry
{
    QString title;
    QString address;
};

class NewTabPage final : public QWidget
{
    Q_OBJECT

public:
    explicit NewTabPage(QWidget *parent = nullptr);
    ~NewTabPage() override;

    void setRecentRoutes(const QVector<NewTabEntry> &validatedRoutes);

signals:
    void addressActivated(const QString &canonicalAddress);

private:
    QPushButton *createRouteButton(const QString &title,
                                   const QString &canonicalAddress,
                                   const QString &objectName,
                                   const QString &accessibleName,
                                   QWidget *parent);
    [[nodiscard]] bool configureRouteButton(
        QPushButton *routeButton,
        const QString &title,
        const QString &canonicalAddress,
        const QString &objectName,
        const QString &accessibleName);
    [[nodiscard]] QPushButton *acquireRecentButton();
    void forgetDestroyedRecentButton(QObject *destroyedObject);
    void applyRecentRoutes(const QVector<NewTabEntry> &normalizedRoutes);
    void schedulePendingRecentRoutes();
    void rebuildFocusOrder();

    QVBoxLayout *recentRoutesLayout_ = nullptr;
    QVector<QPushButton *> fixedButtons_;
    QVector<QPointer<QPushButton>> recentButtons_;
    QVector<QPointer<QPushButton>> reusableRecentButtons_;
    QVector<NewTabEntry> pendingRecentRoutes_;
    bool recentRoutesRebuildInProgress_ = false;
    bool recentRoutesUpdatePending_ = false;
    bool recentRoutesDispatchScheduled_ = false;
};
