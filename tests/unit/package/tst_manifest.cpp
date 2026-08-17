#include "Manifest.h"
#include "ManifestResourceLimits.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTest>

#include <algorithm>
#include <optional>
#include <type_traits>

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(Q_BROWSER_MANIFEST_FIXTURE_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray withTopLevelValue(const QString &key, const QJsonValue &value)
{
    QJsonObject object = QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
    object.insert(key, value);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray withRuntime(const QJsonObject &runtime)
{
    return withTopLevelValue(QStringLiteral("runtime"), runtime);
}

QByteArray withNetwork(const QJsonObject &network)
{
    return withTopLevelValue(
        QStringLiteral("permissions"),
        QJsonObject{{QStringLiteral("network"), network}, {QStringLiteral("process"), false}});
}

QByteArray withLimits(const QJsonObject &limits)
{
    return withTopLevelValue(QStringLiteral("limits"), limits);
}

QByteArray replacingRaw(QByteArray bytes, const QByteArray &before, const QByteArray &after)
{
    const qsizetype replacements = bytes.count(before);
    if (replacements != 1) {
        return {};
    }
    bytes.replace(before, after);
    return bytes;
}

QByteArray withRawTopLevelMember(QByteArray bytes, const QByteArray &member)
{
    const qsizetype objectStart = bytes.indexOf('{');
    if (objectStart < 0) {
        return {};
    }
    bytes.insert(objectStart + 1, member + ',');
    return bytes;
}

QJsonObject manifestSchema()
{
    QFile file(QStringLiteral(Q_BROWSER_MANIFEST_SCHEMA_FILE));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

std::optional<QJsonArray> ecmaScriptPatternMatches(const QString &pattern,
                                                   const QStringList &values)
{
    QJsonArray jsonValues;
    for (const QString &value : values) {
        jsonValues.append(value);
    }
    const QByteArray request = QJsonDocument(
                                   QJsonArray{pattern, jsonValues})
                                   .toJson(QJsonDocument::Compact);
    QProcess node;
    node.start(QStringLiteral(Q_BROWSER_NODE_EXECUTABLE),
               {QStringLiteral("-e"),
                QStringLiteral("const fs=require('fs');"
                               "const [p,v]=JSON.parse(fs.readFileSync(0,'utf8'));"
                               "const r=[new RegExp(p),new RegExp(p,'u')];"
                               "process.stdout.write(JSON.stringify(v.flatMap(x=>r.map(y=>y.test(x)))));")});
    if (!node.waitForStarted()) {
        return std::nullopt;
    }
    node.write(request);
    node.closeWriteChannel();
    if (!node.waitForFinished() || node.exitStatus() != QProcess::NormalExit
        || node.exitCode() != 0) {
        return std::nullopt;
    }
    const QJsonDocument response = QJsonDocument::fromJson(node.readAllStandardOutput());
    if (!response.isArray()) {
        return std::nullopt;
    }
    return response.array();
}

} // namespace

static_assert(!std::is_default_constructible_v<Manifest>);
static_assert(!std::is_default_constructible_v<ManifestParseResult>);
static_assert(!std::is_constructible_v<ManifestParseResult, Manifest>);
static_assert(!std::is_constructible_v<ManifestParseResult, QVector<ManifestError>>);

class ManifestTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesValidManifest();
    void parseResultStatesAreTotal();
    void rejectsMalformedJson();
    void rejectsDuplicateJsonMembers_data();
    void rejectsDuplicateJsonMembers();
    void rejectsInvalidJsonUnicode_data();
    void rejectsInvalidJsonUnicode();
    void scannerJsonGrammarCorpus_data();
    void scannerJsonGrammarCorpus();
    void usesExactJsonIntegerSemantics_data();
    void usesExactJsonIntegerSemantics();
    void resourceBoundsAreInclusive_data();
    void resourceBoundsAreInclusive();
    void scannerStringAndMemberBounds_data();
    void scannerStringAndMemberBounds();
    void jsonContainerNestingIsExact_data();
    void jsonContainerNestingIsExact();
    void rejectsLargeRouteCorpusWithOneBoundError();
    void rejectsNonObjectRoot();
    void rejectsMissingRequiredField();
    void rejectsUnsupportedSchemaVersion();
    void rejectsNonIntegerSchemaVersion();
    void rejectsWrongTopLevelTypesDeterministically();
    void rejectsUnknownFieldsDeterministically();
    void escapesUnknownFieldPaths_data();
    void escapesUnknownFieldPaths();
    void rejectsInvalidAppId_data();
    void rejectsInvalidAppId();
    void rejectsLoosePackageVersion_data();
    void rejectsLoosePackageVersion();
    void rejectsInvalidRuntimeRange();
    void rejectsMalformedRuntimeObject_data();
    void rejectsMalformedRuntimeObject();
    void acceptsCompatibleRuntimeRange_data();
    void acceptsCompatibleRuntimeRange();
    void rejectsRuntimeMinimumAboveMaximumMajor();
    void rejectsUnsafeEntryPoint_data();
    void rejectsUnsafeEntryPoint();
    void windowsEntryPointCorpusHasSchemaRuntimeParity_data();
    void windowsEntryPointCorpusHasSchemaRuntimeParity();
    void rejectsWorstCaseWindowsEntryPointComponent();
    void entryPointSchemaHandlesEcmaScriptLineSeparators();
    void rejectsUnsupportedAndDuplicateImports();
    void rejectsInvalidPermissionsDeterministically();
    void emptyPermissionsDefaultToDeny();
    void rejectsNonCanonicalAndDuplicateNetworkHosts();
    void canonicalHostCorpusHasSchemaRuntimeParity_data();
    void canonicalHostCorpusHasSchemaRuntimeParity();
    void rejectsUnsupportedAndDuplicateNetworkMethods();
    void rejectsMissingNetworkFields();
    void rejectsMalformedNetworkArrays_data();
    void rejectsMalformedNetworkArrays();
    void rejectsOutOfRangeAndFloatingLimits();
    void rejectsMalformedLimits_data();
    void rejectsMalformedLimits();
    void rejectsInvalidAndDuplicateRoutes();
    void rejectsEmptyRoutes();
    void appIdBoundaryCorpusHasSchemaRuntimeParity_data();
    void appIdBoundaryCorpusHasSchemaRuntimeParity();
    void schemaDeclaresResourceBounds();
    void schemaMatchesRuntimeContract();
};

void ManifestTest::parsesValidManifest()
{
    const ManifestParseResult result = Manifest::parse(fixture(QStringLiteral("valid.json")));

    QVERIFY2(result.hasValue(), "valid manifest should parse");
    QVERIFY(result.errors().isEmpty());
    QCOMPARE(result.value().schemaVersion(), 1);
    QCOMPARE(result.value().appId(), QStringLiteral("com.qbrowser.pilot"));
    QCOMPARE(result.value().version(), QStringLiteral("1.0.0-beta.1+pilot"));
    QCOMPARE(result.value().entryPoint(), QStringLiteral("qml/Main.qml"));
    QCOMPARE(result.value().runtime().minVersion, QStringLiteral("1.0.0"));
    QCOMPARE(result.value().runtime().maxVersion, QStringLiteral("1.x"));
    QCOMPARE(result.value().imports(),
             QStringList({QStringLiteral("QtQuick"),
                          QStringLiteral("QtQuick.Controls"),
                          QStringLiteral("QtQuick.Layouts"),
                          QStringLiteral("Company.Design"),
                          QStringLiteral("Company.Runtime")}));
    QCOMPARE(result.value().permissions().network.hosts,
             QStringList({QStringLiteral("127.0.0.1"), QStringLiteral("api.example.com")}));
    QCOMPARE(result.value().permissions().network.methods,
             QStringList({QStringLiteral("GET"),
                          QStringLiteral("POST"),
                          QStringLiteral("PUT")}));
    QCOMPARE(result.value().permissions().storage, StoragePermission::AppPrivate);
    QCOMPARE(result.value().permissions().clipboardWrite, true);
    QCOMPARE(result.value().permissions().clipboardRead, ClipboardReadPermission::UserGesture);
    QCOMPARE(result.value().permissions().fileOpen, FileOpenPermission::UserBrokered);
    QCOMPARE(result.value().limits().packageBytes, 52428800);
    QCOMPARE(result.value().limits().memoryMiB, 384);
    QCOMPARE(result.value().limits().processes, 1);
    QCOMPARE(result.value().routes().size(), 4);
}

void ManifestTest::parseResultStatesAreTotal()
{
    const ManifestParseResult success = Manifest::parse(fixture(QStringLiteral("valid.json")));
    QVERIFY(success.hasValue());
    QVERIFY(success.errors().isEmpty());

    const ManifestParseResult failure =
        Manifest::parse(fixture(QStringLiteral("invalid-json.json")));
    QVERIFY(!failure.hasValue());
    QVERIFY(!failure.errors().isEmpty());
    QVERIFY_THROWS_EXCEPTION(std::bad_optional_access, static_cast<void>(failure.value()));
}

void ManifestTest::rejectsMalformedJson()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-json.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidJson);
    QCOMPARE(result.errors().front().path, QStringLiteral("$"));
}

