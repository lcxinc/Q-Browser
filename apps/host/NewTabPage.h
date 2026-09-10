#pragma once

#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#ifdef Q_BROWSER_HOST_TESTING
#include <functional>
#endif

class QPushButton;
class QVBoxLayout;
class QTabWidget;
class DemoGallery;

#ifdef Q_BROWSER_HOST_TESTING
namespace qbrowser_host_testing
{
struct NewTabPageTestHooks final
{
    std::function<void()> beforePendingRecentRoutesDispatch;
};

void setNewTabPageTestHooks(NewTabPageTestHooks hooks);
void resetNewTabPageTestHooks();
[[nodiscard]] NewTabPageTestHooks newTabPageTestHooks();
}
#endif

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
    void showExamples();
    void showPilotRoutes();

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
    QTabWidget *sections_ = nullptr;
    DemoGallery *demoGallery_ = nullptr;
    QVector<QPushButton *> fixedButtons_;
    QVector<QPointer<QPushButton>> recentButtons_;
    QVector<QPointer<QPushButton>> reusableRecentButtons_;
    QVector<NewTabEntry> pendingRecentRoutes_;
    bool recentRoutesRebuildInProgress_ = false;
    bool recentRoutesUpdatePending_ = false;
    bool recentRoutesDispatchScheduled_ = false;
};
