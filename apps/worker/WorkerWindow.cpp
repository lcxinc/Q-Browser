#include "WorkerWindow.h"

#include "RuntimeFacade.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QQmlContext>
#include <QQmlEngine>

namespace {

class DeniedReply final : public QNetworkReply
{
public:
    explicit DeniedReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOpenMode(QIODevice::ReadOnly);
        setError(QNetworkReply::ContentAccessDenied,
                 QStringLiteral("worker network access is broker-only"));
        QMetaObject::invokeMethod(this, [this] {
            emit errorOccurred(error());
            emit finished();
        }, Qt::QueuedConnection);
    }
    void abort() override {}
protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class DeniedNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit DeniedNetworkAccessManager(QObject *parent) : QNetworkAccessManager(parent) {}
protected:
    QNetworkReply *createRequest(Operation,
                                 const QNetworkRequest &request,
                                 QIODevice *) override
    {
        return new DeniedReply(request, this);
    }
};

class DeniedNetworkAccessManagerFactory final
    : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        return new DeniedNetworkAccessManager(parent);
    }
};

bool isStrictLocalEntry(const QString &packageDirectory,
                        const QString &entryPoint,
                        QString &resolved)
{
    if (entryPoint.isEmpty() || QDir::isAbsolutePath(entryPoint)
        || entryPoint.contains(u'\\') || entryPoint.split(u'/').contains(QStringLiteral(".."))) {
        return false;
    }
    const QFileInfo rootInfo(packageDirectory);
    const QFileInfo entryInfo(QDir(packageDirectory).filePath(entryPoint));
    const QString root = rootInfo.canonicalFilePath();
    resolved = entryInfo.canonicalFilePath();
    return rootInfo.isDir() && entryInfo.isFile() && !root.isEmpty() && !resolved.isEmpty()
        && resolved.startsWith(root + u'/') && entryInfo.suffix() == QStringLiteral("qml");
}

bool packageDeclaresNativeQmlPlugin(const QString &packageDirectory)
{
    const QString qmlRoot = QDir(packageDirectory).filePath(QStringLiteral("qml"));
    QDirIterator files(qmlRoot, {QStringLiteral("qmldir")},
                       QDir::Files | QDir::NoSymLinks,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile descriptor(files.next());
        if (!descriptor.open(QIODevice::ReadOnly) || descriptor.size() > 1024 * 1024) {
            return true;
        }
        while (!descriptor.atEnd()) {
            const QString line = QString::fromUtf8(descriptor.readLine())
                                     .section(u'#', 0, 0).trimmed();
            const QStringList tokens = line.split(
                QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if ((!tokens.isEmpty() && tokens.first() == QStringLiteral("plugin"))
                || (tokens.size() >= 2
                    && tokens.at(0) == QStringLiteral("optional")
                    && tokens.at(1) == QStringLiteral("plugin"))) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

WorkerWindow::WorkerWindow()
    : networkFactory_(std::make_unique<DeniedNetworkAccessManagerFactory>())
{
    view_.setResizeMode(QQuickView::SizeRootObjectToView);
    view_.setFlags(Qt::FramelessWindowHint | Qt::Tool);
}

WorkerWindow::~WorkerWindow() = default;

bool WorkerWindow::load(const QString &packageDirectory,
                        const QString &entryPoint,
                        RuntimeFacade *runtimeFacade)
{
    if (runtimeFacade == nullptr) {
        errorString_ = QStringLiteral("worker.runtime.missing_facade");
        return false;
    }
    QString entry;
    if (!isStrictLocalEntry(packageDirectory, entryPoint, entry)) {
        errorString_ = QStringLiteral("worker.qml.invalid_entry");
        return false;
    }
    if (packageDeclaresNativeQmlPlugin(packageDirectory)) {
        errorString_ = QStringLiteral("worker.qml.native_plugin_forbidden");
        return false;
    }
    QQmlEngine *engine = view_.engine();
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    engine->setImportPathList({QDir(packageDirectory).filePath(QStringLiteral("qml")),
                               QDir(applicationDirectory).filePath(QStringLiteral("qml")),
                               QStringLiteral("qrc:/qt/qml")});
    engine->setNetworkAccessManagerFactory(networkFactory_.get());
    engine->rootContext()->setContextProperty(QStringLiteral("Runtime"), runtimeFacade);
    view_.setSource(QUrl::fromLocalFile(entry));
    if (view_.status() != QQuickView::Ready || view_.rootObject() == nullptr) {
        errorString_ = QStringLiteral("worker.qml.load_failed");
        return false;
    }
    view_.show();
    view_.create();
    return view_.winId() != 0;
}

WId WorkerWindow::windowId() const noexcept { return view_.winId(); }
QString WorkerWindow::errorString() const { return errorString_; }
