#include "Manifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

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

QJsonObject manifestSchema()
{
    QFile file(QStringLiteral(Q_BROWSER_MANIFEST_SCHEMA_FILE));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
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
    void rejectsNonObjectRoot();
    void rejectsMissingRequiredField();
    void rejectsUnsupportedSchemaVersion();
    void rejectsNonIntegerSchemaVersion();
    void rejectsWrongTopLevelTypesDeterministically();
    void rejectsUnknownFieldsDeterministically();
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
    QTest::newRow("device-prefix-is-normal") << QStringLiteral("qml/concept/Main.qml") << true;
    QTest::newRow("com-zero-is-normal") << QStringLiteral("qml/COM0.qml") << true;
    QTest::newRow("com-ten-is-normal") << QStringLiteral("qml/COM10.qml") << true;
    QTest::newRow("lpt-ten-is-normal") << QStringLiteral("qml/LPT10.qml") << true;
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
    QTest::newRow("lpt-one") << QStringLiteral("qml/lpt1.qml") << false;
    QTest::newRow("lpt-nine") << QStringLiteral("qml/LPT9.qml") << false;
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
    const QRegularExpression schemaPattern(pattern);
    QVERIFY2(schemaPattern.isValid(), qPrintable(schemaPattern.errorString()));
    QCOMPARE(schemaPattern.match(path).hasMatch(), valid);

    const ManifestParseResult result =
        Manifest::parse(withTopLevelValue(QStringLiteral("entryPoint"), path));
    QCOMPARE(result.hasValue(), valid);
    if (!valid) {
        QCOMPARE(result.errors().size(), 1);
        QCOMPARE(result.errors().front().code, ManifestErrorCode::InvalidEntryPoint);
        QCOMPARE(result.errors().front().path, QStringLiteral("$.entryPoint"));
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
    QTest::newRow("noncanonical-ipv4") << QStringLiteral("127.00.0.1") << false;
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
    const ManifestParseResult result =
        Manifest::parse(fixture(QStringLiteral("invalid-network-methods.json")));

    QVERIFY(!result.hasValue());
    QCOMPARE(result.errors().size(), 5);
    for (qsizetype index = 0; index < 3; ++index) {
        QCOMPARE(result.errors().at(index).code, ManifestErrorCode::UnsupportedNetworkMethod);
        QCOMPARE(result.errors().at(index).path,
                 QStringLiteral("$.permissions.network.methods[%1]").arg(index + 1));
    }
    QCOMPARE(result.errors().at(3).code, ManifestErrorCode::DuplicateNetworkMethod);
    QCOMPARE(result.errors().at(3).path, QStringLiteral("$.permissions.network.methods[4]"));
    QCOMPARE(result.errors().at(4).code, ManifestErrorCode::WrongType);
    QCOMPARE(result.errors().at(4).path, QStringLiteral("$.permissions.network.methods[5]"));
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
