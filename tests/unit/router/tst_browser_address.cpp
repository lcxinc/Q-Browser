#include "BrowserAddress.h"

#include <QByteArray>
#include <QTest>

namespace {

void verifyInvalid(const BrowserAddress &address, const BrowserAddressError expectedError)
{
    QVERIFY(!address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::Invalid);
    QCOMPARE(address.error(), expectedError);
    QVERIFY(address.canonical().isEmpty());
    QVERIFY(address.appPath().isEmpty());
    QVERIFY(address.appQuery().isEmpty());
}

} // namespace

class BrowserAddressTest final : public QObject
{
    Q_OBJECT

private slots:
    void acceptsExactNewTab();
    void parsesCanonicalAppAddress();
    void acceptsConfiguredAppAuthority();
    void preservesSyntacticallyValidUnregisteredAppPath();
    void acceptsCanonicalQueries_data();
    void acceptsCanonicalQueries();
    void rejectsNonExactHostAddress_data();
    void rejectsNonExactHostAddress();
    void rejectsUnsupportedScheme_data();
    void rejectsUnsupportedScheme();
    void rejectsWrongAppAuthority_data();
    void rejectsWrongAppAuthority();
    void rejectsCaseAndPercentEncodingAmbiguity_data();
    void rejectsCaseAndPercentEncodingAmbiguity();
    void rejectsUnsafeOrNonCanonicalQuery_data();
    void rejectsUnsafeOrNonCanonicalQuery();
    void rejectsInvalidApplicationPath_data();
    void rejectsInvalidApplicationPath();
    void enforcesUtf8ByteLimit();
};

void BrowserAddressTest::acceptsExactNewTab()
{
    const auto address = BrowserAddress::parse(QStringLiteral("qbrowser://newtab"));

    QVERIFY(address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::NewTab);
    QCOMPARE(address.error(), BrowserAddressError::None);
    QCOMPARE(address.canonical(), QStringLiteral("qbrowser://newtab"));
    QVERIFY(address.appPath().isEmpty());
    QVERIFY(address.appQuery().isEmpty());
}

void BrowserAddressTest::parsesCanonicalAppAddress()
{
    const QString input = QStringLiteral(
        "app://pilot/orders/%E2%9C%93?tab=history&return=%2Forders%3Fstate%3Dopen");

    const auto address = BrowserAddress::parse(input);

    QVERIFY(address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::App);
    QCOMPARE(address.error(), BrowserAddressError::None);
    QCOMPARE(address.canonical(), input);
    QCOMPARE(address.appPath(), QStringLiteral("/orders/%E2%9C%93"));
    QCOMPARE(address.appQuery(), QStringLiteral("tab=history&return=%2Forders%3Fstate%3Dopen"));
}

void BrowserAddressTest::acceptsConfiguredAppAuthority()
{
    const QString input = QStringLiteral("app://console.internal/dashboard?view=summary");

    const auto address = BrowserAddress::parse(input, u"console.internal");

    QVERIFY(address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::App);
    QCOMPARE(address.error(), BrowserAddressError::None);
    QCOMPARE(address.canonical(), input);
    QCOMPARE(address.appPath(), QStringLiteral("/dashboard"));
    QCOMPARE(address.appQuery(), QStringLiteral("view=summary"));
}

void BrowserAddressTest::preservesSyntacticallyValidUnregisteredAppPath()
{
    const QString input = QStringLiteral("app://pilot/not-registered/42?mode=preview");

    const auto address = BrowserAddress::parse(input);

    QVERIFY(address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::App);
    QCOMPARE(address.canonical(), input);
    QCOMPARE(address.appPath(), QStringLiteral("/not-registered/42"));
    QCOMPARE(address.appQuery(), QStringLiteral("mode=preview"));
}

void BrowserAddressTest::acceptsCanonicalQueries_data()
{
    QTest::addColumn<QString>("query");

    QTest::newRow("unreserved-and-query-delimiters")
        << QStringLiteral("AZaz09-._~!$&'()*+,;=:@/?");
    QTest::newRow("encoded-reserved")
        << QStringLiteral("next=%2Forders%3Fstate%3Dopen%26sort%3Dnew%23top%5B0%5D");
    QTest::newRow("encoded-percent") << QStringLiteral("value=100%25");
    QTest::newRow("encoded-unicode") << QStringLiteral("check=%E2%9C%93");
}