void ManifestTest::rejectsDuplicateJsonMembers_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("path");

    const QByteArray valid = fixture(QStringLiteral("valid.json"));
    QTest::newRow("root")
        << replacingRaw(valid,
                        QByteArrayLiteral("\"schemaVersion\": 1,"),
                        QByteArrayLiteral("\"schemaVersion\": 1,\n  \"schemaVersion\": 1,"))
        << QStringLiteral("$.schemaVersion");
    QTest::newRow("nested")
        << replacingRaw(valid,
                        QByteArrayLiteral("\"minVersion\": \"1.0.0\","),
                        QByteArrayLiteral("\"minVersion\": \"1.0.0\",\n    \"minVersion\": \"1.0.0\","))
        << QStringLiteral("$.runtime.minVersion");
    QTest::newRow("deep-network")
        << replacingRaw(valid,
                        QByteArrayLiteral("\"hosts\": [\"127.0.0.1\", \"api.example.com\"],"),
                        QByteArrayLiteral("\"hosts\": [],\n      \"hosts\": [\"127.0.0.1\", \"api.example.com\"],"))
        << QStringLiteral("$.permissions.network.hosts");
    QTest::newRow("decoded-escape-equivalent")
        << QByteArrayLiteral("{\"a\":1,\"\\u0061\":2}") << QStringLiteral("$.a");
    QTest::newRow("escaped-slash-equivalent")
        << QByteArrayLiteral("{\"a/b\":1,\"a\\/b\":2}") << QStringLiteral("$[\"a/b\"]");
}

void ManifestTest::rejectsDuplicateJsonMembers()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, path);
    QVERIFY(!bytes.isEmpty());

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidJson);
    QCOMPARE(result.errors().front().path, path);
    QCOMPARE(manifestErrorCodeName(result.errors().front().code), QStringLiteral("invalid_json"));
}

void ManifestTest::rejectsInvalidJsonUnicode_data()
{
    QTest::addColumn<QByteArray>("bytes");

    const QByteArray valid = fixture(QStringLiteral("valid.json"));
    QTest::newRow("lone-high-surrogate")
        << replacingRaw(valid,
                        QByteArrayLiteral("com.qbrowser.pilot"),
                        QByteArrayLiteral("com.qbrowser.\\uD800"));
    QTest::newRow("lone-low-surrogate")
        << replacingRaw(valid,
                        QByteArrayLiteral("com.qbrowser.pilot"),
                        QByteArrayLiteral("com.qbrowser.\\uDC00"));
    QTest::newRow("high-followed-by-non-low")
        << replacingRaw(valid,
                        QByteArrayLiteral("com.qbrowser.pilot"),
                        QByteArrayLiteral("com.qbrowser.\\uD800\\u0041"));
    QByteArray invalidUtf8 = valid;
    const qsizetype appIdOffset = invalidUtf8.indexOf(QByteArrayLiteral("com.qbrowser.pilot"));
    QVERIFY(appIdOffset >= 0);
    invalidUtf8[appIdOffset] = static_cast<char>(0xff);
    QTest::newRow("invalid-utf8") << invalidUtf8;
    const auto jsonWithRawUtf8 = [](const QByteArray &encoded) {
        return QByteArrayLiteral("{\"probe\":\"") + encoded + QByteArrayLiteral("\"}");
    };
    QTest::newRow("utf8-overlong")
        << jsonWithRawUtf8(QByteArray::fromHex(QByteArrayLiteral("c0af")));
    QTest::newRow("utf8-encoded-surrogate")
        << jsonWithRawUtf8(QByteArray::fromHex(QByteArrayLiteral("eda080")));
    QTest::newRow("utf8-above-u10ffff")
        << jsonWithRawUtf8(QByteArray::fromHex(QByteArrayLiteral("f4908080")));
    QTest::newRow("utf8-invalid-continuation")
        << jsonWithRawUtf8(QByteArray::fromHex(QByteArrayLiteral("e228a1")));
    QTest::newRow("utf8-truncated")
        << jsonWithRawUtf8(QByteArray::fromHex(QByteArrayLiteral("e282")));
}

void ManifestTest::rejectsInvalidJsonUnicode()
{
    QFETCH(QByteArray, bytes);
    QVERIFY(!bytes.isEmpty());

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidJson);
    QCOMPARE(result.errors().front().path, QStringLiteral("$"));
}

void ManifestTest::scannerJsonGrammarCorpus_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    const QByteArray valid = fixture(QStringLiteral("valid.json"));
    const auto withProbe = [&valid](const QByteArray &rawValue) {
        return withRawTopLevelMember(valid, QByteArrayLiteral("\"probe\":") + rawValue);
    };

    const QVector<QPair<QByteArray, QByteArray>> invalidStrings = {
        {QByteArrayLiteral("invalid-escape-x"), QByteArrayLiteral("\"\\x41\"")},
        {QByteArrayLiteral("invalid-unicode-hex"), QByteArrayLiteral("\"\\u12G4\"")},
        {QByteArrayLiteral("truncated-unicode-escape"), QByteArrayLiteral("\"\\u123\"")},
        {QByteArrayLiteral("trailing-backslash"), QByteArrayLiteral("\"value\\\"")},
    };
    for (const auto &[name, rawValue] : invalidStrings) {
        QTest::addRow("%s", name.constData())
            << withProbe(rawValue) << ManifestErrorCode::InvalidJson << QStringLiteral("$");
    }

    const QVector<QPair<QByteArray, QByteArray>> invalidNumbers = {
        {QByteArrayLiteral("number-leading-zero"), QByteArrayLiteral("01")},
        {QByteArrayLiteral("number-missing-fraction"), QByteArrayLiteral("1.")},
        {QByteArrayLiteral("number-missing-exponent"), QByteArrayLiteral("1e")},
        {QByteArrayLiteral("number-missing-signed-exponent"), QByteArrayLiteral("1e+")},
    };
    for (const auto &[name, rawValue] : invalidNumbers) {
        QTest::addRow("%s", name.constData())
            << withProbe(rawValue) << ManifestErrorCode::InvalidJson << QStringLiteral("$");
    }

    const QVector<QPair<QByteArray, QByteArray>> validNumbers = {
        {QByteArrayLiteral("number-negative-integer"), QByteArrayLiteral("-1")},
        {QByteArrayLiteral("number-negative-fraction"), QByteArrayLiteral("-0.25")},
        {QByteArrayLiteral("number-positive-exponent"), QByteArrayLiteral("1e+2")},
        {QByteArrayLiteral("number-fraction-exponent"), QByteArrayLiteral("1.25e-2")},
    };
    for (const auto &[name, rawValue] : validNumbers) {
        QTest::addRow("%s", name.constData())
            << withProbe(rawValue) << ManifestErrorCode::UnknownField
            << QStringLiteral("$.probe");
    }
}

void ManifestTest::scannerJsonGrammarCorpus()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);
    QVERIFY(!bytes.isEmpty());

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::usesExactJsonIntegerSemantics_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    const QByteArray valid = fixture(QStringLiteral("valid.json"));
    QTest::newRow("schema-mathematical-one-decimal")
        << replacingRaw(valid, QByteArrayLiteral("\"schemaVersion\": 1"),
                        QByteArrayLiteral("\"schemaVersion\": 1.0"))
        << true << ManifestErrorCode::InvalidJson << QString{};
    QTest::newRow("schema-mathematical-one-exponent")
        << replacingRaw(valid, QByteArrayLiteral("\"schemaVersion\": 1"),
                        QByteArrayLiteral("\"schemaVersion\": 1e0"))
        << true << ManifestErrorCode::InvalidJson << QString{};
    QTest::newRow("schema-rounded-fraction")
        << replacingRaw(valid, QByteArrayLiteral("\"schemaVersion\": 1"),
                        QByteArrayLiteral("\"schemaVersion\": 1.0000000000000001"))
        << false << ManifestErrorCode::UnsupportedSchemaVersion
        << QStringLiteral("$.schemaVersion");
    QTest::newRow("limit-mathematical-integer-decimal")
        << replacingRaw(valid, QByteArrayLiteral("\"memoryMiB\": 384"),
                        QByteArrayLiteral("\"memoryMiB\": 1.00e2"))
        << true << ManifestErrorCode::InvalidJson << QString{};
    QTest::newRow("limit-mathematical-integer-exponent")
        << replacingRaw(valid, QByteArrayLiteral("\"processes\": 1\n"),
                        QByteArrayLiteral("\"processes\": 1e0\n"))
        << true << ManifestErrorCode::InvalidJson << QString{};
    QTest::newRow("limit-rounded-fraction")
        << replacingRaw(valid, QByteArrayLiteral("\"packageBytes\": 52428800"),
                        QByteArrayLiteral("\"packageBytes\": 52428800.000000001"))
        << false << ManifestErrorCode::InvalidLimit << QStringLiteral("$.limits.packageBytes");
    QTest::newRow("limit-overflowing-exponent")
        << replacingRaw(valid, QByteArrayLiteral("\"packageBytes\": 52428800"),
                        QByteArrayLiteral("\"packageBytes\": 1e9999"))
        << false << ManifestErrorCode::InvalidLimit << QStringLiteral("$.limits.packageBytes");
}

