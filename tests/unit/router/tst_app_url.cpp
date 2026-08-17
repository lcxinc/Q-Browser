#include "AppUrl.h"

#include <QTest>

class AppUrlTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesNormalizedApplicationUrl();
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
    void queryTextCannotChangeApplicationPath();
};

void AppUrlTest::parsesNormalizedApplicationUrl()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders/42?tab=history"));

    QVERIFY(url.isValid());
    QCOMPARE(url.error(), AppUrlError::None);
    QCOMPARE(url.path(), QStringLiteral("/orders/42"));
    QCOMPARE(url.query(), QStringLiteral("tab=history"));
}

void AppUrlTest::rejectsWrongScheme()
{
    const auto url = AppUrl::parse(QStringLiteral("https://pilot/orders"));

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

    const auto url = AppUrl::parse(input);

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

    const auto url = AppUrl::parse(input);

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

    const auto url = AppUrl::parse(input);

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::PathTraversal);
}

void AppUrlTest::rejectsDuplicateSlashes()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders//42"));

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), AppUrlError::DuplicateSlash);
}

void AppUrlTest::rejectsFragments()
{
    const auto url = AppUrl::parse(QStringLiteral("app://pilot/orders#history"));

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

    const auto url = AppUrl::parse(input);

    QVERIFY(!url.isValid());
    QCOMPARE(url.error(), expectedError);
}

void AppUrlTest::queryTextCannotChangeApplicationPath()
{
    const auto url = AppUrl::parse(
        QStringLiteral("app://pilot/orders?file=..%2Fprivate%2Fsecret.txt"));

    QVERIFY(url.isValid());
    QCOMPARE(url.path(), QStringLiteral("/orders"));
    QCOMPARE(url.query(), QStringLiteral("file=..%2Fprivate%2Fsecret.txt"));
}

QTEST_MAIN(AppUrlTest)

#include "tst_app_url.moc"
