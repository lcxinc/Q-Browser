#include "WorkerApplication.h"

#include <QGuiApplication>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbrowser-worker"));
    WorkerApplication worker;
    if (!worker.start(QCoreApplication::arguments())) {
        return WorkerApplication::invalidLaunchExitCode();
    }
    return application.exec();
}
