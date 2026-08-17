#include "Manifest.h"

#include "JsonPreflight.h"
#include "ManifestResourceLimits.h"
#include "RouteRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace {

QStringList stringList(const QJsonArray &array)
{
    QStringList values;
    values.reserve(array.size());
    for (const QJsonValue value : array) {
        values.append(value.toString());
    }
    return values;
}

QVector<ManifestError> missingRequiredFields(const QJsonObject &object)
{
    static const QStringList required = {QStringLiteral("schemaVersion"),
                                         QStringLiteral("appId"),
                                         QStringLiteral("version"),
                                         QStringLiteral("entryPoint"),
                                         QStringLiteral("runtime"),
                                         QStringLiteral("imports"),
                                         QStringLiteral("permissions"),
                                         QStringLiteral("limits"),
                                         QStringLiteral("routes")};

    QVector<ManifestError> errors;
    for (const QString &key : required) {
        if (!object.contains(key)) {
            errors.append({ManifestErrorCode::MissingRequiredField,
                           QStringLiteral("$.") + key,
                           QStringLiteral("required field is missing")});
        }
    }
    return errors;
}

enum class JsonKind
{
    Number,
    String,
    Object,
    Array,
};

bool hasKind(const QJsonValue &value, const JsonKind kind)
{
    switch (kind) {
    case JsonKind::Number:
        return value.isDouble();
    case JsonKind::String:
        return value.isString();
    case JsonKind::Object:
        return value.isObject();
    case JsonKind::Array:
        return value.isArray();
    }
    return false;
}

QVector<ManifestError> wrongTopLevelTypes(const QJsonObject &object)
{
    const QVector<QPair<QString, JsonKind>> fields = {
        {QStringLiteral("schemaVersion"), JsonKind::Number},
        {QStringLiteral("appId"), JsonKind::String},
        {QStringLiteral("version"), JsonKind::String},
        {QStringLiteral("entryPoint"), JsonKind::String},
        {QStringLiteral("runtime"), JsonKind::Object},
        {QStringLiteral("imports"), JsonKind::Array},
        {QStringLiteral("permissions"), JsonKind::Object},
        {QStringLiteral("limits"), JsonKind::Object},
        {QStringLiteral("routes"), JsonKind::Array},
    };

    QVector<ManifestError> errors;
    for (const auto &[key, kind] : fields) {
        if (object.contains(key) && !hasKind(object.value(key), kind)) {
            errors.append({ManifestErrorCode::WrongType,
                           QStringLiteral("$.") + key,
                           QStringLiteral("field has the wrong JSON type")});
        }
    }
    return errors;
}

void appendUnknownFields(QVector<ManifestError> &errors,
                         const QJsonObject &object,
                         const QSet<QString> &allowed,
                         const QString &path)
{
    QStringList keys = object.keys();
    keys.sort(Qt::CaseSensitive);
    for (const QString &key : keys) {
        if (!allowed.contains(key)) {
            errors.append({ManifestErrorCode::UnknownField,
                           manifestJsonPathMember(path, key),
                           QStringLiteral("field is not allowed")});
        }
    }
}

QVector<ManifestError> unknownFields(const QJsonObject &object)
{
    QVector<ManifestError> errors;
    appendUnknownFields(errors,
                        object,
                        {QStringLiteral("schemaVersion"),
                         QStringLiteral("appId"),
                         QStringLiteral("version"),
                         QStringLiteral("entryPoint"),
                         QStringLiteral("runtime"),
                         QStringLiteral("imports"),
                         QStringLiteral("permissions"),
                         QStringLiteral("limits"),
                         QStringLiteral("routes")},
                        QStringLiteral("$"));

    const QJsonObject runtime = object.value(QStringLiteral("runtime")).toObject();
    appendUnknownFields(errors,
                        runtime,
                        {QStringLiteral("minVersion"), QStringLiteral("maxVersion")},
                        QStringLiteral("$.runtime"));

    const QJsonObject permissions = object.value(QStringLiteral("permissions")).toObject();
    appendUnknownFields(errors,
                        permissions,
                        {QStringLiteral("network"),
                         QStringLiteral("storage"),
                         QStringLiteral("clipboardWrite"),
                         QStringLiteral("clipboardRead"),
                         QStringLiteral("fileOpen"),
                         QStringLiteral("process")},
                        QStringLiteral("$.permissions"));

    const QJsonObject network = permissions.value(QStringLiteral("network")).toObject();
    appendUnknownFields(errors,
                        network,
                        {QStringLiteral("hosts"), QStringLiteral("methods")},
                        QStringLiteral("$.permissions.network"));

    const QJsonObject limits = object.value(QStringLiteral("limits")).toObject();
    appendUnknownFields(errors,
                        limits,
                        {QStringLiteral("packageBytes"),
                         QStringLiteral("memoryMiB"),
                         QStringLiteral("processes")},
                        QStringLiteral("$.limits"));
    return errors;
}

