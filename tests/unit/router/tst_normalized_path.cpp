#include "NormalizedPath.h"

#include <QTest>

class NormalizedPathTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesEncodedPathAndDecodesSegments();
    void acceptsRootPath();
    void rejectsUnsafeOrNonCanonicalPath_data();
    void rejectsUnsafeOrNonCanonicalPath();
};

void NormalizedPathTest::preservesEncodedPathAndDecodesSegments()
{
    const auto path = NormalizedPath::parse(QStringLiteral("/customers/alice%20smith/%E2%9C%93"));

    QVERIFY(path.isValid());
    QCOMPARE(path.error(), NormalizedPathError::None);
    QCOMPARE(path.encoded(), QStringLiteral("/customers/alice%20smith/%E2%9C%93"));
    QCOMPARE(path.decodedSegments(),
             QStringList({QStringLiteral("customers"),
                          QStringLiteral("alice smith"),
                          QString::fromUtf8("✓")}));
}

void NormalizedPathTest::acceptsRootPath()
{
    const auto path = NormalizedPath::parse(QStringLiteral("/"));

    QVERIFY(path.isValid());
    QCOMPARE(path.encoded(), QStringLiteral("/"));
    QVERIFY(path.decodedSegments().isEmpty());
}

void NormalizedPathTest::rejectsUnsafeOrNonCanonicalPath_data()
{
    QTest::addColumn<QString>("encoded");
    QTest::addColumn<NormalizedPathError>("expectedError");

    QTest::newRow("empty") << QString() << NormalizedPathError::NotAbsolute;
    QTest::newRow("relative") << QStringLiteral("orders/42")
                               << NormalizedPathError::NotAbsolute;
    QTest::newRow("duplicate-slash") << QStringLiteral("/orders//42")
                                      << NormalizedPathError::DuplicateSlash;
    QTest::newRow("trailing-slash") << QStringLiteral("/orders/")
                                     << NormalizedPathError::TrailingSlash;
    QTest::newRow("malformed-percent") << QStringLiteral("/orders/%2")
                                        << NormalizedPathError::MalformedPercentEncoding;
    QTest::newRow("lowercase-percent") << QStringLiteral("/orders/%e2%9c%93")
                                        << NormalizedPathError::NonCanonicalEncoding;
    QTest::newRow("encoded-unreserved") << QStringLiteral("/orders/%34%32")
                                         << NormalizedPathError::NonCanonicalEncoding;
    QTest::newRow("invalid-utf8") << QStringLiteral("/orders/%FF")
                                   << NormalizedPathError::InvalidUtf8;
    QTest::newRow("decoded-forward-slash") << QStringLiteral("/orders/acme%2Fadmin")
                                            << NormalizedPathError::DecodedSeparator;
    QTest::newRow("decoded-backslash") << QStringLiteral("/orders/acme%5Cadmin")
                                        << NormalizedPathError::DecodedSeparator;
    QTest::newRow("encoded-nul") << QStringLiteral("/orders/%00")
                                  << NormalizedPathError::ControlCharacter;
    QTest::newRow("encoded-control") << QStringLiteral("/orders/%1F")
                                      << NormalizedPathError::ControlCharacter;
    QTest::newRow("encoded-unicode-control") << QStringLiteral("/orders/%C2%80")
                                              << NormalizedPathError::ControlCharacter;
    QTest::newRow("illegal-literal") << QStringLiteral("/orders/[admin]")
                                      << NormalizedPathError::NonCanonicalEncoding;
    QTest::newRow("dot") << QStringLiteral("/orders/./edit")
                          << NormalizedPathError::PathTraversal;
    QTest::newRow("encoded-dot-dot") << QStringLiteral("/orders/%2E%2E/edit")
                                      << NormalizedPathError::PathTraversal;
    QTest::newRow("raw-backslash") << QStringLiteral("/orders\\42")
                                    << NormalizedPathError::DecodedSeparator;
    QTest::newRow("query") << QStringLiteral("/orders?tab=history")
                            << NormalizedPathError::NonCanonicalEncoding;
    QTest::newRow("fragment") << QStringLiteral("/orders#history")
                               << NormalizedPathError::NonCanonicalEncoding;

    QString literalControl = QStringLiteral("/orders/");
    literalControl.append(QChar(0x1f));
    QTest::newRow("literal-control") << literalControl << NormalizedPathError::ControlCharacter;
}

void NormalizedPathTest::rejectsUnsafeOrNonCanonicalPath()
{
    QFETCH(QString, encoded);
    QFETCH(NormalizedPathError, expectedError);

    const auto path = NormalizedPath::parse(encoded);

    QVERIFY(!path.isValid());
    QCOMPARE(path.error(), expectedError);
    QVERIFY(path.encoded().isEmpty());
    QVERIFY(path.decodedSegments().isEmpty());
}

QTEST_MAIN(NormalizedPathTest)

#include "tst_normalized_path.moc"
