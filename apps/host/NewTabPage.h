#pragma once

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

    void setRecentRoutes(const QVector<NewTabEntry> &validatedRoutes);

signals:
    void addressActivated(const QString &canonicalAddress);

private:
    QPushButton *createRouteButton(const QString &title,
                                   const QString &canonicalAddress,
                                   const QString &objectName,
                                   const QString &accessibleName,
                                   QWidget *parent);
    void rebuildFocusOrder();

    QVBoxLayout *recentRoutesLayout_ = nullptr;
    QVector<QPushButton *> fixedButtons_;
    QVector<QPushButton *> recentButtons_;
};
