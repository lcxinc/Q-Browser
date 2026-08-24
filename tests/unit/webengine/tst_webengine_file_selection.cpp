#include "WebSessionProfile.h"
#include "WebSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <optional>

namespace {

class FileInputServer final : public QObject
{
public:
    explicit FileInputServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    if (socket->property("responded").toBool()) {
                        return;
                    }
                    QByteArray request = socket->property("requestBuffer").toByteArray();
                    request += socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("requestBuffer", request);
                        return;
                    }
                    socket->setProperty("responded", true);
                    const QByteArray body = QByteArrayLiteral(
                        "<!doctype html><title>file inputs</title>"
                        "<input id='file' type='file' style='display:block;margin:8px'>"
                        "<input id='directory' type='file' webkitdirectory "
                        "style='display:block;margin:8px'>"
                        "<script>"
                        "window.fileChanges=0;window.directoryChanges=0;"
                        "file.addEventListener('change',()=>window.fileChanges++);"
                        "directory.addEventListener('change',()=>window.directoryChanges++);"
                        "</script>");
                    const QByteArray response = QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/file-inputs")
                        .arg(server_.serverPort()));
    }
    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/")
                        .arg(server_.serverPort()));
    }

private:
    QTcpServer server_;
};

bool navigateAndWait(WebSurface &surface, const QUrl &url)
{
    QSignalSpy spy(&surface, &WebSurface::navigationFinished);
    if (!surface.navigate(url)) {
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000) {
        if (!spy.wait(10000 - static_cast<int>(elapsed.elapsed()))) {
            return false;
        }
        if (surface.currentUrl() == url && !surface.page()->isLoading()) {
            return true;
        }
    }
    return false;
}

std::optional<QJsonValue> evaluateJavaScript(QWebEnginePage *page,
                                             const QString &script)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::optional<QJsonValue> result;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->runJavaScript(script, [&result, &loop](const QVariant &value) {
        result.emplace(QJsonValue::fromVariant(value));
        loop.quit();
    });
    timeout.start(5000);
    loop.exec();
    return result;
}

} // namespace

class WebEngineFileSelectionTest final : public QObject
{
    Q_OBJECT

private slots:
    void actualInputsNeverOpenFileDialogOrReceiveFiles();
};

void WebEngineFileSelectionTest::actualInputsNeverOpenFileDialogOrReceiveFiles()
{
    FileInputServer server;
    QVERIFY(server.listen());
    WebSessionProfile session(server.origin());
    WebSurface surface(session, server.url());
    surface.resize(500, 300);
    surface.move(-10000, -10000);
    surface.show();
    QSignalSpy deniedSpy(&surface, &WebSurface::fileSelectionDenied);

    QVERIFY(navigateAndWait(surface, server.url()));
    QTest::qWait(250);
    QWidget *keyTarget = surface.view()->focusProxy();
    QVERIFY(keyTarget != nullptr);

    bool fileDialogOpened = false;
    QTimer dialogCloser;
    connect(&dialogCloser, &QTimer::timeout, &surface, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QFileDialog *>(widget);
            if (dialog != nullptr && dialog->isVisible()) {
                fileDialogOpened = true;
                dialog->reject();
            }
        }
    });
    dialogCloser.start(10);

    const auto fileFocused = evaluateJavaScript(
        surface.page(), QStringLiteral("file.focus();document.activeElement.id"));
    QVERIFY(fileFocused.has_value());
    QCOMPARE(*fileFocused, QJsonValue(QStringLiteral("file")));
    QTest::keyClick(keyTarget, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(deniedSpy.count() >= 1 || fileDialogOpened, 5000);
    QVERIFY(!fileDialogOpened);
    QCOMPARE(deniedSpy.at(0).at(0).toBool(), false);

    const auto directoryFocused = evaluateJavaScript(
        surface.page(), QStringLiteral("directory.focus();document.activeElement.id"));
    QVERIFY(directoryFocused.has_value());
    QCOMPARE(*directoryFocused, QJsonValue(QStringLiteral("directory")));
    QTest::keyClick(keyTarget, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(deniedSpy.count() >= 2 || fileDialogOpened, 5000);
    dialogCloser.stop();
    QVERIFY(!fileDialogOpened);
    QCOMPARE(deniedSpy.at(1).at(0).toBool(), true);

    const auto state = evaluateJavaScript(
        surface.page(), QStringLiteral(
            "JSON.stringify({fileCount:file.files.length,"
            "directoryCount:directory.files.length,"
            "fileChanges:window.fileChanges,"
            "directoryChanges:window.directoryChanges})"));
    QVERIFY(state.has_value());
    QVERIFY(state->isString());
    const QJsonObject inputState =
        QJsonDocument::fromJson(state->toString().toUtf8()).object();
    QCOMPARE(inputState.value(QStringLiteral("fileCount")).toInt(), 0);
    QCOMPARE(inputState.value(QStringLiteral("directoryCount")).toInt(), 0);
    QCOMPARE(inputState.value(QStringLiteral("fileChanges")).toInt(), 0);
    QCOMPARE(inputState.value(QStringLiteral("directoryChanges")).toInt(), 0);
}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("q-browser-file-selection-test"));
    QCoreApplication::setOrganizationName(QStringLiteral("QBrowserTest"));
    WebEngineFileSelectionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_webengine_file_selection.moc"