void BrowserAddressTest::acceptsCanonicalQueries()
{
    QFETCH(QString, query);
    const QString input = QStringLiteral("app://pilot/orders?") + query;

    const auto address = BrowserAddress::parse(input);

    QVERIFY(address.isValid());
    QCOMPARE(address.kind(), BrowserAddressKind::App);
    QCOMPARE(address.error(), BrowserAddressError::None);
    QCOMPARE(address.canonical(), input);
    QCOMPARE(address.appPath(), QStringLiteral("/orders"));
    QCOMPARE(address.appQuery(), query);
}

void BrowserAddressTest::rejectsNonExactHostAddress_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("trailing-slash") << QStringLiteral("qbrowser://newtab/");
    QTest::newRow("query") << QStringLiteral("qbrowser://newtab?mode=compact");
    QTest::newRow("empty-query") << QStringLiteral("qbrowser://newtab?");
    QTest::newRow("fragment") << QStringLiteral("qbrowser://newtab#top");
    QTest::newRow("userinfo") << QStringLiteral("qbrowser://user@newtab");
    QTest::newRow("port") << QStringLiteral("qbrowser://newtab:443");
    QTest::newRow("encoded-authority") << QStringLiteral("qbrowser://%6Eewtab");
    QTest::newRow("encoded-path") << QStringLiteral("qbrowser://newtab/%70age");
    QTest::newRow("unknown-page") << QStringLiteral("qbrowser://settings");
}

void BrowserAddressTest::rejectsNonExactHostAddress()
{
    QFETCH(QString, input);

    verifyInvalid(BrowserAddress::parse(input), BrowserAddressError::UnknownHostPage);
}

void BrowserAddressTest::rejectsUnsupportedScheme_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("http") << QStringLiteral("http://example.test/");
    QTest::newRow("https") << QStringLiteral("https://example.test/");
    QTest::newRow("file") << QStringLiteral("file:///C:/secret.txt");
    QTest::newRow("data") << QStringLiteral("data:text/plain,hello");
    QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)");
    QTest::newRow("unknown") << QStringLiteral("custom://pilot/orders");
}

void BrowserAddressTest::rejectsUnsupportedScheme()
{
    QFETCH(QString, input);

    verifyInvalid(BrowserAddress::parse(input), BrowserAddressError::WrongScheme);
}

void BrowserAddressTest::rejectsWrongAppAuthority_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("authority");

    QTest::newRow("wrong") << QStringLiteral("app://other/orders") << QStringLiteral("pilot");
    QTest::newRow("userinfo") << QStringLiteral("app://user@pilot/orders")
                                << QStringLiteral("pilot");
    QTest::newRow("port") << QStringLiteral("app://pilot:443/orders")
                            << QStringLiteral("pilot");
    QTest::newRow("encoded") << QStringLiteral("app://%70ilot/orders")
                               << QStringLiteral("pilot");
    QTest::newRow("configured-mismatch") << QStringLiteral("app://pilot/orders")
                                           << QStringLiteral("console");
    QTest::newRow("invalid-configured-authority") << QStringLiteral("app://pilot/orders")
                                                    << QStringLiteral("Pilot");
}

void BrowserAddressTest::rejectsWrongAppAuthority()
{
    QFETCH(QString, input);
    QFETCH(QString, authority);

    verifyInvalid(BrowserAddress::parse(input, authority), BrowserAddressError::WrongAuthority);
}

void BrowserAddressTest::rejectsCaseAndPercentEncodingAmbiguity_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<BrowserAddressError>("expectedError");

    QTest::newRow("host-scheme-case")
        << QStringLiteral("QBrowser://newtab") << BrowserAddressError::WrongScheme;
    QTest::newRow("host-page-case")
        << QStringLiteral("qbrowser://NewTab") << BrowserAddressError::UnknownHostPage;
    QTest::newRow("app-scheme-case")
        << QStringLiteral("APP://pilot/orders") << BrowserAddressError::WrongScheme;
    QTest::newRow("app-authority-case")
        << QStringLiteral("app://Pilot/orders") << BrowserAddressError::WrongAuthority;
    QTest::newRow("path-lowercase-escape")
        << QStringLiteral("app://pilot/orders/%e2%9c%93") << BrowserAddressError::AppUrlInvalid;
    QTest::newRow("path-encoded-unreserved")
        << QStringLiteral("app://pilot/%6Frders") << BrowserAddressError::AppUrlInvalid;
}

void BrowserAddressTest::rejectsCaseAndPercentEncodingAmbiguity()
{
    QFETCH(QString, input);
    QFETCH(BrowserAddressError, expectedError);

    verifyInvalid(BrowserAddress::parse(input), expectedError);
}

