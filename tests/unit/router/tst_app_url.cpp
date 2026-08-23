#include "AppUrl.h"

#include <QTest>

class AppUrlTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesNormalizedApplicationUrl();
    void acceptsConfiguredAuthority();
    void rejectsAuthorityMismatch();
    void rejectsInvalidExpectedAuthority_data();
    void rejectsInvalidExpectedAuthority();
    void rejectsWrongScheme();
    void rejectsMissingOrWrongAuthority_data();
    void rejectsMissingOrWrongAuthority();
    void rejectsMalformedPercentEncoding_data();
    void rejectsMalformedPercentEncoding();
    void rejectsTraversalSegments_data();
    void rejectsTraversalSegments();
    void rejectsDuplicateSlashes();
    void rejectsFragments();
    void rejectsNonAbsoluteOrUnnormalizedPath_data();
    void rejectsNonAbsoluteOrUnnormalizedPath();
    void rejectsUnsafeEncodedPath_data();
    void rejectsUnsafeEncodedPath();
    void queryTextCannotChangeApplicationPath();
};

void AppUrlTest::parsesNormalizedApplicationUrl()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders/42?tab=history"),
                                   QStringLiteral("pilot"));

    QVERIFY(url.isValid());
    QCOMPARE(url.error(), AppUrlError::None);
    QCOMPARE(url.path(), QStringLiteral("/orders/42"));
    QCOMPARE(url.query(), QStringLiteral("tab=history"));
}

void AppUrlTest::acceptsConfiguredAuthority()
{
    const auto url = AppUrl::parse(QStringLiteral("app://console/orders"),
                                   QStringLiteral("console"));

    QVERIFY(url.isValid());
    QCOMPARE(url.path(), QStringLiteral("/orders"));
}

void AppUrlTest::rejectsAuthorityMismatch()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders"),
                                   QStringLiteral("console"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::WrongAuthority);
}

void AppUrlTest::rejectsInvalidExpectedAuthority_data()
{
    QTest::addColumn<QString>("authority");

    QTest::newRow("empty") << QString();
    QTest::newRow("uppercase") << QStringLiteral("Pilot");
    QTest::newRow("wildcard") << QStringLiteral("*.example");
    QTest::newRow("port") << QStringLiteral("pilot:443");
    QTest::newRow("userinfo") << QStringLiteral("user@pilot");
    QTest::newRow("leading-hyphen") << QStringLiteral("-pilot");
    QTest::newRow("trailing-hyphen") << QStringLiteral("pilot-");
    QTest::newRow("empty-label") << QStringLiteral("pilot..internal");
    QTest::newRow("underscore") << QStringLiteral("pilot_internal");
}

void AppUrlTest::rejectsInvalidExpectedAuthority()
{
    QFETCH(QString, authority);

    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders"), authority);

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::InvalidExpectedAuthority);
}

void AppUrlTest::rejectsWrongScheme()
{
    const auto url = AppUrl::parse(QStringLiteral("https://pilot/orders"),
                                   QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::WrongScheme);
}

void AppUrlTest::rejectsMissingOrWrongAuthority_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("missing") << QStringLiteral("app:/orders");
    QTest::newRow("wrong") << QStringLiteral("app://example/orders");
    QTest::newRow("userinfo") << QStringLiteral("app://user@pilot/orders");
    QTest::newRow("port") << QStringLiteral("app://pilot:443/orders");
}

void AppUrlTest::rejectsMissingOrWrongAuthority()
{
    QFETCH(QString, input);

    const auto url = AppUrl::parse(input, QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::WrongAuthority);
}

void AppUrlTest::rejectsMalformedPercentEncoding_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("truncated") << QStringLiteral("app://pilot/orders/%2");
    QTest::newRow("non-hex") << QStringLiteral("app://pilot/orders/%GG");
    QTest::newRow("query") << QStringLiteral("app://pilot/orders?value=%0X");
}

void AppUrlTest::rejectsMalformedPercentEncoding()
{
    QFETCH(QString, input);

    const auto url = AppUrl::parse(input, QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::MalformedPercentEncoding);
}