bool isValidAppId(const QString &appId)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z][a-z0-9]*(?:-[a-z0-9]+)*(?:\.[a-z][a-z0-9]*(?:-[a-z0-9]+)*)+$)"));
    if (appId.size() > 253 || !pattern.match(appId).hasMatch()) {
        return false;
    }
    const QStringList labels = appId.split(u'.');
    return std::ranges::all_of(labels, [](const QString &label) { return label.size() <= 63; });
}

bool isStrictSemVer(const QString &version)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)"));
    return pattern.match(version).hasMatch();
}

bool decimalIntegerIsGreater(QStringView left, QStringView right)
{
    if (left.size() != right.size()) {
        return left.size() > right.size();
    }
    return left > right;
}

QVector<ManifestError> validateRuntime(const QJsonObject &runtime)
{
    QVector<ManifestError> errors;
    const QStringList fields = {QStringLiteral("minVersion"), QStringLiteral("maxVersion")};
    for (const QString &field : fields) {
        const QString path = QStringLiteral("$.runtime.") + field;
        if (!runtime.contains(field)) {
            errors.append({ManifestErrorCode::MissingRequiredField,
                           path,
                           QStringLiteral("required field is missing")});
        } else if (!runtime.value(field).isString()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("field has the wrong JSON type")});
        }
    }
    if (!errors.isEmpty()) {
        return errors;
    }

    if (!isStrictSemVer(runtime.value(QStringLiteral("minVersion")).toString())) {
        errors.append({ManifestErrorCode::InvalidSemanticVersion,
                       QStringLiteral("$.runtime.minVersion"),
                       QStringLiteral("minimum runtime must be strict semantic versioning")});
    }
    static const QRegularExpression maximumPattern(
        QStringLiteral(R"(^(0|[1-9]\d*)\.x$)"));
    if (!maximumPattern.match(runtime.value(QStringLiteral("maxVersion")).toString()).hasMatch()) {
        errors.append({ManifestErrorCode::InvalidRuntimeRange,
                       QStringLiteral("$.runtime.maxVersion"),
                       QStringLiteral("maximum runtime must use the N.x range form")});
    }
    if (errors.isEmpty()) {
        const QString minimum = runtime.value(QStringLiteral("minVersion")).toString();
        const QString maximum = runtime.value(QStringLiteral("maxVersion")).toString();
        const QStringView minimumMajor = QStringView(minimum).first(minimum.indexOf(u'.'));
        const QStringView maximumMajor = QStringView(maximum).first(maximum.indexOf(u'.'));
        if (decimalIntegerIsGreater(minimumMajor, maximumMajor)) {
            errors.append({ManifestErrorCode::InvalidRuntimeRange,
                           QStringLiteral("$.runtime.maxVersion"),
                           QStringLiteral("maximum runtime major cannot be below the minimum")});
        }
    }
    return errors;
}

bool isWindowsDeviceSegment(QStringView segment)
{
    const qsizetype extension = segment.indexOf(u'.');
    QString basename = segment.first(extension < 0 ? segment.size() : extension).toString();
    while (basename.endsWith(u' ') || basename.endsWith(u'.')) {
        basename.chop(1);
    }
    const QString upper = basename.toUpper();
    static const QSet<QString> fixedDevices = {QStringLiteral("CON"),
                                               QStringLiteral("PRN"),
                                               QStringLiteral("AUX"),
                                               QStringLiteral("NUL"),
                                               QStringLiteral("CLOCK$")};
    if (fixedDevices.contains(upper)) {
        return true;
    }
    static const QRegularExpression numberedDevices(
        QStringLiteral(R"(^(?:COM|LPT)(?:[1-9]|\x{00B9}|\x{00B2}|\x{00B3})$)"));
    return numberedDevices.match(upper).hasMatch();
}

bool isUnicodeNoncharacter(const char32_t codePoint)
{
    return (codePoint >= 0xfdd0 && codePoint <= 0xfdef)
        || (codePoint <= 0x10ffff && (codePoint & 0xffff) >= 0xfffe);
}

bool containsInvalidWindowsPathCharacter(QStringView segment)
{
    for (qsizetype index = 0; index < segment.size(); ++index) {
        const QChar character = segment.at(index);
        char32_t codePoint = character.unicode();
        if (character.isHighSurrogate()) {
            if (index + 1 >= segment.size() || !segment.at(index + 1).isLowSurrogate()) {
                return true;
            }
            codePoint = QChar::surrogateToUcs4(character, segment.at(++index));
        } else if (character.isLowSurrogate()) {
            return true;
        }
        if (codePoint <= 0x1f || (codePoint >= 0x7f && codePoint <= 0x9f)
            || isUnicodeNoncharacter(codePoint)) {
            return true;
        }
        switch (codePoint) {
        case u'<':
        case u'>':
        case u':':
        case u'"':
        case u'/':
        case u'\\':
        case u'|':
        case u'?':
        case u'*':
        case u'%':
        case u'#':
            return true;
        default:
            break;
        }
    }
    return false;
}

