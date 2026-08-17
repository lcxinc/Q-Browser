#pragma once

#include <QtTypes>

namespace ManifestResourceLimits {

inline constexpr qsizetype MaxManifestBytes = 1024 * 1024;
// Container nesting counts only JSON objects/arrays; the root container is level one.
inline constexpr qsizetype MaxJsonContainerNesting = 8;
// Decoded member names are counted as Unicode scalar values.
inline constexpr qsizetype MaxJsonMemberNameCharacters = 128;
// Raw string lexemes are counted as UTF-16 code units after strict UTF-8 decoding.
inline constexpr qsizetype MaxJsonStringRawCharacters = 4096;
// Decoded string values are counted as Unicode scalar values.
inline constexpr qsizetype MaxJsonStringCharacters = 4096;
inline constexpr qsizetype MaxVersionCharacters = 128;
inline constexpr qsizetype MaxEntryPointCharacters = 120;
inline constexpr qsizetype MaxWindowsComponentUtf16Units = 240;
inline constexpr qsizetype MaxImports = 32;
inline constexpr qsizetype MaxNetworkHosts = 64;
inline constexpr qsizetype MaxNetworkMethods = 3;
inline constexpr qsizetype MaxRoutes = 256;
inline constexpr qsizetype MaxRouteCharacters = 2048;

static_assert(MaxEntryPointCharacters * 2 == MaxWindowsComponentUtf16Units);

} // namespace ManifestResourceLimits
