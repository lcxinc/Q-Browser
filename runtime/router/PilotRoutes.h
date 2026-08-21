#pragma once

#include "RouteRegistry.h"

#include <QUrl>

#include <optional>

[[nodiscard]] std::optional<RouteRegistry> createPilotRouteRegistry(
    const QUrl &mockOrigin,
    const QString &workerAppId = QStringLiteral("com.qbrowser.pilot"));