bool isValidEntryPoint(const QString &path)
{
    if (path.isEmpty() || path.startsWith(u'/') || path.startsWith(u'\\')
        || path.contains(QStringLiteral("//")) || !path.endsWith(QStringLiteral(".qml"))) {
        return false;
    }
    const QStringList segments = path.split(u'/', Qt::KeepEmptyParts);
    for (const QString &segment : segments) {
        if (segment.size() > ManifestResourceLimits::MaxWindowsComponentUtf16Units
            || segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral("..")
            || segment.endsWith(u'.') || segment.endsWith(u' ')
            || isWindowsDeviceSegment(segment)) {
            return false;
        }
        if (containsInvalidWindowsPathCharacter(segment)) {
            return false;
        }
    }
    return true;
}

QVector<ManifestError> validateImports(const QJsonArray &imports)
{
    static const QSet<QString> allowed = {QStringLiteral("QtQuick"),
                                          QStringLiteral("QtQuick.Controls"),
                                          QStringLiteral("QtQuick.Layouts"),
                                          QStringLiteral("Company.Design"),
                                          QStringLiteral("Company.Runtime")};
    QSet<QString> seen;
    QVector<ManifestError> errors;
    for (qsizetype index = 0; index < imports.size(); ++index) {
        const QString path = QStringLiteral("$.imports[%1]").arg(index);
        const QJsonValue value = imports.at(index);
        if (!value.isString()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("import must be a string")});
            continue;
        }
        const QString importName = value.toString();
        if (seen.contains(importName)) {
            errors.append({ManifestErrorCode::DuplicateImport,
                           path,
                           QStringLiteral("import must be unique")});
            continue;
        }
        seen.insert(importName);
        if (!allowed.contains(importName)) {
            errors.append({ManifestErrorCode::UnsupportedImport,
                           path,
                           QStringLiteral("import is not allowlisted")});
        }
    }
    return errors;
}

QVector<ManifestError> validatePermissionShape(const QJsonObject &permissions)
{
    QVector<ManifestError> errors;
    const QJsonValue network = permissions.value(QStringLiteral("network"));
    if (!network.isUndefined() && !network.isObject()) {
        errors.append({ManifestErrorCode::WrongType,
                       QStringLiteral("$.permissions.network"),
                       QStringLiteral("network permission must be an object")});
    }

    const QJsonValue storage = permissions.value(QStringLiteral("storage"));
    if (!storage.isUndefined()
        && (!storage.isString() || storage.toString() != QStringLiteral("app-private"))) {
        errors.append({ManifestErrorCode::InvalidStoragePermission,
                       QStringLiteral("$.permissions.storage"),
                       QStringLiteral("storage permission must be app-private")});
    }

    const QJsonValue clipboardWrite = permissions.value(QStringLiteral("clipboardWrite"));
    if (!clipboardWrite.isUndefined() && !clipboardWrite.isBool()) {
        errors.append({ManifestErrorCode::WrongType,
                       QStringLiteral("$.permissions.clipboardWrite"),
                       QStringLiteral("clipboardWrite must be a boolean")});
    }

    const QJsonValue clipboardRead = permissions.value(QStringLiteral("clipboardRead"));
    if (!clipboardRead.isUndefined()
        && (!clipboardRead.isString()
            || clipboardRead.toString() != QStringLiteral("user-gesture"))) {
        errors.append({ManifestErrorCode::InvalidClipboardReadPermission,
                       QStringLiteral("$.permissions.clipboardRead"),
                       QStringLiteral("clipboardRead must be user-gesture")});
    }

    const QJsonValue fileOpen = permissions.value(QStringLiteral("fileOpen"));
    if (!fileOpen.isUndefined()
        && (!fileOpen.isString() || fileOpen.toString() != QStringLiteral("user-brokered"))) {
        errors.append({ManifestErrorCode::InvalidFileOpenPermission,
                       QStringLiteral("$.permissions.fileOpen"),
                       QStringLiteral("fileOpen must be user-brokered")});
    }

    const QJsonValue process = permissions.value(QStringLiteral("process"));
    if (!process.isUndefined()) {
        if (!process.isBool()) {
            errors.append({ManifestErrorCode::WrongType,
                           QStringLiteral("$.permissions.process"),
                           QStringLiteral("process must be a boolean")});
        } else if (process.toBool()) {
            errors.append({ManifestErrorCode::ProcessPermissionDenied,
                           QStringLiteral("$.permissions.process"),
                           QStringLiteral("process permission cannot be enabled")});
        }
    }
    return errors;
}

