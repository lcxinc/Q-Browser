#pragma once

#include <QList>
#include <QRect>

[[nodiscard]] QRect restoreBrowserWindowGeometry(
    const QRect &persistedGeometry,
    const QRect &primaryAvailableGeometry,
    const QList<QRect> &availableScreenGeometries);
