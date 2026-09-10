#pragma once

#include <QWidget>
#include <QVector>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

class DemoGallery final : public QWidget
{
    Q_OBJECT
public:
    explicit DemoGallery(QWidget *parent = nullptr);
    bool openDemo(const QString &id);
    void showGallery();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void filterCards();
    void arrangeCards();
    QStackedWidget *pages_ = nullptr;
    QScrollArea *catalog_ = nullptr;
    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QLabel *count_ = nullptr;
    QLabel *empty_ = nullptr;
    QGridLayout *grid_ = nullptr;
    QVector<QWidget *> cards_;
    QWidget *detail_ = nullptr;
    QLabel *detailTitle_ = nullptr;
    QLabel *detailDescription_ = nullptr;
    QVBoxLayout *detailLayout_ = nullptr;
    QWidget *example_ = nullptr;
    int columns_ = 0;
};