QVector<ManifestError> validateNetworkShape(const QJsonObject &permissions)
{
    const QJsonValue networkValue = permissions.value(QStringLiteral("network"));
    if (!networkValue.isObject()) {
        return {};
    }
    const QJsonObject network = networkValue.toObject();
    QVector<ManifestError> errors;
    const QJsonValue hosts = network.value(QStringLiteral("hosts"));
    if (hosts.isUndefined()) {
        errors.append({ManifestErrorCode::MissingRequiredField,
                       QStringLiteral("$.permissions.network.hosts"),
                       QStringLiteral("required field is missing")});
    } else if (!hosts.isArray()) {
        errors.append({ManifestErrorCode::WrongType,
                       QStringLiteral("$.permissions.network.hosts"),
                       QStringLiteral("network hosts must be an array")});
    } else if (hosts.toArray().isEmpty()) {
        errors.append({ManifestErrorCode::InvalidNetworkHost,
                       QStringLiteral("$.permissions.network.hosts"),
                       QStringLiteral("at least one network host is required")});
    }

    const QJsonValue methods = network.value(QStringLiteral("methods"));
    if (methods.isUndefined()) {
        errors.append({ManifestErrorCode::MissingRequiredField,
                       QStringLiteral("$.permissions.network.methods"),
                       QStringLiteral("required field is missing")});
    } else if (!methods.isArray()) {
        errors.append({ManifestErrorCode::WrongType,
                       QStringLiteral("$.permissions.network.methods"),
                       QStringLiteral("network methods must be an array")});
    } else if (methods.toArray().isEmpty()) {
        errors.append({ManifestErrorCode::UnsupportedNetworkMethod,
                       QStringLiteral("$.permissions.network.methods"),
                       QStringLiteral("at least one network method is required")});
    }
    return errors;
}

bool isCanonicalIpv4(const QString &host)
{
    const QStringList parts = host.split(u'.', Qt::KeepEmptyParts);
    if (parts.size() != 4) {
        return false;
    }
    for (const QString &part : parts) {
        if (part.isEmpty() || (part.size() > 1 && part.startsWith(u'0'))
            || !std::ranges::all_of(part, [](const QChar character) { return character.isDigit(); })) {
            return false;
        }
        bool ok = false;
        const int octet = part.toInt(&ok);
        if (!ok || octet > 255) {
            return false;
        }
    }
    return true;
}

bool isCanonicalHostname(const QString &host)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*$)"));
    const bool isPureNumeric = !host.isEmpty()
        && std::ranges::all_of(host, [](const QChar character) {
               return character >= u'0' && character <= u'9';
           });
    if (isPureNumeric || host.size() > 253 || !pattern.match(host).hasMatch()) {
        return false;
    }
    const QStringList labels = host.split(u'.');
    return std::ranges::all_of(labels,
                               [](const QString &label) { return label.size() <= 63; });
}

bool isLegacyIpv4Alias(const QString &host)
{
    const QStringList parts = host.split(u'.', Qt::KeepEmptyParts);
    if (parts.isEmpty() || parts.size() > 4) {
        return false;
    }
    static const QRegularExpression numericComponent(
        QStringLiteral(R"(^(?:[0-9]+|0[xX][0-9A-Fa-f]+)$)"));
    return std::ranges::all_of(parts, [](const QString &part) {
        return numericComponent.match(part).hasMatch();
    });
}

bool isCanonicalNetworkHost(const QString &host)
{
    if (isCanonicalIpv4(host)) {
        return true;
    }
    return !isLegacyIpv4Alias(host) && isCanonicalHostname(host);
}

QVector<ManifestError> validateNetworkHosts(const QJsonObject &permissions)
{
    const QJsonValue networkValue = permissions.value(QStringLiteral("network"));
    if (!networkValue.isObject()) {
        return {};
    }
    const QJsonValue hostsValue = networkValue.toObject().value(QStringLiteral("hosts"));
    if (!hostsValue.isArray()) {
        return {};
    }

    QSet<QString> seen;
    QVector<ManifestError> errors;
    const QJsonArray hosts = hostsValue.toArray();
    for (qsizetype index = 0; index < hosts.size(); ++index) {
        const QString path = QStringLiteral("$.permissions.network.hosts[%1]").arg(index);
        const QJsonValue value = hosts.at(index);
        if (!value.isString()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("network host must be a string")});
            continue;
        }
        const QString host = value.toString();
        if (!isCanonicalNetworkHost(host)) {
            errors.append({ManifestErrorCode::InvalidNetworkHost,
                           path,
                           QStringLiteral("network host must be an exact canonical hostname or IPv4 address")});
            continue;
        }
        if (seen.contains(host)) {
            errors.append({ManifestErrorCode::DuplicateNetworkHost,
                           path,
                           QStringLiteral("network host must be unique")});
            continue;
        }
        seen.insert(host);
    }
    return errors;
}