void ManifestTest::usesExactJsonIntegerSemantics()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, valid);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);
    QVERIFY(!bytes.isEmpty());

    const ManifestParseResult result = Manifest::parse(bytes);

    QCOMPARE(result.hasValue(), valid);
    if (valid) {
        QVERIFY(result.errors().isEmpty());
        return;
    }
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::resourceBoundsAreInclusive_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("atLimit");
    QTest::addColumn<QString>("path");

    QByteArray manifestAtLimit = fixture(QStringLiteral("valid.json"));
    manifestAtLimit.append(ManifestResourceLimits::MaxManifestBytes - manifestAtLimit.size(),
                           ' ');
    QTest::newRow("manifest-bytes-max") << manifestAtLimit << true << QStringLiteral("$");
    QByteArray manifestOverLimit = manifestAtLimit;
    manifestOverLimit.append(' ');
    QTest::newRow("manifest-bytes-max-plus-one")
        << manifestOverLimit << false << QStringLiteral("$");

    const QString versionAtLimit = QStringLiteral("1.0.0+") + QString(122, u'a');
    QCOMPARE(versionAtLimit.size(), ManifestResourceLimits::MaxVersionCharacters);
    QTest::newRow("version-max")
        << withTopLevelValue(QStringLiteral("version"), versionAtLimit) << true
        << QStringLiteral("$.version");
    QTest::newRow("version-max-plus-one")
        << withTopLevelValue(QStringLiteral("version"), versionAtLimit + u'a') << false
        << QStringLiteral("$.version");

    const QString minimumAtLimit = QStringLiteral("1.0.0+") + QString(122, u'a');
    QJsonObject runtime = QJsonDocument::fromJson(fixture(QStringLiteral("valid.json")))
                              .object()
                              .value(QStringLiteral("runtime"))
                              .toObject();
    runtime.insert(QStringLiteral("minVersion"), minimumAtLimit);
    QTest::newRow("runtime-min-max") << withRuntime(runtime) << true
                                      << QStringLiteral("$.runtime.minVersion");
    runtime.insert(QStringLiteral("minVersion"), minimumAtLimit + u'a');
    QTest::newRow("runtime-min-max-plus-one") << withRuntime(runtime) << false
                                               << QStringLiteral("$.runtime.minVersion");

    const QString maximumAtLimit = QString(126, u'9') + QStringLiteral(".x");
    QCOMPARE(maximumAtLimit.size(), ManifestResourceLimits::MaxVersionCharacters);
    runtime.insert(QStringLiteral("minVersion"), QStringLiteral("1.0.0"));
    runtime.insert(QStringLiteral("maxVersion"), maximumAtLimit);
    QTest::newRow("runtime-max-max") << withRuntime(runtime) << true
                                      << QStringLiteral("$.runtime.maxVersion");
    runtime.insert(QStringLiteral("maxVersion"), QString(127, u'9') + QStringLiteral(".x"));
    QTest::newRow("runtime-max-max-plus-one") << withRuntime(runtime) << false
                                               << QStringLiteral("$.runtime.maxVersion");

    const QString entryPointAtLimit = QStringLiteral("qml/")
        + QString(ManifestResourceLimits::MaxEntryPointCharacters - 8, u'a')
        + QStringLiteral(".qml");
    QCOMPARE(entryPointAtLimit.size(), ManifestResourceLimits::MaxEntryPointCharacters);
    QTest::newRow("entry-point-max")
        << withTopLevelValue(QStringLiteral("entryPoint"), entryPointAtLimit) << true
        << QStringLiteral("$.entryPoint");
    QTest::newRow("entry-point-max-plus-one")
        << withTopLevelValue(QStringLiteral("entryPoint"),
                             QStringLiteral("qml/")
                                 + QString(ManifestResourceLimits::MaxEntryPointCharacters - 7,
                                           u'a')
                                 + QStringLiteral(".qml"))
        << false << QStringLiteral("$.entryPoint");
    const char32_t emoji = 0x1f600;
    QString unicodeEntryPointAtLimit = QStringLiteral("qml/");
    for (qsizetype index = 0;
         index < ManifestResourceLimits::MaxEntryPointCharacters - 8;
         ++index) {
        unicodeEntryPointAtLimit.append(QString::fromUcs4(&emoji, 1));
    }
    unicodeEntryPointAtLimit.append(QStringLiteral(".qml"));
    QTest::newRow("entry-point-unicode-scalar-max")
        << withTopLevelValue(QStringLiteral("entryPoint"), unicodeEntryPointAtLimit) << true
        << QStringLiteral("$.entryPoint");
    unicodeEntryPointAtLimit.insert(unicodeEntryPointAtLimit.size() - 4,
                                    QString::fromUcs4(&emoji, 1));
    QTest::newRow("entry-point-unicode-scalar-max-plus-one")
        << withTopLevelValue(QStringLiteral("entryPoint"), unicodeEntryPointAtLimit) << false
        << QStringLiteral("$.entryPoint");

    QJsonArray importsAtLimit;
    for (qsizetype index = 0; index < ManifestResourceLimits::MaxImports; ++index) {
        importsAtLimit.append(QStringLiteral("QtQuick"));
    }
    QTest::newRow("imports-max")
        << withTopLevelValue(QStringLiteral("imports"), importsAtLimit) << true
        << QStringLiteral("$.imports");
    QJsonArray importsOverLimit = importsAtLimit;
    importsOverLimit.append(QStringLiteral("QtQuick"));
    QTest::newRow("imports-max-plus-one")
        << withTopLevelValue(QStringLiteral("imports"), importsOverLimit) << false
        << QStringLiteral("$.imports");

    QJsonArray hostsAtLimit;
    for (qsizetype index = 0; index < ManifestResourceLimits::MaxNetworkHosts; ++index) {
        hostsAtLimit.append(QStringLiteral("host%1.example.com").arg(index));
    }
    QTest::newRow("hosts-max")
        << withNetwork({{QStringLiteral("hosts"), hostsAtLimit},
                        {QStringLiteral("methods"), QJsonArray{QStringLiteral("GET")}}})
        << true << QStringLiteral("$.permissions.network.hosts");
    QJsonArray hostsOverLimit = hostsAtLimit;
    hostsOverLimit.append(QStringLiteral("overflow.example.com"));
    QTest::newRow("hosts-max-plus-one")
        << withNetwork({{QStringLiteral("hosts"), hostsOverLimit},
                        {QStringLiteral("methods"), QJsonArray{QStringLiteral("GET")}}})
        << false << QStringLiteral("$.permissions.network.hosts");

    const QJsonArray methodsAtLimit{QStringLiteral("GET"),
                                    QStringLiteral("POST"),
                                    QStringLiteral("PUT")};
    QTest::newRow("methods-max")
        << withNetwork({{QStringLiteral("hosts"), QJsonArray{QStringLiteral("example.com")}},
                        {QStringLiteral("methods"), methodsAtLimit}})
        << true << QStringLiteral("$.permissions.network.methods");
    QJsonArray methodsOverLimit = methodsAtLimit;
    methodsOverLimit.append(QStringLiteral("GET"));
    QTest::newRow("methods-max-plus-one")
        << withNetwork({{QStringLiteral("hosts"), QJsonArray{QStringLiteral("example.com")}},
                        {QStringLiteral("methods"), methodsOverLimit}})
        << false << QStringLiteral("$.permissions.network.methods");

    QJsonArray routesAtLimit;
    for (qsizetype index = 0; index < ManifestResourceLimits::MaxRoutes; ++index) {
        routesAtLimit.append(QStringLiteral("/route%1").arg(index));
    }
    QTest::newRow("routes-max")
        << withTopLevelValue(QStringLiteral("routes"), routesAtLimit) << true
        << QStringLiteral("$.routes");
    QJsonArray routesOverLimit = routesAtLimit;
    routesOverLimit.append(QStringLiteral("/overflow"));
    QTest::newRow("routes-max-plus-one")
        << withTopLevelValue(QStringLiteral("routes"), routesOverLimit) << false
        << QStringLiteral("$.routes");

    const QString routeAtLimit = u'/' + QString(2047, u'a');
    QCOMPARE(routeAtLimit.size(), ManifestResourceLimits::MaxRouteCharacters);
    QTest::newRow("route-string-max")
        << withTopLevelValue(QStringLiteral("routes"), QJsonArray{routeAtLimit}) << true
        << QStringLiteral("$.routes[0]");
    QTest::newRow("route-string-max-plus-one")
        << withTopLevelValue(QStringLiteral("routes"),
                             QJsonArray{routeAtLimit + u'a'})
        << false << QStringLiteral("$.routes[0]");
}

void ManifestTest::resourceBoundsAreInclusive()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, atLimit);
    QFETCH(QString, path);

    const ManifestParseResult result = Manifest::parse(bytes);
    if (atLimit) {
        QVERIFY(std::ranges::none_of(result.errors(), [](const ManifestError &error) {
            return error.code == ManifestErrorCode::ResourceLimitExceeded;
        }));
        return;
    }

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::ResourceLimitExceeded);
    QCOMPARE(result.errors().front().path, path);
    QCOMPARE(manifestErrorCodeName(result.errors().front().code),
             QStringLiteral("resource_limit_exceeded"));
}

void ManifestTest::scannerStringAndMemberBounds_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    const QByteArray valid = fixture(QStringLiteral("valid.json"));
    const auto withUnknownString = [&valid](const QByteArray &rawContents) {
        return withRawTopLevelMember(
            valid, QByteArrayLiteral("\"probe\":\"") + rawContents + QByteArrayLiteral("\""));
    };

    QTest::newRow("decoded-string-max")
        << withUnknownString(QByteArray(ManifestResourceLimits::MaxJsonStringCharacters, 'a'))
        << ManifestErrorCode::UnknownField
        << QStringLiteral("$.probe");
    QTest::newRow("decoded-string-max-plus-one")
        << withUnknownString(
               QByteArray(ManifestResourceLimits::MaxJsonStringCharacters + 1, 'a'))
        << ManifestErrorCode::ResourceLimitExceeded << QStringLiteral("$.probe");
    constexpr qsizetype escapedScalarBytes = 6;
    constexpr qsizetype rawEscapedScalars =
        ManifestResourceLimits::MaxJsonStringRawCharacters / escapedScalarBytes;
    QTest::newRow("raw-escaped-string-max")
        << withUnknownString(QByteArrayLiteral("\\u0061").repeated(rawEscapedScalars))
        << ManifestErrorCode::UnknownField << QStringLiteral("$.probe");
    QTest::newRow("raw-escaped-string-max-plus-one")
        << withUnknownString(QByteArrayLiteral("\\u0061").repeated(rawEscapedScalars + 1))
        << ManifestErrorCode::ResourceLimitExceeded << QStringLiteral("$.probe");

    QJsonObject object = QJsonDocument::fromJson(valid).object();
    const QString memberAtLimit(ManifestResourceLimits::MaxJsonMemberNameCharacters, u'k');
    object.insert(memberAtLimit, true);
    QTest::newRow("member-name-max")
        << QJsonDocument(object).toJson(QJsonDocument::Compact)
        << ManifestErrorCode::UnknownField << (QStringLiteral("$.") + memberAtLimit);
    object.remove(memberAtLimit);
    object.insert(QString(ManifestResourceLimits::MaxJsonMemberNameCharacters + 1, u'k'), true);
    QTest::newRow("member-name-max-plus-one")
        << QJsonDocument(object).toJson(QJsonDocument::Compact)
        << ManifestErrorCode::ResourceLimitExceeded << QStringLiteral("$");

    QByteArray amplified = QByteArrayLiteral("{\"");
    amplified.append(QByteArray(ManifestResourceLimits::MaxManifestBytes - 64, 'k'));
    amplified.append(QByteArrayLiteral("\":[[[[[[[[[]]]]]]]]]}"));
    QVERIFY(amplified.size() < ManifestResourceLimits::MaxManifestBytes);
    QTest::newRow("near-limit-key-with-deep-value")
        << amplified << ManifestErrorCode::ResourceLimitExceeded << QStringLiteral("$");
}