void AppUrlTest::rejectsTraversalSegments_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("dot") << QStringLiteral("app://pilot/orders/./settings");
    QTest::newRow("dot-dot") << QStringLiteral("app://pilot/orders/../settings");
    QTest::newRow("encoded-dot") << QStringLiteral("app://pilot/orders/%2E/settings");
    QTest::newRow("encoded-dot-dot") << QStringLiteral("app://pilot/orders/%2E%2E/settings");
    QTest::newRow("mixed-dot-dot") << QStringLiteral("app://pilot/orders/.%2E/settings");
}

void AppUrlTest::rejectsTraversalSegments()
{
    QFETCH(QString, input);

    const auto url = AppUrl::parse(input, QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::PathTraversal);
}

void AppUrlTest::rejectsDuplicateSlashes()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders//42"),
                                   QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::DuplicateSlash);
}

void AppUrlTest::rejectsFragments()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders#history"),
                                   QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::FragmentNotAllowed);
}

void AppUrlTest::rejectsNonAbsoluteOrUnnormalizedPath_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<AppUrlError>("expectedError");

    QTest::newRow("empty") << QStringLiteral("app://pilot") << AppUrlError::PathNotAbsolute;
    QTest::newRow("encoded-unreserved")
        << QStringLiteral("app://pilot/orders/%34%32") << AppUrlError::NonNormalizedPath;
    QTest::newRow("lowercase-escape")
        << QStringLiteral("app://pilot/orders/%e2%9c%93") << AppUrlError::NonNormalizedPath;
    QTest::newRow("backslash")
        << QStringLiteral("app://pilot/orders\\42") << AppUrlError::NonNormalizedPath;
}

void AppUrlTest::rejectsNonAbsoluteOrUnnormalizedPath()
{
    QFETCH(QString, input);
    QFETCH(AppUrlError, expectedError);

    const auto url = AppUrl::parse(input, QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), expectedError);
}

void AppUrlTest::rejectsUnsafeEncodedPath_data()
{
    QTest::addColumn<QString>("path");

    QTest::newRow("invalid-utf8") << QStringLiteral("/orders/%FF");
    QTest::newRow("truncated-2-byte-utf8") << QStringLiteral("/orders/%C2");
    QTest::newRow("truncated-3-byte-utf8") << QStringLiteral("/orders/%E2%82");
    QTest::newRow("truncated-4-byte-utf8") << QStringLiteral("/orders/%F0%9F%98");
    QTest::newRow("decoded-forward-slash") << QStringLiteral("/orders/acme%2Fadmin");
    QTest::newRow("decoded-backslash") << QStringLiteral("/orders/acme%5Cadmin");
    QTest::newRow("encoded-nul") << QStringLiteral("/orders/%00");
    QTest::newRow("encoded-control") << QStringLiteral("/orders/%1F");
    QTest::newRow("encoded-unicode-control") << QStringLiteral("/orders/%C2%80");
    QTest::newRow("illegal-literal") << QStringLiteral("/orders/[admin]");
    QTest::newRow("trailing-slash") << QStringLiteral("/orders/");

    QString literalControl = QStringLiteral("/orders/");
    literalControl.append(QChar(0x1f));
    QTest::newRow("literal-control") << literalControl;
}

void AppUrlTest::rejectsUnsafeEncodedPath()
{
    QFETCH(QString, path);

    const auto url = AppUrl::parse(QStringLiteral("app://pilot") + path,
                                   QStringLiteral("pilot"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::NonNormalizedPath);
}

void AppUrlTest::queryTextCannotChangeApplicationPath()
{
    const auto url = AppUrl::parse(
        QStringLiteral("app://pilot/orders?file=..%2Fprivate%2Fsecret.txt"),
        QStringLiteral("pilot"));

    QVERIFY(url.isValid());
    QCOMPARE(url.path(), QStringLiteral("/orders"));
    QCOMPARE(url.query(), QStringLiteral("file=..%2Fprivate%2Fsecret.txt"));
}

QTEST_MAIN(AppUrlTest)

#include "tst_app_url.moc"