QVector<ManifestError> validateNetworkMethods(const QJsonObject &permissions)
{
    const QJsonValue networkValue = permissions.value(QStringLiteral("network"));
    if (!networkValue.isObject()) {
        return {};
    }
    const QJsonValue methodsValue = networkValue.toObject().value(QStringLiteral("methods"));
    if (!methodsValue.isArray()) {
        return {};
    }

    static const QSet<QString> allowed = {
        QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT")};
    QSet<QString> seen;
    QVector<ManifestError> errors;
    const QJsonArray methods = methodsValue.toArray();
    for (qsizetype index = 0; index < methods.size(); ++index) {
        const QString path = QStringLiteral("$.permissions.network.methods[%1]").arg(index);
        const QJsonValue value = methods.at(index);
        if (!value.isString()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("network method must be a string")});
            continue;
        }
        const QString method = value.toString();
        if (!allowed.contains(method)) {
            errors.append({ManifestErrorCode::UnsupportedNetworkMethod,
                           path,
                           QStringLiteral("network method is not allowlisted")});
            continue;
        }
        if (seen.contains(method)) {
            errors.append({ManifestErrorCode::DuplicateNetworkMethod,
                           path,
                           QStringLiteral("network method must be unique")});
            continue;
        }
        seen.insert(method);
    }
    return errors;
}

struct LimitDefinition
{
    QString key;
    qint64 maximum;
};

const QVector<LimitDefinition> &limitDefinitions()
{
    static const QVector<LimitDefinition> definitions = {
        {QStringLiteral("packageBytes"), 50LL * 1024LL * 1024LL},
        {QStringLiteral("memoryMiB"), 384},
        {QStringLiteral("processes"), 1},
    };
    return definitions;
}

std::optional<ManifestError> exactNumberError(const JsonPreflightResult &preflight)
{
    const QString schemaVersionPath = QStringLiteral("$.schemaVersion");
    const auto schemaVersion = preflight.numberLexemes.constFind(schemaVersionPath);
    if (schemaVersion != preflight.numberLexemes.cend()
        && !manifestExactPositiveJsonInteger(*schemaVersion, 1)) {
        return ManifestError{ManifestErrorCode::UnsupportedSchemaVersion,
                             schemaVersionPath,
                             QStringLiteral("only schema version 1 is supported")};
    }
    for (const LimitDefinition &definition : limitDefinitions()) {
        const QString path = QStringLiteral("$.limits.") + definition.key;
        const auto lexeme = preflight.numberLexemes.constFind(path);
        if (lexeme != preflight.numberLexemes.cend()
            && !manifestExactPositiveJsonInteger(*lexeme, definition.maximum)) {
            return ManifestError{ManifestErrorCode::InvalidLimit,
                                 path,
                                 QStringLiteral("limit must be a positive integer within the host maximum")};
        }
    }
    return std::nullopt;
}

std::optional<ManifestError> resourceBoundError(const JsonPreflightResult &preflight)
{
    struct Bound
    {
        QString path;
        qsizetype maximum;
    };
    const QVector<Bound> stringBounds = {
        {QStringLiteral("$.version"), ManifestResourceLimits::MaxVersionCharacters},
        {QStringLiteral("$.runtime.minVersion"),
         ManifestResourceLimits::MaxVersionCharacters},
        {QStringLiteral("$.runtime.maxVersion"),
         ManifestResourceLimits::MaxVersionCharacters},
        {QStringLiteral("$.entryPoint"), ManifestResourceLimits::MaxEntryPointCharacters},
    };
    const QVector<Bound> arrayBounds = {
        {QStringLiteral("$.imports"), ManifestResourceLimits::MaxImports},
        {QStringLiteral("$.permissions.network.hosts"),
         ManifestResourceLimits::MaxNetworkHosts},
        {QStringLiteral("$.permissions.network.methods"),
         ManifestResourceLimits::MaxNetworkMethods},
        {QStringLiteral("$.routes"), ManifestResourceLimits::MaxRoutes},
    };

    const auto makeError = [](const QString &path) {
        return ManifestError{ManifestErrorCode::ResourceLimitExceeded,
                             path,
                             QStringLiteral("manifest resource bound exceeded")};
    };
    for (const Bound &bound : stringBounds) {
        const auto size = preflight.stringLengths.constFind(bound.path);
        if (size != preflight.stringLengths.cend() && *size > bound.maximum) {
            return makeError(bound.path);
        }
    }
    for (const Bound &bound : arrayBounds) {
        const auto size = preflight.arraySizes.constFind(bound.path);
        if (size != preflight.arraySizes.cend() && *size > bound.maximum) {
            return makeError(bound.path);
        }
    }
    const qsizetype routeCount = std::min(
        preflight.arraySizes.value(QStringLiteral("$.routes")),
        ManifestResourceLimits::MaxRoutes);
    for (qsizetype index = 0; index < routeCount; ++index) {
        const QString path = QStringLiteral("$.routes[%1]").arg(index);
        const auto size = preflight.stringLengths.constFind(path);
        if (size != preflight.stringLengths.cend()
            && *size > ManifestResourceLimits::MaxRouteCharacters) {
            return makeError(path);
        }
    }
    return std::nullopt;
}