void ManifestTest::scannerStringAndMemberBounds()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);

    QElapsedTimer timer;
    timer.start();
    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY2(timer.elapsed() < 5'000, "scanner bounds must prevent path/string amplification");
    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::jsonContainerNestingIsExact_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");

    const auto nested = [](const qsizetype containers, const bool scalarLeaf) {
        QByteArray bytes(containers, '[');
        if (scalarLeaf) {
            bytes.append('0');
        }
        bytes.append(QByteArray(containers, ']'));
        return bytes;
    };

    QTest::newRow("max-empty-container")
        << nested(ManifestResourceLimits::MaxJsonContainerNesting, false)
                                          << ManifestErrorCode::RootNotObject;
    QTest::newRow("max-container-with-scalar")
        << nested(ManifestResourceLimits::MaxJsonContainerNesting, true)
                                                << ManifestErrorCode::RootNotObject;
    QTest::newRow("max-plus-one-empty-container")
        << nested(ManifestResourceLimits::MaxJsonContainerNesting + 1, false)
        << ManifestErrorCode::ResourceLimitExceeded;
    QTest::newRow("max-plus-one-container-with-scalar")
        << nested(ManifestResourceLimits::MaxJsonContainerNesting + 1, true)
        << ManifestErrorCode::ResourceLimitExceeded;
}

void ManifestTest::jsonContainerNestingIsExact()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, QStringLiteral("$"));
}

void ManifestTest::rejectsLargeRouteCorpusWithOneBoundError()
{
    QJsonArray routes;
    for (qsizetype index = 0; index < 50'000; ++index) {
        routes.append(QStringLiteral("/route-%1").arg(index, 5, 10, QLatin1Char('0')));
    }
    const QByteArray bytes = withTopLevelValue(QStringLiteral("routes"), routes);
    QVERIFY(bytes.size() < ManifestResourceLimits::MaxManifestBytes);
    QVERIFY(bytes.size() > ManifestResourceLimits::MaxManifestBytes / 2);

    QElapsedTimer timer;
    timer.start();
    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY2(timer.elapsed() < 5'000, "oversized route corpus must bypass RouteRegistry");
    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::ResourceLimitExceeded);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.routes"));
}

void ManifestTest::rejectsNonObjectRoot()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-root.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::RootNotObject);
    QCOMPARE(result.errors().front().path, QStringLiteral("$"));
}

void ManifestTest::rejectsMissingRequiredField()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-missing-entry-point.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::MissingRequiredField);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
    QCOMPARE(manifestErrorCodeName(result.errors().front().code),
             QStringLiteral("missing_required_field"));
}

void ManifestTest::rejectsUnsupportedSchemaVersion()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-schema-version.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::UnsupportedSchemaVersion);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.schemaVersion"));
}

void ManifestTest::rejectsNonIntegerSchemaVersion()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-schema-floating.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::UnsupportedSchemaVersion);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.schemaVersion"));
}

void ManifestTest::rejectsWrongTopLevelTypesDeterministically()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-types.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 9);
    const QStringList expectedPaths = {QStringLiteral("$.schemaVersion"),
                                       QStringLiteral("$.appId"),
                                       QStringLiteral("$.version"),
                                       QStringLiteral("$.entryPoint"),
                                       QStringLiteral("$.runtime"),
                                       QStringLiteral("$.imports"),
                                       QStringLiteral("$.permissions"),
                                       QStringLiteral("$.limits"),
                                       QStringLiteral("$.routes")};
    for (qsizetype index = 0; index < result.errors().size(); ++index) {
        QCOMPARE(result.errors().at(index).code, ManifestErrorCode::WrongType);
        QCOMPARE(result.errors().at(index).path, expectedPaths.at(index));
    }
}

void ManifestTest::rejectsUnknownFieldsDeterministically()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-unknown-fields.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 5);
    const QStringList expectedPaths = {QStringLiteral("$.signature"),
                                       QStringLiteral("$.runtime.channel"),
                                       QStringLiteral("$.permissions.shell"),
                                       QStringLiteral("$.permissions.network.paths"),
                                       QStringLiteral("$.limits.threads")};
    for (qsizetype index = 0; index < result.errors().size(); ++index) {
        QCOMPARE(result.errors().at(index).code, ManifestErrorCode::UnknownField);
        QCOMPARE(result.errors().at(index).path, expectedPaths.at(index));
    }
}

void ManifestTest::escapesUnknownFieldPaths_data()
{
    QTest::addColumn<QString>("member");
    QTest::addColumn<QString>("path");

    QTest::newRow("newline") << QStringLiteral("a\nb") << QStringLiteral("$[\"a\\nb\"]");
    QTest::newRow("quote") << QStringLiteral("a\"b") << QStringLiteral("$[\"a\\\"b\"]");
    QTest::newRow("backslash") << QStringLiteral("a\\b")
                                << QStringLiteral("$[\"a\\\\b\"]");
    QTest::newRow("dot-is-unambiguous") << QStringLiteral("a.b")
                                         << QStringLiteral("$[\"a.b\"]");
}

void ManifestTest::escapesUnknownFieldPaths()
{
    QFETCH(QString, member);
    QFETCH(QString, path);
    QJsonObject object = QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
    object.insert(member, true);

    const ManifestParseResult result =
        Manifest::parse(QJsonDocument(object).toJson(QJsonDocument::Compact));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::UnknownField);
    QCOMPARE(result.errors().front().path, path);
    QVERIFY(!result.errors().front().path.contains(u'\n'));
    QVERIFY(!result.errors().front().path.contains(u'\r'));
    QVERIFY(!result.errors().front().message.contains(u'\n'));
    QVERIFY(!result.errors().front().message.contains(u'\r'));
}

void ManifestTest::rejectsInvalidAppId_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QTest::newRow("fixture-empty-label")
        << fixture(QStringLiteral("invalid-app-id.json"));
    QTest::newRow("uppercase")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("Com.qbrowser.pilot"));
    QTest::newRow("wildcard")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("com.qbrowser.*"));
    QTest::newRow("path")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("com.qbrowser/pilot"));
    QTest::newRow("userinfo")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("user@com.qbrowser.pilot"));
    QTest::newRow("single-label")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("pilot"));
    QTest::newRow("leading-hyphen")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("com.-pilot.app"));
    QTest::newRow("underscore")
        << withTopLevelValue(QStringLiteral("appId"), QStringLiteral("com.q_browser.pilot"));
}

void ManifestTest::rejectsInvalidAppId()
{
    QFETCH(QByteArray, bytes);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidAppId);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.appId"));
}

void ManifestTest::rejectsLoosePackageVersion_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QTest::newRow("fixture-leading-zero") << fixture(QStringLiteral("invalid-version.json"));
    const QStringList invalidVersions = {QStringLiteral("1"),
                                         QStringLiteral("1.0"),
                                         QStringLiteral("1.00.0"),
                                         QStringLiteral("1.0.0.0"),
                                         QStringLiteral("v1.0.0"),
                                         QStringLiteral("1.0.0-"),
                                         QStringLiteral("1.0.0-01"),
                                         QStringLiteral("1.0.0+"),
                                         QStringLiteral("1.0.0 ")};
    for (const QString &version : invalidVersions) {
        QTest::addRow("%s", qPrintable(version))
            << withTopLevelValue(QStringLiteral("version"), version);
    }
}

void ManifestTest::rejectsLoosePackageVersion()
{
    QFETCH(QByteArray, bytes);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidSemanticVersion);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.version"));
}

void ManifestTest::rejectsInvalidRuntimeRange()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-runtime.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 2);
    QCOMPARE(result.errors().at(0).code, ManifestErrorCode::InvalidSemanticVersion);
    QCOMPARE(result.errors().at(0).path, QStringLiteral("$.runtime.minVersion"));
    QCOMPARE(result.errors().at(1).code, ManifestErrorCode::InvalidRuntimeRange);
    QCOMPARE(result.errors().at(1).path, QStringLiteral("$.runtime.maxVersion"));
}

void ManifestTest::rejectsMalformedRuntimeObject_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    QTest::newRow("missing-min")
        << withRuntime({{QStringLiteral("maxVersion"), QStringLiteral("1.x")}})
        << ManifestErrorCode::MissingRequiredField << QStringLiteral("$.runtime.minVersion");
    QTest::newRow("wrong-max-type")
        << withRuntime({{QStringLiteral("minVersion"), QStringLiteral("1.0.0")},
                        {QStringLiteral("maxVersion"), 1}})
        << ManifestErrorCode::WrongType << QStringLiteral("$.runtime.maxVersion");
}

