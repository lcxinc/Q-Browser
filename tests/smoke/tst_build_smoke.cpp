#include <QtTest>

class BuildSmokeTest final : public QObject {
  Q_OBJECT

private slots:
  void qtRuntimeIsUsable() {
    QVERIFY(QVersionNumber::fromString(qVersion()) >= QVersionNumber(6, 11));
  }
};

QTEST_MAIN(BuildSmokeTest)
#include "tst_build_smoke.moc"