QVector<ManifestError> validateLimits(const QJsonObject &limits,
                                      const JsonPreflightResult &preflight)
{

    QVector<ManifestError> errors;
    for (const LimitDefinition &definition : limitDefinitions()) {
        const QString path = QStringLiteral("$.limits.") + definition.key;
        if (!limits.contains(definition.key)) {
            errors.append({ManifestErrorCode::MissingRequiredField,
                           path,
                           QStringLiteral("required field is missing")});
            continue;
        }
        const QJsonValue value = limits.value(definition.key);
        if (!value.isDouble()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("limit must be a JSON integer")});
            continue;
        }
        const auto lexeme = preflight.numberLexemes.constFind(path);
        if (lexeme == preflight.numberLexemes.cend()
            || !manifestExactPositiveJsonInteger(*lexeme, definition.maximum)) {
            errors.append({ManifestErrorCode::InvalidLimit,
                           path,
                           QStringLiteral("limit must be a positive integer within the host maximum")});
        }
    }
    return errors;
}

QVector<ManifestError> validateRoutes(const QJsonArray &routes,
                                      const QString &appId,
                                      const QString &entryPoint)
{
    if (routes.isEmpty()) {
        return {{ManifestErrorCode::InvalidRoute,
                 QStringLiteral("$.routes"),
                 QStringLiteral("at least one route is required")}};
    }

    RouteRegistry registry;
    QVector<ManifestError> errors;
    for (qsizetype index = 0; index < routes.size(); ++index) {
        const QString path = QStringLiteral("$.routes[%1]").arg(index);
        const QJsonValue value = routes.at(index);
        if (!value.isString()) {
            errors.append({ManifestErrorCode::WrongType,
                           path,
                           QStringLiteral("route must be a string")});
            continue;
        }
        const RouteRecord record{
            value.toString(), Engine::QmlWorker, appId, entryPoint};
        switch (registry.add(record)) {
        case RouteAddResult::Added:
            break;
        case RouteAddResult::DuplicateShape:
            errors.append({ManifestErrorCode::DuplicateRoute,
                           path,
                           QStringLiteral("route conflicts with an earlier normalized shape")});
            break;
        case RouteAddResult::InvalidPattern:
        case RouteAddResult::InvalidEngine:
            errors.append({ManifestErrorCode::InvalidRoute,
                           path,
                           QStringLiteral("route pattern is invalid")});
            break;
        }
    }
    return errors;
}

} // namespace

QString manifestErrorCodeName(const ManifestErrorCode code)
{
    switch (code) {
    case ManifestErrorCode::InvalidJson:
        return QStringLiteral("invalid_json");
    case ManifestErrorCode::RootNotObject:
        return QStringLiteral("root_not_object");
    case ManifestErrorCode::MissingRequiredField:
        return QStringLiteral("missing_required_field");
    case ManifestErrorCode::UnknownField:
        return QStringLiteral("unknown_field");
    case ManifestErrorCode::WrongType:
        return QStringLiteral("wrong_type");
    case ManifestErrorCode::UnsupportedSchemaVersion:
        return QStringLiteral("unsupported_schema_version");
    case ManifestErrorCode::InvalidAppId:
        return QStringLiteral("invalid_app_id");
    case ManifestErrorCode::InvalidSemanticVersion:
        return QStringLiteral("invalid_semantic_version");
    case ManifestErrorCode::InvalidRuntimeRange:
        return QStringLiteral("invalid_runtime_range");
    case ManifestErrorCode::InvalidEntryPoint:
        return QStringLiteral("invalid_entry_point");
    case ManifestErrorCode::UnsupportedImport:
        return QStringLiteral("unsupported_import");
    case ManifestErrorCode::DuplicateImport:
        return QStringLiteral("duplicate_import");
    case ManifestErrorCode::InvalidNetworkHost:
        return QStringLiteral("invalid_network_host");
    case ManifestErrorCode::DuplicateNetworkHost:
        return QStringLiteral("duplicate_network_host");
    case ManifestErrorCode::UnsupportedNetworkMethod:
        return QStringLiteral("unsupported_network_method");
    case ManifestErrorCode::DuplicateNetworkMethod:
        return QStringLiteral("duplicate_network_method");
    case ManifestErrorCode::ResourceLimitExceeded:
        return QStringLiteral("resource_limit_exceeded");
    case ManifestErrorCode::InvalidStoragePermission:
        return QStringLiteral("invalid_storage_permission");
    case ManifestErrorCode::InvalidClipboardReadPermission:
        return QStringLiteral("invalid_clipboard_read_permission");
    case ManifestErrorCode::InvalidFileOpenPermission:
        return QStringLiteral("invalid_file_open_permission");
    case ManifestErrorCode::ProcessPermissionDenied:
        return QStringLiteral("process_permission_denied");
    case ManifestErrorCode::InvalidLimit:
        return QStringLiteral("invalid_limit");
    case ManifestErrorCode::InvalidRoute:
        return QStringLiteral("invalid_route");
    case ManifestErrorCode::DuplicateRoute:
        return QStringLiteral("duplicate_route");
    }
    return QStringLiteral("unknown_manifest_error");
}

