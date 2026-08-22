#include "HostApplication.h"
#include "WorkerRetirementManager.h"

#include <QApplication>
#include <QEvent>
#include <QFile>

#include <cstdio>

namespace
{
void recordHostDiagnosticPhase(const QByteArray &phase)
{
    if (!qEnvironmentVariableIsSet("Q_BROWSER_HOST_DIAGNOSTIC_PHASES")) return;
    QFile standardError;
    if (!standardError.open(stderr, QIODevice::WriteOnly,
                            QFileDevice::DontCloseHandle)) {
        return;
    }
    (void)standardError.write(QByteArrayLiteral("qbrowser-host phase: ")
                              + phase + '\n');
    (void)standardError.flush();
}
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbrowser-host"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowser"));
    QStringList arguments = QCoreApplication::arguments();
    if (!arguments.isEmpty()) arguments.removeFirst();
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    if (!parsed.value.has_value()) return 64;
    recordHostDiagnosticPhase("arguments-parsed");
    int applicationResult = 64;
    {
        HostApplication host(std::move(*parsed.value));
        recordHostDiagnosticPhase("application-created");
        QObject::connect(
            &host, &HostApplication::updateLifecycleFailed, &host,
            [](const QString &stableError) {
                QFile standardError;
                if (!standardError.open(stderr, QIODevice::WriteOnly,
                                        QFileDevice::DontCloseHandle)) {
                    return;
                }
                const QByteArray line =
                    QByteArrayLiteral("qbrowser-host lifecycle failure: ")
                    + stableError.toUtf8() + '\n';
                (void)standardError.write(line);
                (void)standardError.flush();
            });
        if (host.start()) {
            recordHostDiagnosticPhase("event-loop-enter");
            applicationResult = application.exec();
            recordHostDiagnosticPhase("event-loop-exit");
        }
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return WorkerRetirementManager::instance().shutdownChecked(10'000)
        ? applicationResult : 70;
}
