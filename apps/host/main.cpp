#include "HostApplication.h"
#include "WorkerRetirementManager.h"

#include <QApplication>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbrowser-host"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowser"));
    QStringList arguments = QCoreApplication::arguments();
    if (!arguments.isEmpty()) arguments.removeFirst();
    HostRuntimeConfigResult parsed = HostRuntimeConfig::fromArguments(arguments);
    if (!parsed.value.has_value()) return 64;
    int applicationResult = 64;
    {
        HostApplication host(std::move(*parsed.value));
        if (host.start()) applicationResult = application.exec();
    }
    return WorkerRetirementManager::instance().shutdownChecked(10'000)
        ? applicationResult : 70;
}
