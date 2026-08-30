#include "BrowserWindowGeometry.h"

#include <QSize>

#include <algorithm>

namespace
{
constexpr int DefaultWindowWidth = 1280;
constexpr int DefaultWindowHeight = 800;
constexpr int MinimumVisibleWidth = 64;
constexpr int MinimumVisibleHeight = 64;

QRect centeredDefault(const QRect &availableGeometry)
{
    const QSize fallbackSize(
        std::min(DefaultWindowWidth, availableGeometry.width()),
        std::min(DefaultWindowHeight, availableGeometry.height()));
    return QRect(
        availableGeometry.x()
            + (availableGeometry.width() - fallbackSize.width()) / 2,
        availableGeometry.y()
            + (availableGeometry.height() - fallbackSize.height()) / 2,
        fallbackSize.width(),
        fallbackSize.height());
}

QRect deterministicDefault()
{
    return QRect(0, 0, DefaultWindowWidth, DefaultWindowHeight);
}
}

QRect restoreBrowserWindowGeometry(
    const QRect &persistedGeometry,
    const QRect &primaryAvailableGeometry,
    const QList<QRect> &availableScreenGeometries)
{
    if (persistedGeometry.isValid()) {
        const bool sufficientlyVisible = std::ranges::any_of(
            availableScreenGeometries,
            [&persistedGeometry](const QRect &availableGeometry) {
                if (!availableGeometry.isValid()) return false;
                const QRect visible = persistedGeometry.intersected(
                    availableGeometry);
                return visible.width() >= MinimumVisibleWidth
                    && visible.height() >= MinimumVisibleHeight;
            });
        if (sufficientlyVisible) return persistedGeometry;
    }

    if (primaryAvailableGeometry.isValid()) {
        return centeredDefault(primaryAvailableGeometry);
    }
    const auto firstValidScreen = std::ranges::find_if(
        availableScreenGeometries,
        [](const QRect &availableGeometry) {
            return availableGeometry.isValid();
        });
    return firstValidScreen != availableScreenGeometries.cend()
        ? centeredDefault(*firstValidScreen)
        : deterministicDefault();
}