void BrowserAddressTest::rejectsUnsafeOrNonCanonicalQuery_data()
{
    QTest::addColumn<QString>("query");

    QTest::newRow("empty") << QString();
    QTest::newRow("raw-unicode") << QString::fromUtf8("q=✓");
    QTest::newRow("raw-space") << QStringLiteral("q=hello world");
    QTest::newRow("raw-tab") << QStringLiteral("q=hello\tworld");
    QTest::newRow("raw-newline") << QStringLiteral("q=hello\nworld");
    QTest::newRow("raw-quote") << QStringLiteral("q=\"quoted\"");
    QTest::newRow("raw-backslash") << QStringLiteral("q=one\\two");
    QTest::newRow("malformed-escape") << QStringLiteral("q=%");
    QTest::newRow("lowercase-escape") << QStringLiteral("q=%2forders");
    QTest::newRow("encoded-unreserved") << QStringLiteral("q=%41");
    QTest::newRow("encoded-backtick") << QStringLiteral("q=%60");
    QTest::newRow("encoded-brace") << QStringLiteral("q=%7B");
    QTest::newRow("encoded-space") << QStringLiteral("q=%20");
    QTest::newRow("encoded-control") << QStringLiteral("q=%1F");
    QTest::newRow("encoded-quote") << QStringLiteral("q=%22quoted%22");
    QTest::newRow("encoded-backslash") << QStringLiteral("q=%5C");
    QTest::newRow("invalid-utf8") << QStringLiteral("q=%FF");
    QTest::newRow("overlong-utf8") << QStringLiteral("q=%C0%AF");
    QTest::newRow("utf8-surrogate") << QStringLiteral("q=%ED%A0%80");
    QTest::newRow("utf8-out-of-range") << QStringLiteral("q=%F4%90%80%80");
    QTest::newRow("unicode-control") << QStringLiteral("q=%C2%80");
    QTest::newRow("bidi-alm") << QStringLiteral("q=%D8%9C");
    QTest::newRow("bidi-lrm") << QStringLiteral("q=%E2%80%8E");
    QTest::newRow("bidi-override") << QStringLiteral("q=%E2%80%AE");
    QTest::newRow("bidi-isolate") << QStringLiteral("q=%E2%81%A6");
}

void BrowserAddressTest::rejectsUnsafeOrNonCanonicalQuery()
{
    QFETCH(QString, query);
    const QString input = QStringLiteral("app://pilot/orders?") + query;

    verifyInvalid(BrowserAddress::parse(input), BrowserAddressError::AppUrlInvalid);
}

void BrowserAddressTest::rejectsInvalidApplicationPath_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("missing-path") << QStringLiteral("app://pilot");
    QTest::newRow("trailing-slash") << QStringLiteral("app://pilot/orders/");
    QTest::newRow("duplicate-slash") << QStringLiteral("app://pilot/orders//42");
    QTest::newRow("traversal") << QStringLiteral("app://pilot/orders/../secret");
    QTest::newRow("encoded-separator") << QStringLiteral("app://pilot/orders%2Fsecret");
    QTest::newRow("invalid-utf8") << QStringLiteral("app://pilot/orders/%FF");
    QTest::newRow("fragment") << QStringLiteral("app://pilot/orders#history");
}

void BrowserAddressTest::rejectsInvalidApplicationPath()
{
    QFETCH(QString, input);

    verifyInvalid(BrowserAddress::parse(input), BrowserAddressError::AppUrlInvalid);
}

void BrowserAddressTest::enforcesUtf8ByteLimit()
{
    const QString prefix = QStringLiteral("app://pilot/");
    const QString atLimit = prefix + QString(2048 - prefix.toUtf8().size(), u'a');
    const QString overLimit = atLimit + u'a';
    QCOMPARE(atLimit.toUtf8().size(), 2048);
    QCOMPARE(overLimit.toUtf8().size(), 2049);

    const auto accepted = BrowserAddress::parse(atLimit);
    QVERIFY(accepted.isValid());
    QCOMPARE(accepted.kind(), BrowserAddressKind::App);
    QCOMPARE(accepted.error(), BrowserAddressError::None);
    QCOMPARE(accepted.canonical(), atLimit);

    verifyInvalid(BrowserAddress::parse(overLimit), BrowserAddressError::TooLong);

    const QString multibyteOverLimit = QStringLiteral("app://pilot/orders?q=")
        + QString(1014, QChar(0x00e9));
    QVERIFY(multibyteOverLimit.size() < 2048);
    QVERIFY(multibyteOverLimit.toUtf8().size() > 2048);
    verifyInvalid(BrowserAddress::parse(multibyteOverLimit), BrowserAddressError::TooLong);
}

QTEST_MAIN(BrowserAddressTest)

#include "tst_browser_address.moc"
