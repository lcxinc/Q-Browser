#include "HostApplication.h"

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
    HostApplication host(std::move(*parsed.value));
    if (!host.start()) {
        return 64;
    }
    return application.exec();
}