void ManifestTest::rejectsMalformedRuntimeObject()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::acceptsCompatibleRuntimeRange_data()
{
    QTest::addColumn<QString>("minimum");
    QTest::addColumn<QString>("maximum");

    QTest::newRow("same-major") << QStringLiteral("1.2.3") << QStringLiteral("1.x");
    QTest::newRow("lower-major") << QStringLiteral("0.9.0") << QStringLiteral("1.x");
    QTest::newRow("large-equal-major")
        << QStringLiteral("999999999999999999999.0.0")
        << QStringLiteral("999999999999999999999.x");
}

void ManifestTest::acceptsCompatibleRuntimeRange()
{
    QFETCH(QString, minimum);
    QFETCH(QString, maximum);

    const ManifestParseResult result = Manifest::parse(
        withRuntime({{QStringLiteral("minVersion"), minimum},
                     {QStringLiteral("maxVersion"), maximum}}));

    QVERIFY(result.hasValue());
    QVERIFY(result.errors().isEmpty());
}

void ManifestTest::rejectsRuntimeMinimumAboveMaximumMajor()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-runtime-relation.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidRuntimeRange);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.runtime.maxVersion"));
}

void ManifestTest::rejectsUnsafeEntryPoint_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QTest::newRow("fixture-traversal")
        << fixture(QStringLiteral("invalid-entry-point.json"));
    const QStringList paths = {QStringLiteral("/qml/Main.qml"),
                               QStringLiteral("C:/qml/Main.qml"),
                               QStringLiteral("//server/share/Main.qml"),
                               QStringLiteral("qml\\Main.qml"),
                               QStringLiteral("qml/./Main.qml"),
                               QStringLiteral("qml//Main.qml"),
                               QStringLiteral("qml/%2e%2e/Main.qml"),
                               QStringLiteral("qml/Main:stream.qml"),
                               QStringLiteral("qml/Main.qml?debug=1"),
                               QStringLiteral("qml/Main.QML"),
                               QStringLiteral("qml/Main.js")};
    for (const QString &path : paths) {
        QTest::addRow("%s", qPrintable(path))
            << withTopLevelValue(QStringLiteral("entryPoint"), path);
    }
}

void ManifestTest::rejectsUnsafeEntryPoint()
{
    QFETCH(QByteArray, bytes);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidEntryPoint);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
}

void ManifestTest::windowsEntryPointCorpusHasSchemaRuntimeParity_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("valid");

    QTest::newRow("normal-dotted-file") << QStringLiteral("qml/pages/Main.view.qml") << true;
    QTest::newRow("normal-dotted-directory") << QStringLiteral("qml/v1.2/Main.qml") << true;
    QTest::newRow("normal-unicode") << QStringLiteral("qml/页面/报告.qml") << true;
    QTest::newRow("device-prefix-is-normal") << QStringLiteral("qml/concept/Main.qml") << true;
    QTest::newRow("com-zero-is-normal") << QStringLiteral("qml/COM0.qml") << true;
    QTest::newRow("com-ten-is-normal") << QStringLiteral("qml/COM10.qml") << true;
    QTest::newRow("lpt-zero-is-normal") << QStringLiteral("qml/LPT0.qml") << true;
    QTest::newRow("lpt-ten-is-normal") << QStringLiteral("qml/LPT10.qml") << true;
    QTest::newRow("less-than") << QStringLiteral("qml/less<than/Main.qml") << false;
    QTest::newRow("greater-than") << QStringLiteral("qml/greater>than/Main.qml") << false;
    QTest::newRow("double-quote") << QStringLiteral("qml/quote\"name/Main.qml") << false;
    QTest::newRow("pipe") << QStringLiteral("qml/pipe|name/Main.qml") << false;
    QTest::newRow("asterisk") << QStringLiteral("qml/star*name/Main.qml") << false;
    QTest::newRow("colon-regression") << QStringLiteral("qml/name:stream/Main.qml") << false;
    QTest::newRow("question-regression") << QStringLiteral("qml/Main?.qml") << false;
    QTest::newRow("duplicate-slash-regression") << QStringLiteral("qml//Main.qml") << false;
    QTest::newRow("backslash-regression") << QStringLiteral("qml\\Main.qml") << false;
    QString controlOne = QStringLiteral("qml/control");
    controlOne.append(QChar(0x0001));
    controlOne.append(QStringLiteral("/Main.qml"));
    QTest::newRow("control-u0001-regression") << controlOne << false;
    QString controlUnitSeparator = QStringLiteral("qml/control");
    controlUnitSeparator.append(QChar(0x001f));
    controlUnitSeparator.append(QStringLiteral("/Main.qml"));
    QTest::newRow("control-u001f-regression") << controlUnitSeparator << false;
    QString c1Start = QStringLiteral("qml/control");
    c1Start.append(QChar(0x0080));
    c1Start.append(QStringLiteral("/Main.qml"));
    QTest::newRow("control-u0080") << c1Start << false;
    QString c1End = QStringLiteral("qml/control");
    c1End.append(QChar(0x009f));
    c1End.append(QStringLiteral("/Main.qml"));
    QTest::newRow("control-u009f") << c1End << false;
    const auto entryPointWithCodePoint = [](const char32_t codePoint) {
        return QStringLiteral("qml/") + QString::fromUcs4(&codePoint, 1)
            + QStringLiteral("/Main.qml");
    };
    QTest::newRow("noncharacter-u-fdd0") << entryPointWithCodePoint(0xfdd0) << false;
    QTest::newRow("noncharacter-u-fdef") << entryPointWithCodePoint(0xfdef) << false;
    QTest::newRow("noncharacter-u-fffe") << entryPointWithCodePoint(0xfffe) << false;
    QTest::newRow("noncharacter-u-ffff") << entryPointWithCodePoint(0xffff) << false;
    QTest::newRow("noncharacter-u-1fffe") << entryPointWithCodePoint(0x1fffe) << false;
    QTest::newRow("noncharacter-u-10ffff") << entryPointWithCodePoint(0x10ffff) << false;
    QTest::newRow("valid-supplementary-u-103fe") << entryPointWithCodePoint(0x103fe) << true;
    QTest::newRow("valid-supplementary-u-103ff") << entryPointWithCodePoint(0x103ff) << true;
    QTest::newRow("normal-supplementary-unicode") << entryPointWithCodePoint(0x1f600) << true;
    QTest::newRow("normal-line-separator") << entryPointWithCodePoint(0x2028) << true;
    QTest::newRow("directory-trailing-dot") << QStringLiteral("qml./Main.qml") << false;
    QTest::newRow("directory-trailing-space") << QStringLiteral("qml /Main.qml") << false;
    QTest::newRow("file-trailing-space") << QStringLiteral("qml/Main.qml ") << false;
    QTest::newRow("con-directory") << QStringLiteral("CON/Main.qml") << false;
    QTest::newRow("con-extension") << QStringLiteral("qml/con.qml") << false;
    QTest::newRow("prn-mixed-case") << QStringLiteral("qml/PrN.txt/Main.qml") << false;
    QTest::newRow("aux-extension") << QStringLiteral("qml/AUX.qml") << false;
    QTest::newRow("nul-multiple-extensions") << QStringLiteral("qml/NUL.any.qml") << false;
    QTest::newRow("clock-dollar") << QStringLiteral("qml/CLOCK$.qml") << false;
    QTest::newRow("com-one") << QStringLiteral("qml/com1.qml") << false;
    QTest::newRow("com-nine-directory") << QStringLiteral("qml/COM9/Main.qml") << false;
    QTest::newRow("com-superscript-one") << QStringLiteral("qml/COM¹.qml") << false;
    QTest::newRow("com-superscript-two-lowercase") << QStringLiteral("qml/com².txt.qml") << false;
    QTest::newRow("com-superscript-three-padded") << QStringLiteral("qml/Com³ .qml") << false;
    QTest::newRow("lpt-one") << QStringLiteral("qml/lpt1.qml") << false;
    QTest::newRow("lpt-nine") << QStringLiteral("qml/LPT9.qml") << false;
    QTest::newRow("lpt-superscript-one") << QStringLiteral("qml/LPT¹.qml") << false;
    QTest::newRow("lpt-superscript-two-lowercase") << QStringLiteral("qml/lpt².txt.qml") << false;
    QTest::newRow("lpt-superscript-three-padded") << QStringLiteral("qml/LpT³ .qml") << false;
    QTest::newRow("device-base-padded-before-extension")
        << QStringLiteral("qml/CON .qml") << false;
}

void ManifestTest::windowsEntryPointCorpusHasSchemaRuntimeParity()
{
    QFETCH(QString, path);
    QFETCH(bool, valid);

    const QJsonObject schema = manifestSchema();
    QVERIFY(!schema.isEmpty());
    const QString pattern = schema.value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("entryPoint"))
                                .toObject()
                                .value(QStringLiteral("pattern"))
                                .toString();
    const std::optional<QJsonArray> schemaMatches =
        ecmaScriptPatternMatches(pattern, QStringList{path});
    QVERIFY(schemaMatches.has_value());
    QCOMPARE(schemaMatches->size(), 2);
    QCOMPARE(schemaMatches->at(0).toBool(), valid);
    QCOMPARE(schemaMatches->at(1).toBool(), valid);

    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("entryPoint"), path));
    QCOMPARE(result.hasValue(), valid);
    if (!valid) {
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidEntryPoint);
        QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
    }
}

void ManifestTest::rejectsWorstCaseWindowsEntryPointComponent()
{
    const char32_t emoji = 0x1f600;
    QString path = QStringLiteral("qml/");
    for (qsizetype index = 0; index < 126; ++index) {
        path.append(QString::fromUcs4(&emoji, 1));
    }
    path.append(QStringLiteral(".qml"));

    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("entryPoint"), path));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::ResourceLimitExceeded);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
}