ManifestParseResult Manifest::parse(const QByteArray &bytes)
{
    if (bytes.size() > ManifestResourceLimits::MaxManifestBytes) {
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::ResourceLimitExceeded,
             QStringLiteral("$"),
             QStringLiteral("manifest resource bound exceeded")}});
    }
    const JsonPreflightResult preflight = preflightManifestJson(bytes);
    if (preflight.error.has_value()) {
        return ManifestParseResult(QVector<ManifestError>{*preflight.error});
    }
    if (const std::optional<ManifestError> error = resourceBoundError(preflight);
        error.has_value()) {
        return ManifestParseResult(QVector<ManifestError>{*error});
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (const std::optional<ManifestError> error = exactNumberError(preflight);
            error.has_value()) {
            return ManifestParseResult(QVector<ManifestError>{*error});
        }
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::InvalidJson, QStringLiteral("$"), QStringLiteral("invalid JSON")}});
    }
    if (!document.isObject()) {
        return ManifestParseResult(QVector<ManifestError>{{ManifestErrorCode::RootNotObject,
                                                           QStringLiteral("$"),
                                                           QStringLiteral("root must be an object")}});
    }

    const QJsonObject object = document.object();
    QVector<ManifestError> errors = missingRequiredFields(object);
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = wrongTopLevelTypes(object);
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = unknownFields(object);
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    const auto schemaVersionLexeme =
        preflight.numberLexemes.constFind(QStringLiteral("$.schemaVersion"));
    if (object.value(QStringLiteral("schemaVersion")).isDouble()
        && (schemaVersionLexeme == preflight.numberLexemes.cend()
            || !manifestExactPositiveJsonInteger(*schemaVersionLexeme, 1))) {
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::UnsupportedSchemaVersion,
             QStringLiteral("$.schemaVersion"),
             QStringLiteral("only schema version 1 is supported")}});
    }
    if (!isValidAppId(object.value(QStringLiteral("appId")).toString())) {
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::InvalidAppId,
             QStringLiteral("$.appId"),
             QStringLiteral("application ID must be a lowercase reverse-DNS identifier")}});
    }
    if (!isStrictSemVer(object.value(QStringLiteral("version")).toString())) {
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::InvalidSemanticVersion,
             QStringLiteral("$.version"),
             QStringLiteral("version must be strict semantic versioning")}});
    }
    errors = validateRuntime(object.value(QStringLiteral("runtime")).toObject());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    if (!isValidEntryPoint(object.value(QStringLiteral("entryPoint")).toString())) {
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::InvalidEntryPoint,
             QStringLiteral("$.entryPoint"),
             QStringLiteral("entry point must be a normalized package-relative QML path")}});
    }
    errors = validateImports(object.value(QStringLiteral("imports")).toArray());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validatePermissionShape(object.value(QStringLiteral("permissions")).toObject());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validateNetworkShape(object.value(QStringLiteral("permissions")).toObject());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validateNetworkHosts(object.value(QStringLiteral("permissions")).toObject());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validateNetworkMethods(object.value(QStringLiteral("permissions")).toObject());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validateLimits(object.value(QStringLiteral("limits")).toObject(), preflight);
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }
    errors = validateRoutes(object.value(QStringLiteral("routes")).toArray(),
                            object.value(QStringLiteral("appId")).toString(),
                            object.value(QStringLiteral("entryPoint")).toString());
    if (!errors.isEmpty()) {
        return ManifestParseResult(std::move(errors));
    }

    Manifest manifest;
    manifest.m_schemaVersion = 1;
    manifest.m_appId = object.value(QStringLiteral("appId")).toString();
    manifest.m_version = object.value(QStringLiteral("version")).toString();
    manifest.m_entryPoint = object.value(QStringLiteral("entryPoint")).toString();

    const QJsonObject runtime = object.value(QStringLiteral("runtime")).toObject();
    manifest.m_runtime.minVersion = runtime.value(QStringLiteral("minVersion")).toString();
    manifest.m_runtime.maxVersion = runtime.value(QStringLiteral("maxVersion")).toString();
    manifest.m_imports = stringList(object.value(QStringLiteral("imports")).toArray());

    const QJsonObject permissions = object.value(QStringLiteral("permissions")).toObject();
    const QJsonObject network = permissions.value(QStringLiteral("network")).toObject();
    manifest.m_permissions.network.hosts = stringList(network.value(QStringLiteral("hosts")).toArray());
    manifest.m_permissions.network.methods = stringList(network.value(QStringLiteral("methods")).toArray());
    if (permissions.value(QStringLiteral("storage")).toString() == QStringLiteral("app-private")) {
        manifest.m_permissions.storage = StoragePermission::AppPrivate;
    }
    manifest.m_permissions.clipboardWrite =
        permissions.value(QStringLiteral("clipboardWrite")).toBool();
    if (permissions.value(QStringLiteral("clipboardRead")).toString()
        == QStringLiteral("user-gesture")) {
        manifest.m_permissions.clipboardRead = ClipboardReadPermission::UserGesture;
    }
    if (permissions.value(QStringLiteral("fileOpen")).toString()
        == QStringLiteral("user-brokered")) {
        manifest.m_permissions.fileOpen = FileOpenPermission::UserBrokered;
    }

    qint64 packageBytes = 0;
    qint64 memoryMiB = 0;
    qint64 processes = 0;
    const bool packageBytesValid = manifestExactPositiveJsonInteger(
        preflight.numberLexemes.value(QStringLiteral("$.limits.packageBytes")),
        50LL * 1024LL * 1024LL,
        &packageBytes);
    const bool memoryMiBValid = manifestExactPositiveJsonInteger(
        preflight.numberLexemes.value(QStringLiteral("$.limits.memoryMiB")), 384, &memoryMiB);
    const bool processesValid = manifestExactPositiveJsonInteger(
        preflight.numberLexemes.value(QStringLiteral("$.limits.processes")), 1, &processes);
    if (!packageBytesValid || !memoryMiBValid || !processesValid) {
        const QString invalidKey = !packageBytesValid
            ? QStringLiteral("packageBytes")
            : (!memoryMiBValid ? QStringLiteral("memoryMiB") : QStringLiteral("processes"));
        return ManifestParseResult(QVector<ManifestError>{
            {ManifestErrorCode::InvalidLimit,
             QStringLiteral("$.limits.") + invalidKey,
             QStringLiteral("limit must be a positive integer within the host maximum")}});
    }
    manifest.m_limits.packageBytes = packageBytes;
    manifest.m_limits.memoryMiB = static_cast<int>(memoryMiB);
    manifest.m_limits.processes = static_cast<int>(processes);
    manifest.m_routes = stringList(object.value(QStringLiteral("routes")).toArray());

    return ManifestParseResult(std::move(manifest));
}

