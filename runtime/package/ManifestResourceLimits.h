#pragma once

#include <QtTypes>

namespace ManifestResourceLimits {

inline constexpr qsizetype MaxManifestBytes = 1024 * 1024;
inline constexpr qsizetype MaxVersionCharacters = 128;
inline constexpr qsizetype MaxEntryPointCharacters = 240;
inline constexpr qsizetype MaxImports = 32;
inline constexpr qsizetype MaxNetworkHosts = 64;
inline constexpr qsizetype MaxNetworkMethods = 3;
inline constexpr qsizetype MaxRoutes = 256;
inline constexpr qsizetype MaxRouteCharacters = 2048;

} // namespace ManifestResourceLimits