void ManifestTest::entryPointSchemaHandlesEcmaScriptLineSeparators()
{
    const QString pattern = manifestSchema()
                                .value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("entryPoint"))
                                .toObject()
                                .value(QStringLiteral("pattern"))
                                .toString();
    QVERIFY(!pattern.isEmpty());

    QString lineSeparatorAttack = QStringLiteral("qml/");
    lineSeparatorAttack.append(QChar(0x2028));
    lineSeparatorAttack.append(QStringLiteral(":Main.qml"));
    QString paragraphSeparatorAttack = QStringLiteral("qml/");
    paragraphSeparatorAttack.append(QChar(0x2029));
    paragraphSeparatorAttack.append(QStringLiteral("|Main.qml"));
    const QStringList paths = {QStringLiteral("qml/页面/报告.qml"),
                               QStringLiteral("qml/😀/Main.qml"),
                               lineSeparatorAttack,
                               paragraphSeparatorAttack};
    const std::optional<QJsonArray> matches = ecmaScriptPatternMatches(pattern, paths);
    QVERIFY(matches.has_value());
    QCOMPARE(*matches,
             QJsonArray({true, true, true, true, false, false, false, false}));

    for (qsizetype index = 0; index < paths.size(); ++index) {
        const ManifestParseResult result =
            Manifest::parse(withTopLevelValue(QStringLiteral("entryPoint"), paths.at(index)));
        QCOMPARE(result.hasValue(), index < 2);
        if (index >= 2) {
            QCOMPARE(result.errors().size(), 1);
            QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidEntryPoint);
            QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
        }
    }
}

void ManifestTest::rejectsUnsupportedAndDuplicateImports()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-imports.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 4);
    QCOMPARE(result.errors().at(0).code, ManifestErrorCode::DuplicateImport);
    QCOMPARE(result.errors().at(0).path, QStringLiteral("$.imports[1]"));
    QCOMPARE(result.errors().at(1).code, ManifestErrorCode::UnsupportedImport);
    QCOMPARE(result.errors().at(1).path, QStringLiteral("$.imports[2]"));
    QCOMPARE(result.errors().at(2).code, ManifestErrorCode::UnsupportedImport);
    QCOMPARE(result.errors().at(2).path, QStringLiteral("$.imports[3]"));
    QCOMPARE(result.errors().at(3).code, ManifestErrorCode::WrongType);
    QCOMPARE(result.errors().at(3).path, QStringLiteral("$.imports[4]"));
}

void ManifestTest::rejectsInvalidPermissionsDeterministically()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-permissions.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 6);
    const QVector<ManifestErrorCode> expectedCodes = {
        ManifestErrorCode::WrongType,
        ManifestErrorCode::InvalidStoragePermission,
        ManifestErrorCode::WrongType,
        ManifestErrorCode::InvalidClipboardReadPermission,
        ManifestErrorCode::InvalidFileOpenPermission,
        ManifestErrorCode::ProcessPermissionDenied};
    const QStringList expectedPaths = {QStringLiteral("$.permissions.network"),
                                       QStringLiteral("$.permissions.storage"),
                                       QStringLiteral("$.permissions.clipboardWrite"),
                                       QStringLiteral("$.permissions.clipboardRead"),
                                       QStringLiteral("$.permissions.fileOpen"),
                                       QStringLiteral("$.permissions.process")};
    for (qsizetype index = 0; index < result.errors().size(); ++index) {
        QCOMPARE(result.errors().at(index).code, expectedCodes.at(index));
        QCOMPARE(result.errors().at(index).path, expectedPaths.at(index));
    }
}

void ManifestTest::emptyPermissionsDefaultToDeny()
{
    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("permissions"), QJsonObject{}));

    QVERIFY(result.hasValue());
    QVERIFY(result.errors().isEmpty());
    QVERIFY(result.value().permissions().network.hosts.isEmpty());
    QVERIFY(result.value().permissions().network.methods.isEmpty());
    QCOMPARE(result.value().permissions().storage, StoragePermission::Disabled);
    QCOMPARE(result.value().permissions().clipboardWrite, false);
    QCOMPARE(result.value().permissions().clipboardRead, ClipboardReadPermission::Disabled);
    QCOMPARE(result.value().permissions().fileOpen, FileOpenPermission::Disabled);
}

void ManifestTest::rejectsNonCanonicalAndDuplicateNetworkHosts()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-network-hosts.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 10);
    for (qsizetype index = 0; index < 8; ++index) {
        QCOMPARE(result.errors().at(index).code, ManifestErrorCode::InvalidNetworkHost);
        QCOMPARE(result.errors().at(index).path,
                 QStringLiteral("$.permissions.network.hosts[%1]").arg(index));
    }
    QCOMPARE(result.errors().at(8).code, ManifestErrorCode::DuplicateNetworkHost);
    QCOMPARE(result.errors().at(8).path, QStringLiteral("$.permissions.network.hosts[9]"));
    QCOMPARE(result.errors().at(9).code, ManifestErrorCode::WrongType);
    QCOMPARE(result.errors().at(9).path, QStringLiteral("$.permissions.network.hosts[10]"));
}

void ManifestTest::canonicalHostCorpusHasSchemaRuntimeParity_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("valid");

    const QString label63 = QStringLiteral("a") + QString(62, u'b');
    const QString label64 = QStringLiteral("a") + QString(63, u'b');
    const QString label61 = QStringLiteral("a") + QString(60, u'b');
    const QString label62 = QStringLiteral("a") + QString(61, u'b');
    const QString total253 =
        label63 + u'.' + label63 + u'.' + label63 + u'.' + label61;
    const QString total254 =
        label63 + u'.' + label63 + u'.' + label63 + u'.' + label62;

    QTest::newRow("one-character-hostname") << QStringLiteral("a") << true;
    QTest::newRow("63-character-label") << (label63 + QStringLiteral(".com")) << true;
    QTest::newRow("253-character-hostname") << total253 << true;
    QTest::newRow("64-character-label") << (label64 + QStringLiteral(".com")) << false;
    QTest::newRow("254-character-hostname") << total254 << false;
    QTest::newRow("numeric-short-alias") << QStringLiteral("123") << false;
    QTest::newRow("numeric-long-alias") << QStringLiteral("2130706433") << false;
    QTest::newRow("canonical-ipv4") << QStringLiteral("127.0.0.1") << true;
    QTest::newRow("canonical-ipv4-zero") << QStringLiteral("0.0.0.0") << true;
    QTest::newRow("canonical-ipv4-maximum") << QStringLiteral("255.255.255.255") << true;
    QTest::newRow("noncanonical-ipv4") << QStringLiteral("127.00.0.1") << false;
    QTest::newRow("hex-dotted-alias") << QStringLiteral("0x7f.0.0.1") << false;
    QTest::newRow("hex-single-alias") << QStringLiteral("0x7f000001") << false;
    QTest::newRow("octal-dotted-alias") << QStringLiteral("0177.0.0.1") << false;
    QTest::newRow("octal-single-alias") << QStringLiteral("017700000001") << false;
    QTest::newRow("shortened-two-component") << QStringLiteral("127.1") << false;
    QTest::newRow("shortened-three-component") << QStringLiteral("127.0.1") << false;
    QTest::newRow("mixed-hex-shortened") << QStringLiteral("0x7f.1") << false;
    QTest::newRow("empty-label") << QStringLiteral("api..example.com") << false;
}

void ManifestTest::canonicalHostCorpusHasSchemaRuntimeParity()
{
    QFETCH(QString, host);
    QFETCH(bool, valid);

    const QJsonObject schema = manifestSchema();
    QVERIFY(!schema.isEmpty());
    const QString pattern = schema.value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("permissions"))
                                .toObject()
                                .value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("network"))
                                .toObject()
                                .value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("hosts"))
                                .toObject()
                                .value(QStringLiteral("items"))
                                .toObject()
                                .value(QStringLiteral("pattern"))
                                .toString();
    const QRegularExpression schemaPattern(pattern);
    QVERIFY(schemaPattern.isValid());
    QCOMPARE(schemaPattern.match(host).hasMatch(), valid);

    const ManifestParseResult result = Manifest::parse(withNetwork(
        {{QStringLiteral("hosts"), QJsonArray{host}},
         {QStringLiteral("methods"), QJsonArray{QStringLiteral("GET")}}}));
    QCOMPARE(result.hasValue(), valid);
    if (!valid) {
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidNetworkHost);
        QCOMPARE(result.errors().front().path,
                 QStringLiteral("$.permissions.network.hosts[0]"));
    }
}

void ManifestTest::rejectsUnsupportedAndDuplicateNetworkMethods()
{
    const ManifestParseResult unsupported = Manifest::parse(withNetwork(
        {{QStringLiteral("hosts"), QJsonArray{QStringLiteral("api.example.com")}},
         {QStringLiteral("methods"),
          QJsonArray{QStringLiteral("DELETE"),
                     QStringLiteral("get"),
                     QStringLiteral("CONNECT")}}}));
    QVERIFY(!unsupported.hasValue());
    QCOMPARE(unsupported.errors().size(), 3);
    for (qsizetype index = 0; index < 3; ++index) {
        QCOMPARE(unsupported.errors().at(index).code,
                 ManifestErrorCode::UnsupportedNetworkMethod);
        QCOMPARE(unsupported.errors().at(index).path,
                 QStringLiteral("$.permissions.network.methods[%1]").arg(index));
    }

    const ManifestParseResult duplicate = Manifest::parse(withNetwork(
        {{QStringLiteral("hosts"), QJsonArray{QStringLiteral("api.example.com")}},
         {QStringLiteral("methods"),
          QJsonArray{QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("GET")}}}));
    QVERIFY(!duplicate.hasValue());
    QCOMPARE(duplicate.errors().size(), 1);
    QCOMPARE(duplicate.errors().front().code, ManifestErrorCode::DuplicateNetworkMethod);
    QCOMPARE(duplicate.errors().front().path,
             QStringLiteral("$.permissions.network.methods[2]"));

    const ManifestParseResult wrongType = Manifest::parse(withNetwork(
        {{QStringLiteral("hosts"), QJsonArray{QStringLiteral("api.example.com")}},
         {QStringLiteral("methods"),
          QJsonArray{QStringLiteral("GET"), QStringLiteral("POST"), 7}}}));
    QVERIFY(!wrongType.hasValue());
    QCOMPARE(wrongType.errors().size(), 1);
    QCOMPARE(wrongType.errors().front().code, ManifestErrorCode::WrongType);
    QCOMPARE(wrongType.errors().front().path,
             QStringLiteral("$.permissions.network.methods[2]"));
}

