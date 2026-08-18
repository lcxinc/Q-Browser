#include "HostApplication.h"

#include <QApplication>
#include <QUrl>

namespace {

QUrl mockOrigin(const QStringList &arguments)
{
    constexpr QStringView prefix = u"--mock-origin=";
    for (const QString &argument : arguments) {
        if (argument.startsWith(prefix)) {
            return QUrl(argument.sliced(prefix.size()), QUrl::StrictMode);
        }
    }
    return QUrl(QStringLiteral("http://127.0.0.1:4173/"));
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qbrowser-host"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowser"));
    HostApplication host(mockOrigin(QCoreApplication::arguments()));
    if (!host.start()) {
        return 64;
    }
    return application.exec();
}