int Manifest::schemaVersion() const noexcept
{
    return m_schemaVersion;
}

const QString &Manifest::appId() const noexcept
{
    return m_appId;
}

const QString &Manifest::version() const noexcept
{
    return m_version;
}

const QString &Manifest::entryPoint() const noexcept
{
    return m_entryPoint;
}

const RuntimeCompatibility &Manifest::runtime() const noexcept
{
    return m_runtime;
}

const QStringList &Manifest::imports() const noexcept
{
    return m_imports;
}

const ManifestPermissions &Manifest::permissions() const noexcept
{
    return m_permissions;
}

const ManifestLimits &Manifest::limits() const noexcept
{
    return m_limits;
}

const QStringList &Manifest::routes() const noexcept
{
    return m_routes;
}

ManifestParseResult::ManifestParseResult(Manifest value)
    : m_value(std::move(value))
{
}

ManifestParseResult::ManifestParseResult(QVector<ManifestError> errors)
    : m_errors(std::move(errors))
{
    Q_ASSERT(!m_errors.isEmpty());
}

bool ManifestParseResult::hasValue() const noexcept
{
    return m_value.has_value();
}

const Manifest &ManifestParseResult::value() const
{
    return m_value.value();
}

const QVector<ManifestError> &ManifestParseResult::errors() const noexcept
{
    return m_errors;
}