void ManifestTest::rejectsMissingNetworkFields()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-network-shape.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 2);
    QCOMPARE(result.errors().at(0).code, ManifestErrorCode::MissingRequiredField);
    QCOMPARE(result.errors().at(0).path, QStringLiteral("$.permissions.network.hosts"));
    QCOMPARE(result.errors().at(1).code, ManifestErrorCode::MissingRequiredField);
    QCOMPARE(result.errors().at(1).path, QStringLiteral("$.permissions.network.methods"));
}

void ManifestTest::rejectsMalformedNetworkArrays_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    const QJsonArray hosts{QStringLiteral("api.example.com")};
    const QJsonArray methods{QStringLiteral("GET")};
    QTest::newRow("hosts-not-array")
        << withNetwork({{QStringLiteral("hosts"), QStringLiteral("api.example.com")},
                        {QStringLiteral("methods"), methods}})
        << ManifestErrorCode::WrongType << QStringLiteral("$.permissions.network.hosts");
    QTest::newRow("methods-not-array")
        << withNetwork({{QStringLiteral("hosts"), hosts},
                        {QStringLiteral("methods"), false}})
        << ManifestErrorCode::WrongType << QStringLiteral("$.permissions.network.methods");
    QTest::newRow("hosts-empty")
        << withNetwork({{QStringLiteral("hosts"), QJsonArray{}},
                        {QStringLiteral("methods"), methods}})
        << ManifestErrorCode::InvalidNetworkHost
        << QStringLiteral("$.permissions.network.hosts");
    QTest::newRow("methods-empty")
        << withNetwork({{QStringLiteral("hosts"), hosts},
                        {QStringLiteral("methods"), QJsonArray{}}})
        << ManifestErrorCode::UnsupportedNetworkMethod
        << QStringLiteral("$.permissions.network.methods");
}

void ManifestTest::rejectsMalformedNetworkArrays()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::rejectsOutOfRangeAndFloatingLimits()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-limits.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 3);
    const QStringList expectedPaths = {QStringLiteral("$.limits.packageBytes"),
                                       QStringLiteral("$.limits.memoryMiB"),
                                       QStringLiteral("$.limits.processes")};
    for (qsizetype index = 0; index < result.errors().size(); ++index) {
        QCOMPARE(result.errors().at(index).code, ManifestErrorCode::InvalidLimit);
        QCOMPARE(result.errors().at(index).path, expectedPaths.at(index));
    }
}

void ManifestTest::rejectsMalformedLimits_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<ManifestErrorCode>("code");
    QTest::addColumn<QString>("path");

    QTest::newRow("missing-package-bytes")
        << withLimits({{QStringLiteral("memoryMiB"), 64}, {QStringLiteral("processes"), 1}})
        << ManifestErrorCode::MissingRequiredField << QStringLiteral("$.limits.packageBytes");
    QTest::newRow("wrong-type")
        << withLimits({{QStringLiteral("packageBytes"), QStringLiteral("1024")},
                       {QStringLiteral("memoryMiB"), 64},
                       {QStringLiteral("processes"), 1}})
        << ManifestErrorCode::WrongType << QStringLiteral("$.limits.packageBytes");
    QTest::newRow("zero")
        << withLimits({{QStringLiteral("packageBytes"), 1024},
                       {QStringLiteral("memoryMiB"), 0},
                       {QStringLiteral("processes"), 1}})
        << ManifestErrorCode::InvalidLimit << QStringLiteral("$.limits.memoryMiB");
    QTest::newRow("negative")
        << withLimits({{QStringLiteral("packageBytes"), 1024},
                       {QStringLiteral("memoryMiB"), 64},
                       {QStringLiteral("processes"), -1}})
        << ManifestErrorCode::InvalidLimit << QStringLiteral("$.limits.processes");
    QTest::newRow("overflow")
        << withLimits({{QStringLiteral("packageBytes"), 1.0e100},
                       {QStringLiteral("memoryMiB"), 64},
                       {QStringLiteral("processes"), 1}})
        << ManifestErrorCode::InvalidLimit << QStringLiteral("$.limits.packageBytes");
}

void ManifestTest::rejectsMalformedLimits()
{
    QFETCH(QByteArray, bytes);
    QFETCH(ManifestErrorCode, code);
    QFETCH(QString, path);

    const ManifestParseResult result = Manifest::parse(bytes);

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, code);
    QCOMPARE(result.errors().front().path, path);
}

void ManifestTest::rejectsInvalidAndDuplicateRoutes()
{
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-routes.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 4);
    QCOMPARE(result.errors().at(0).code, ManifestErrorCode::DuplicateRoute);
    QCOMPARE(result.errors().at(0).path, QStringLiteral("$.routes[1]"));
    QCOMPARE(result.errors().at(1).code, ManifestErrorCode::InvalidRoute);
    QCOMPARE(result.errors().at(1).path, QStringLiteral("$.routes[2]"));
    QCOMPARE(result.errors().at(2).code, ManifestErrorCode::InvalidRoute);
    QCOMPARE(result.errors().at(2).path, QStringLiteral("$.routes[3]"));
    QCOMPARE(result.errors().at(3).code, ManifestErrorCode::WrongType);
    QCOMPARE(result.errors().at(3).path, QStringLiteral("$.routes[4]"));
}

void ManifestTest::rejectsEmptyRoutes()
{
    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("routes"), QJsonArray{}));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 1);
    QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidRoute);
    QCOMPARE(result.errors().front().path, QStringLiteral("$.routes"));
}

void ManifestTest::appIdBoundaryCorpusHasSchemaRuntimeParity_data()
{
    QTest::addColumn<QString>("appId");
    QTest::addColumn<bool>("valid");

    const QString label63 = QStringLiteral("a") + QString(62, u'b');
    const QString label64 = QStringLiteral("a") + QString(63, u'b');
    const QString label61 = QStringLiteral("a") + QString(60, u'b');
    const QString label62 = QStringLiteral("a") + QString(61, u'b');
    const QString total253 =
        label63 + u'.' + label63 + u'.' + label63 + u'.' + label61;
    const QString total254 =
        label63 + u'.' + label63 + u'.' + label63 + u'.' + label62;

    QTest::newRow("minimal") << QStringLiteral("a.b") << true;
    QTest::newRow("63-character-label") << (label63 + QStringLiteral(".b")) << true;
    QTest::newRow("253-characters") << total253 << true;
    QTest::newRow("64-character-label") << (label64 + QStringLiteral(".b")) << false;
    QTest::newRow("254-characters") << total254 << false;
    QTest::newRow("empty-label") << QStringLiteral("com..pilot") << false;
}

void ManifestTest::appIdBoundaryCorpusHasSchemaRuntimeParity()
{
    QFETCH(QString, appId);
    QFETCH(bool, valid);

    const QJsonObject schema = manifestSchema();
    QVERIFY(!schema.isEmpty());
    const QString pattern = schema.value(QStringLiteral("properties"))
                                .toObject()
                                .value(QStringLiteral("appId"))
                                .toObject()
                                .value(QStringLiteral("pattern"))
                                .toString();
    const QRegularExpression schemaPattern(pattern);
    QVERIFY(schemaPattern.isValid());
    QCOMPARE(schemaPattern.match(appId).hasMatch(), valid);

    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("appId"), appId));
    QCOMPARE(result.hasValue(), valid);
    if (!valid) {
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidAppId);
        QCOMPARE(result.errors().front().path, QStringLiteral("$.appId"));
    }
}

