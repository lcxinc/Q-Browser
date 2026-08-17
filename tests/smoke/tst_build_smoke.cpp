#include <QtTest>

class BuildSmokeTest final : public QObject {
  Q_OBJECT

private slots:
  void qtRuntimeIsUsable() {
    QCOMPARE(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR));
  }
};

QTEST_MAIN(BuildSmokeTest)
#include "tst_build_smoke.moc"
