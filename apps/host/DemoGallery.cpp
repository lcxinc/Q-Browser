#include "DemoGallery.h"
#include "DemoExamples.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <array>

namespace {
struct DemoInfo {
    const char *id;
    QString title;
    QString description;
    QString category;
    QString tags;
    QColor accent;
};
const std::array<DemoInfo, 6> &demos()
{
    static const std::array<DemoInfo, 6> entries{{
        {"widgets", QStringLiteral("控件实验室"), QStringLiteral("组合表单、滑块与进度反馈，实时预览你的设置。"), QStringLiteral("基础控件"), QStringLiteral("Widgets · 表单 · 状态"), QColor("#38b99a")},
        {"table", QStringLiteral("数据探索"), QStringLiteral("搜索、筛选和排序订单，体验桌面级数据表格。"), QStringLiteral("数据展示"), QStringLiteral("Model / View · 筛选"), QColor("#6485ef")},
        {"charts", QStringLiteral("图表工作台"), QStringLiteral("切换折线与柱状图，观察不同数据集的变化。"), QStringLiteral("数据展示"), QStringLiteral("QPainter · 可视化"), QColor("#b18aeb")},
        {"animation", QStringLiteral("动效实验室"), QStringLiteral("比较三种缓动曲线，调整速度、暂停并重新播放。"), QStringLiteral("交互体验"), QStringLiteral("Animation · 缓动"), QColor("#ebaa51")},
        {"kanban", QStringLiteral("轻量任务看板"), QStringLiteral("添加任务，在三列之间拖放卡片，组织一天的工作。"), QStringLiteral("交互体验"), QStringLiteral("Drag & Drop · 看板"), QColor("#e58ca5")},
        {"canvas", QStringLiteral("创意画布"), QStringLiteral("选择颜色和笔宽，自由绘制属于你的第一张草图。"), QStringLiteral("交互体验"), QStringLiteral("Canvas · 鼠标交互"), QColor("#54b9d0")},
    }};
    return entries;
}

class DemoPreview final : public QWidget {
public:
    DemoPreview(int index, const QColor &accent, QWidget *parent)
        : QWidget(parent), index_(index), accent_(accent)
    {
        setFixedHeight(126);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QColor tint = accent_;
        tint.setAlpha(24);
        p.setBrush(tint);
        p.drawRoundedRect(rect(), 10, 10);
        p.translate(width() / 2.0 - 100, 11);
        p.setBrush(palette().base());
        p.drawRoundedRect(QRectF(12, 0, 176, 104), 8, 8);
        p.setPen(QPen(accent_, 2));
        if (index_ == 0) {
            p.drawRoundedRect(QRectF(28, 16, 94, 16), 4, 4);
            p.drawLine(28, 51, 160, 51);
            p.setBrush(accent_);
            p.drawEllipse(QPointF(100, 51), 5, 5);
            p.drawRoundedRect(QRectF(28, 73, 66, 15), 4, 4);
        } else if (index_ == 1) {
            p.fillRect(QRect(26, 14, 148, 15), accent_);
            for (int row = 0; row < 4; ++row) {
                p.setOpacity(0.2 + row * 0.12);
                p.drawLine(28, 41 + row * 16, 170, 41 + row * 16);
            }
            p.setOpacity(0.4);
            p.drawLine(65, 34, 65, 94);
            p.drawLine(125, 34, 125, 94);
        } else if (index_ == 2) {
            const std::array<int, 7> heights{28, 42, 33, 56, 44, 64, 73};
            p.setBrush(accent_);
            p.setPen(Qt::NoPen);
            for (int i = 0; i < 7; ++i) {
                p.setOpacity(0.35 + i * 0.09);
                p.drawRoundedRect(QRectF(29 + i * 21, 89 - heights.at(i), 12, heights.at(i)), 3, 3);
            }
        } else if (index_ == 3) {
            for (int i = 0; i < 3; ++i) {
                p.setOpacity(0.2);
                p.drawLine(31, 26 + i * 26, 168, 26 + i * 26);
                p.setOpacity(1.0);
                p.setBrush(accent_);
                p.drawEllipse(QPointF(53 + i * 44, 26 + i * 26), 8, 8);
            }
        } else if (index_ == 4) {
            for (int col = 0; col < 3; ++col) {
                p.setOpacity(0.3);
                p.setBrush(accent_);
                p.drawRoundedRect(QRectF(25 + col * 52, 14, 44, 76), 4, 4);
                p.setOpacity(1);
                for (int row = 0; row < 3 - col; ++row) {
                    p.drawRoundedRect(QRectF(31 + col * 52, 25 + row * 19, 32, 12), 3, 3);
                }
            }
        } else {
            QPainterPath stroke;
            stroke.moveTo(29, 72);
            stroke.cubicTo(61, -1, 83, 118, 115, 40);
            stroke.cubicTo(127, 12, 141, 48, 171, 30);
            p.setPen(QPen(accent_, 5, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawPath(stroke);
            p.setPen(QPen(QColor("#ebaa51"), 4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(132, 76, 168, 76);
        }
    }
private:
    int index_;
    QColor accent_;
};

QLabel *label(const QString &text, const QString &name, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(name);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
}

DemoGallery::DemoGallery(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("demo-gallery"));
    setAccessibleName(QStringLiteral("Q-Browser 示例中心"));
    setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    pages_ = new QStackedWidget(this);
    root->addWidget(pages_);
    catalog_ = new QScrollArea(pages_);
    catalog_->setWidgetResizable(true);
    catalog_->setFrameShape(QFrame::NoFrame);
    auto *catalogContent = new QWidget;
    auto *catalogLayout = new QVBoxLayout(catalogContent);
    catalogLayout->setContentsMargins(30, 26, 30, 30);
    catalogLayout->setSpacing(16);
    catalogLayout->addWidget(label(QStringLiteral("EXPLORE  /  Q-BROWSER"), QStringLiteral("demo-eyebrow"), catalogContent));
    catalogLayout->addWidget(label(QStringLiteral("从一个示例开始。"), QStringLiteral("demo-heading"), catalogContent));
    catalogLayout->addWidget(label(QStringLiteral("探索桌面应用的不同可能。挑选一个示例，直接上手体验。"), QStringLiteral("demo-subtitle"), catalogContent));
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(12);
    search_ = new QLineEdit(catalogContent);
    search_->setObjectName(QStringLiteral("demo-search"));
    search_->setPlaceholderText(QStringLiteral("搜索示例，例如：图表、动画、Widgets…"));
    search_->setAccessibleName(QStringLiteral("搜索示例"));
    search_->setClearButtonEnabled(true);
    category_ = new QComboBox(catalogContent);
    category_->setObjectName(QStringLiteral("demo-category"));
    category_->setAccessibleName(QStringLiteral("示例分类"));
    category_->addItems({QStringLiteral("全部示例"), QStringLiteral("基础控件"), QStringLiteral("数据展示"), QStringLiteral("交互体验")});
    toolbar->addWidget(search_, 1);
    toolbar->addWidget(category_);
    catalogLayout->addLayout(toolbar);
    count_ = label({}, QStringLiteral("demo-count"), catalogContent);
    catalogLayout->addWidget(count_);
    grid_ = new QGridLayout;
    grid_->setSpacing(18);
    catalogLayout->addLayout(grid_);
    empty_ = label(QStringLiteral("没有找到匹配的示例。试试其他关键词或分类。"), QStringLiteral("demo-empty"), catalogContent);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setMinimumHeight(100);
    catalogLayout->addWidget(empty_);
    catalogLayout->addStretch();
    for (int i = 0; i < static_cast<int>(demos().size()); ++i) {
        const auto &info = demos().at(i);
        auto *card = new QFrame(catalogContent);
        card->setObjectName(QStringLiteral("demo-card-") + QString::fromLatin1(info.id));
        card->setProperty("demoCard", true);
        card->setMinimumWidth(210);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);
        layout->addWidget(new DemoPreview(i, info.accent, card));
        layout->addWidget(label(info.title, QStringLiteral("demo-card-title"), card));
        auto *description = label(info.description, QStringLiteral("demo-card-description"), card);
        description->setMinimumHeight(42);
        layout->addWidget(description);
        layout->addWidget(label(info.tags, QStringLiteral("demo-card-tags"), card));
        auto *open = new QPushButton(QStringLiteral("打开示例  →"), card);
        open->setObjectName(QStringLiteral("demo-open-") + QString::fromLatin1(info.id));
        open->setAccessibleName(QStringLiteral("打开示例：") + info.title);
        open->setCursor(Qt::PointingHandCursor);
        connect(open, &QPushButton::clicked, this, [this, id = QString::fromLatin1(info.id)] { (void)openDemo(id); });
        layout->addWidget(open);
        cards_.append(card);
    }
    catalog_->setWidget(catalogContent);
    pages_->addWidget(catalog_);

    auto *detailScroll = new QScrollArea(pages_);
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detail_ = new QWidget;
    detailLayout_ = new QVBoxLayout(detail_);
    detailLayout_->setContentsMargins(30, 24, 30, 30);
    detailLayout_->setSpacing(16);
    auto *back = new QPushButton(QStringLiteral("←  返回示例中心"), detail_);
    back->setObjectName(QStringLiteral("demo-back"));
    detailLayout_->addWidget(back, 0, Qt::AlignLeft);
    detailTitle_ = label({}, QStringLiteral("demo-heading"), detail_);
    detailDescription_ = label({}, QStringLiteral("demo-subtitle"), detail_);
    detailLayout_->addWidget(detailTitle_);
    detailLayout_->addWidget(detailDescription_);
    detailScroll->setWidget(detail_);
    pages_->addWidget(detailScroll);
    connect(back, &QPushButton::clicked, this, &DemoGallery::showGallery);
    connect(search_, &QLineEdit::textChanged, this, &DemoGallery::filterCards);
    connect(category_, &QComboBox::currentIndexChanged, this, &DemoGallery::filterCards);
    setStyleSheet(QStringLiteral(R"(
        QWidget#demo-gallery { font-size: 13px; }
        QWidget#demo-gallery QLabel#demo-eyebrow { color: #22987a; font-size: 11px; font-weight: 700; }
        QWidget#demo-gallery QLabel#demo-heading { font-size: 28px; font-weight: 700; }
        QWidget#demo-gallery QLabel#demo-subtitle { color: palette(placeholder-text); font-size: 14px; }
        QWidget#demo-gallery QFrame[demoCard="true"] { background: palette(base); border: 1px solid palette(midlight); border-radius: 12px; }
        QWidget#demo-gallery QLabel#demo-card-title { font-size: 17px; font-weight: 600; }
        QWidget#demo-gallery QLabel#demo-card-description { color: palette(text); }
        QWidget#demo-gallery QLabel#demo-card-tags, QWidget#demo-gallery QLabel#demo-count { color: palette(placeholder-text); font-size: 12px; }
        QWidget#demo-gallery QPushButton { padding: 9px 16px; border: 1px solid palette(midlight); border-radius: 6px; background: palette(base); }
        QWidget#demo-gallery QPushButton:hover { background: palette(midlight); }
        QWidget#demo-gallery QPushButton:focus { border-color: #22987a; }
        QWidget#demo-gallery QLineEdit, QWidget#demo-gallery QComboBox { padding: 9px 12px; border: 1px solid palette(midlight); border-radius: 6px; background: palette(base); }
        QWidget#demo-gallery QLineEdit:focus { border-color: #22987a; }
    )"));
    filterCards();
}

void DemoGallery::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    arrangeCards();
}

void DemoGallery::filterCards()
{
    const QString query = search_->text().trimmed();
    int visible = 0;
    for (int i = 0; i < cards_.size(); ++i) {
        const auto &info = demos().at(i);
        const bool match = (category_->currentIndex() == 0 || category_->currentText() == info.category)
            && (query.isEmpty() || (info.title + info.description + info.tags + QString::fromLatin1(info.id)).contains(query, Qt::CaseInsensitive));
        cards_.at(i)->setProperty("matchesFilter", match);
        cards_.at(i)->setVisible(match);
        if (match) ++visible;
    }
    count_->setText(QStringLiteral("%1 个示例  ·  本地运行，即点即用").arg(visible));
    empty_->setVisible(visible == 0);
    columns_ = 0;
    arrangeCards();
}

void DemoGallery::arrangeCards()
{
    const int columns = width() >= 1100 ? 3 : width() >= 650 ? 2 : 1;
    if (columns_ == columns) return;
    columns_ = columns;
    while (QLayoutItem *item = grid_->takeAt(0)) delete item;
    for (int i = 0; i < 3; ++i) grid_->setColumnStretch(i, i < columns ? 1 : 0);
    int position = 0;
    for (QWidget *card : cards_) {
        if (!card->property("matchesFilter").toBool()) continue;
        grid_->addWidget(card, position / columns, position % columns);
        ++position;
    }
}

bool DemoGallery::openDemo(const QString &id)
{
    for (const auto &info : demos()) {
        if (id != QString::fromLatin1(info.id)) continue;
        QWidget *const candidate = createDemoExample(id, detail_);
        if (candidate == nullptr) return false;
        delete example_;
        example_ = candidate;
        example_->setObjectName(QStringLiteral("demo-example-") + id);
        detailTitle_->setText(info.title);
        detailDescription_->setText(info.description);
        detailLayout_->addWidget(example_, 1);
        pages_->setCurrentIndex(1);
        return true;
    }
    return false;
}

void DemoGallery::showGallery()
{
    pages_->setCurrentIndex(0);
    delete example_;
    example_ = nullptr;
}
