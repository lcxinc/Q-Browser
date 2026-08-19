#include <QAccessible>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest/quicktest.h>

class AccessibilityAnnouncementRecorder final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY changed)
    Q_PROPERTY(int lastPoliteness READ lastPoliteness NOTIFY changed)

public:
    explicit AccessibilityAnnouncementRecorder(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~AccessibilityAnnouncementRecorder() override
    {
        uninstall();
    }

    void install()
    {
        instance = this;
        previousHandler = QAccessible::installUpdateHandler(handleAccessibilityUpdate);
    }

    void uninstall()
    {
        if (instance != this)
            return;
        QAccessible::installUpdateHandler(previousHandler);
        instance = nullptr;
    }

    [[nodiscard]] int count() const noexcept { return announcementCount; }
    [[nodiscard]] QString lastMessage() const { return announcementMessage; }
    [[nodiscard]] int lastPoliteness() const noexcept { return announcementPoliteness; }

    Q_INVOKABLE void clear()
    {
        announcementCount = 0;
        announcementMessage.clear();
        announcementPoliteness = -1;
        emit changed();
    }

signals:
    void changed();

private:
    static void handleAccessibilityUpdate(QAccessibleEvent *event)
    {
        if (instance == nullptr || event == nullptr || event->type() != QAccessible::Announcement)
            return;

        const auto *announcement = static_cast<QAccessibleAnnouncementEvent *>(event);
        ++instance->announcementCount;
        instance->announcementMessage = announcement->message();
        instance->announcementPoliteness = static_cast<int>(announcement->politeness());
        emit instance->changed();
    }

    inline static AccessibilityAnnouncementRecorder *instance = nullptr;
    QAccessible::UpdateHandler previousHandler = nullptr;
    int announcementCount = 0;
    QString announcementMessage;
    int announcementPoliteness = -1;
};

class QuickTestSetup final : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        announcementRecorder.install();
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty(QStringLiteral("AccessibilityRecorder"),
                                                  &announcementRecorder);
    }

private:
    AccessibilityAnnouncementRecorder announcementRecorder;
};

QUICK_TEST_MAIN_WITH_SETUP(qbrowser_quick_tests, QuickTestSetup)

#include "main.moc"