void ManifestTest::schemaDeclaresResourceBounds()
{
    const QJsonObject schema = manifestSchema();
    QVERIFY(!schema.isEmpty());
    QCOMPARE(schema.value(QStringLiteral("x-maxManifestBytes")).toInteger(),
             ManifestResourceLimits::MaxManifestBytes);

    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(properties.value(QStringLiteral("version"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInteger(),
             ManifestResourceLimits::MaxVersionCharacters);
    QCOMPARE(properties.value(QStringLiteral("entryPoint"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInteger(),
             ManifestResourceLimits::MaxEntryPointCharacters);
    QCOMPARE(properties.value(QStringLiteral("entryPoint"))
                 .toObject()
                 .value(QStringLiteral("x-maxWindowsComponentUtf16Units"))
                 .toInteger(),
             ManifestResourceLimits::MaxWindowsComponentUtf16Units);
    const QJsonObject runtimeProperties = properties.value(QStringLiteral("runtime"))
                                              .toObject()
                                              .value(QStringLiteral("properties"))
                                              .toObject();
    QCOMPARE(runtimeProperties.value(QStringLiteral("minVersion"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInteger(),
             ManifestResourceLimits::MaxVersionCharacters);
    QCOMPARE(runtimeProperties.value(QStringLiteral("maxVersion"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInteger(),
             ManifestResourceLimits::MaxVersionCharacters);
    QCOMPARE(properties.value(QStringLiteral("imports"))
                 .toObject()
                 .value(QStringLiteral("maxItems"))
                 .toInteger(),
             ManifestResourceLimits::MaxImports);

    const QJsonObject networkProperties = properties.value(QStringLiteral("permissions"))
                                              .toObject()
                                              .value(QStringLiteral("properties"))
                                              .toObject()
                                              .value(QStringLiteral("network"))
                                              .toObject()
                                              .value(QStringLiteral("properties"))
                                              .toObject();
    QCOMPARE(networkProperties.value(QStringLiteral("hosts"))
                 .toObject()
                 .value(QStringLiteral("maxItems"))
                 .toInteger(),
             ManifestResourceLimits::MaxNetworkHosts);
    QCOMPARE(networkProperties.value(QStringLiteral("methods"))
                 .toObject()
                 .value(QStringLiteral("maxItems"))
                 .toInteger(),
             ManifestResourceLimits::MaxNetworkMethods);

    const QJsonObject routes = properties.value(QStringLiteral("routes")).toObject();
    QCOMPARE(routes.value(QStringLiteral("maxItems")).toInteger(),
             ManifestResourceLimits::MaxRoutes);
    QCOMPARE(routes.value(QStringLiteral("items"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInteger(),
             ManifestResourceLimits::MaxRouteCharacters);
}

void ManifestTest::schemaMatchesRuntimeContract()
{
    QFile schemaFile(QStringLiteral(Q_BROWSER_MANIFEST_SCHEMA_FILE));
    QVERIFY2(schemaFile.open(QIODevice::ReadOnly), qPrintable(schemaFile.errorString()));
    QJsonParseError parseError;
    const QJsonDocument schemaDocument = QJsonDocument::fromJson(schemaFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(schemaDocument.isObject());

    const QJsonObject schema = schemaDocument.object();
    QCOMPARE(schema.value(QStringLiteral("$schema")).toString(),
             QStringLiteral("https://json-schema.org/draft/2020-12/schema"));
    QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(true), false);

    const QStringList expectedRequired = {QStringLiteral("schemaVersion"),
                                          QStringLiteral("appId"),
                                          QStringLiteral("version"),
                                          QStringLiteral("entryPoint"),
                                          QStringLiteral("runtime"),
                                          QStringLiteral("imports"),
                                          QStringLiteral("permissions"),
                                          QStringLiteral("limits"),
                                          QStringLiteral("routes")};
    QStringList schemaRequired;
    for (const QJsonValue value : schema.value(QStringLiteral("required")).toArray()) {
        schemaRequired.append(value.toString());
    }
    QCOMPARE(schemaRequired, expectedRequired);

    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(properties.value(QStringLiteral("schemaVersion"))
                 .toObject()
                 .value(QStringLiteral("const"))
                 .toInt(),
             1);
    QCOMPARE(properties.value(QStringLiteral("appId"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInt(),
             253);
    QCOMPARE(properties.value(QStringLiteral("entryPoint"))
                 .toObject()
                 .value(QStringLiteral("x-windowsPathInvariants"))
                 .toArray(),
             QJsonArray({QStringLiteral("segments do not end in dot or space"),
                         QStringLiteral(
                             "Win32 device basenames are forbidden case-insensitively")}));
    for (const QString &requiredKey : expectedRequired) {
        QJsonObject manifest = QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
        manifest.remove(requiredKey);
        const ManifestParseResult result =
            Manifest::parse(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::MissingRequiredField);
        QCOMPARE(result.errors().front().path, QStringLiteral("$.") + requiredKey);
    }

    const QJsonObject runtimeSchema =
        properties.value(QStringLiteral("runtime")).toObject();
    const QJsonObject permissionsSchema =
        properties.value(QStringLiteral("permissions")).toObject();
    const QJsonObject networkSchema = permissionsSchema.value(QStringLiteral("properties"))
                                          .toObject()
                                          .value(QStringLiteral("network"))
                                          .toObject();
    const QJsonObject limitsSchema = properties.value(QStringLiteral("limits")).toObject();
    QCOMPARE(runtimeSchema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    QCOMPARE(runtimeSchema.value(QStringLiteral("x-runtimeInvariants")).toArray(),
             QJsonArray({QStringLiteral("minVersion.major <= maxVersion.major")}));
    QCOMPARE(permissionsSchema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    QCOMPARE(networkSchema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    QCOMPARE(limitsSchema.value(QStringLiteral("additionalProperties")).toBool(true), false);

    const QStringList expectedRuntimeRequired = {QStringLiteral("minVersion"),
                                                 QStringLiteral("maxVersion")};
    QStringList runtimeRequired;
    for (const QJsonValue value : runtimeSchema.value(QStringLiteral("required")).toArray()) {
        runtimeRequired.append(value.toString());
    }
    QCOMPARE(runtimeRequired, expectedRuntimeRequired);
    for (const QString &requiredKey : expectedRuntimeRequired) {
        QJsonObject manifest =
            QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
        QJsonObject runtime = manifest.value(QStringLiteral("runtime")).toObject();
        runtime.remove(requiredKey);
        manifest.insert(QStringLiteral("runtime"), runtime);
        const ManifestParseResult result =
            Manifest::parse(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::MissingRequiredField);
        QCOMPARE(result.errors().front().path, QStringLiteral("$.runtime.") + requiredKey);
    }

    const QStringList expectedNetworkRequired = {QStringLiteral("hosts"),
                                                 QStringLiteral("methods")};
    QStringList networkRequired;
    for (const QJsonValue value : networkSchema.value(QStringLiteral("required")).toArray()) {
        networkRequired.append(value.toString());
    }
    QCOMPARE(networkRequired, expectedNetworkRequired);
    for (const QString &requiredKey : expectedNetworkRequired) {
        QJsonObject manifest =
            QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
        QJsonObject permissions = manifest.value(QStringLiteral("permissions")).toObject();
        QJsonObject network = permissions.value(QStringLiteral("network")).toObject();
        network.remove(requiredKey);
        permissions.insert(QStringLiteral("network"), network);
        manifest.insert(QStringLiteral("permissions"), permissions);
        const ManifestParseResult result =
            Manifest::parse(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::MissingRequiredField);
        QCOMPARE(result.errors().front().path,
                 QStringLiteral("$.permissions.network.") + requiredKey);
    }

    const QJsonObject importItems = properties.value(QStringLiteral("imports"))
                                        .toObject()
                                        .value(QStringLiteral("items"))
                                        .toObject();
    QStringList schemaImports;
    for (const QJsonValue value : importItems.value(QStringLiteral("enum")).toArray()) {
        schemaImports.append(value.toString());
    }
    QCOMPARE(schemaImports,
             QStringList({QStringLiteral("QtQuick"),
                          QStringLiteral("QtQuick.Controls"),
                          QStringLiteral("QtQuick.Layouts"),
                          QStringLiteral("Company.Design"),
                          QStringLiteral("Company.Runtime")}));

    const QJsonObject permissionProperties =
        permissionsSchema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(permissionProperties.value(QStringLiteral("process"))
                 .toObject()
                 .value(QStringLiteral("const"))
                 .toBool(true),
             false);
    const QJsonObject methodItems = networkSchema.value(QStringLiteral("properties"))
                                        .toObject()
                                        .value(QStringLiteral("methods"))
                                        .toObject()
                                        .value(QStringLiteral("items"))
                                        .toObject();
    QStringList schemaMethods;
    for (const QJsonValue value : methodItems.value(QStringLiteral("enum")).toArray()) {
        schemaMethods.append(value.toString());
    }
    QCOMPARE(schemaMethods,
             QStringList({QStringLiteral("GET"),
                          QStringLiteral("POST"),
                          QStringLiteral("PUT")}));
    QCOMPARE(networkSchema.value(QStringLiteral("properties"))
                 .toObject()
                 .value(QStringLiteral("hosts"))
                 .toObject()
                 .value(QStringLiteral("items"))
                 .toObject()
                 .value(QStringLiteral("maxLength"))
                 .toInt(),
             253);

    const QStringList expectedLimitKeys = {QStringLiteral("packageBytes"),
                                           QStringLiteral("memoryMiB"),
                                           QStringLiteral("processes")};
    QStringList requiredLimitKeys;
    for (const QJsonValue value : limitsSchema.value(QStringLiteral("required")).toArray()) {
        requiredLimitKeys.append(value.toString());
    }
    QCOMPARE(requiredLimitKeys, expectedLimitKeys);

    const QVector<QPair<QString, qint64>> maxima = {
        {QStringLiteral("packageBytes"), 50LL * 1024LL * 1024LL},
        {QStringLiteral("memoryMiB"), 384},
        {QStringLiteral("processes"), 1},
    };
    const QJsonObject limitProperties = limitsSchema.value(QStringLiteral("properties")).toObject();
    for (const auto &[key, maximum] : maxima) {
        QCOMPARE(limitProperties.value(key).toObject().value(QStringLiteral("maximum")).toInteger(),
                 maximum);

        QJsonObject validManifest =
            QJsonDocument::fromJson(fixture(QStringLiteral("valid.json"))).object();
        QJsonObject limits = validManifest.value(QStringLiteral("limits")).toObject();
        limits.insert(key, maximum);
        validManifest.insert(QStringLiteral("limits"), limits);
        QVERIFY(Manifest::parse(QJsonDocument(validManifest).toJson(QJsonDocument::Compact))
                    .hasValue());

        limits.insert(key, maximum + 1);
        validManifest.insert(QStringLiteral("limits"), limits);
        const ManifestParseResult invalid =
            Manifest::parse(QJsonDocument(validManifest).toJson(QJsonDocument::Compact));
        QVERIFY(!invalid.hasValue());
        QCOMPARE(invalid.errors().size(), 1);
        QCOMPARE(invalid.errors().front().code, ManifestErrorCode::InvalidLimit);
        QCOMPARE(invalid.errors().front().path, QStringLiteral("$.limits.") + key);
    }
}

QTEST_MAIN(ManifestTest)

#include "tst_manifest.moc"
