#include "QmlSourcePolicy.h"

#include <QSet>
#include <QVector>

namespace {

enum class TokenKind { Identifier, String, Punctuation };

struct Token final {
    TokenKind kind;
    QString text;
};

bool identifierStart(const QChar value)
{
    return value == u'_' || value == u'$' || value.isLetter();
}

bool identifierPart(const QChar value)
{
    return identifierStart(value) || value.isDigit();
}

QVector<Token> tokenize(const QString &source)
{
    QVector<Token> result;
    for (qsizetype index = 0; index < source.size();) {
        const QChar value = source.at(index);
        if (value.isSpace()) {
            ++index;
            continue;
        }
        if (value == u'/' && index + 1 < source.size() && source.at(index + 1) == u'/') {
            index += 2;
            while (index < source.size() && source.at(index) != u'\n') ++index;
            continue;
        }
        if (value == u'/' && index + 1 < source.size() && source.at(index + 1) == u'*') {
            index += 2;
            while (index + 1 < source.size()
                   && !(source.at(index) == u'*' && source.at(index + 1) == u'/')) {
                ++index;
            }
            index = qMin(source.size(), index + 2);
            continue;
        }
        if (value == u'\'' || value == u'"') {
            const QChar quote = value;
            QString decoded;
            ++index;
            while (index < source.size() && source.at(index) != quote) {
                if (source.at(index) == u'\\' && index + 1 < source.size()) {
                    ++index;
                }
                decoded += source.at(index++);
            }
            if (index < source.size()) ++index;
            result.push_back({TokenKind::String, std::move(decoded)});
            continue;
        }
        if (identifierStart(value)) {
            const qsizetype start = index++;
            while (index < source.size() && identifierPart(source.at(index))) ++index;
            result.push_back({TokenKind::Identifier, source.sliced(start, index - start)});
            continue;
        }
        result.push_back({TokenKind::Punctuation, QString(value)});
        ++index;
    }
    return result;
}

void addUnique(QStringList &values, const QString &value)
{
    if (!values.contains(value)) values.push_back(value);
}

bool isAlias(const QSet<QString> &aliases, const Token &token)
{
    return token.kind == TokenKind::Identifier && aliases.contains(token.text);
}

QString concatenatedString(const QVector<Token> &tokens, qsizetype begin, qsizetype end)
{
    QString value;
    bool expectString = true;
    for (qsizetype index = begin; index < end; ++index) {
        if (expectString) {
            if (tokens.at(index).kind != TokenKind::String) return {};
            value += tokens.at(index).text;
        } else if (tokens.at(index).text != QStringLiteral("+")) {
            return {};
        }
        expectString = !expectString;
    }
    return !expectString ? value : QString{};
}

} // namespace

QStringList QmlSourcePolicy::violations(const QByteArray &source)
{
    const QVector<Token> tokens = tokenize(QString::fromUtf8(source));
    QStringList result;
    QSet<QString> qtAliases{QStringLiteral("Qt")};

    for (qsizetype index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens.at(index).kind != TokenKind::Identifier) continue;
        if ((tokens.at(index + 1).text == QStringLiteral("=")
             || tokens.at(index + 1).text == QStringLiteral(":"))
            && index + 2 < tokens.size() && isAlias(qtAliases, tokens.at(index + 2))) {
            qtAliases.insert(tokens.at(index).text);
        }
    }

    QVector<bool> loaderScopes;
    bool nextScopeIsLoader = false;
    for (qsizetype index = 0; index < tokens.size(); ++index) {
        const Token &token = tokens.at(index);
        if (token.kind == TokenKind::Identifier && token.text == QStringLiteral("Loader")
            && index + 1 < tokens.size() && tokens.at(index + 1).text == QStringLiteral("{")) {
            nextScopeIsLoader = true;
        }
        if (token.text == QStringLiteral("{")) {
            loaderScopes.push_back(nextScopeIsLoader);
            nextScopeIsLoader = false;
            continue;
        }
        if (token.text == QStringLiteral("}")) {
            if (!loaderScopes.isEmpty()) loaderScopes.pop_back();
            continue;
        }
        const bool inLoader = !loaderScopes.isEmpty() && loaderScopes.back();
        if (inLoader && token.kind == TokenKind::Identifier
            && token.text == QStringLiteral("source") && index + 1 < tokens.size()
            && tokens.at(index + 1).text == QStringLiteral(":")) {
            addUnique(result, QStringLiteral("dynamic-loader-source"));
        }
        if (token.kind == TokenKind::Identifier && token.text == QStringLiteral("import")) {
            if (index + 1 < tokens.size() && tokens.at(index + 1).text == QStringLiteral("(")) {
                addUnique(result, QStringLiteral("dynamic-import"));
            } else if (index + 1 < tokens.size()
                       && tokens.at(index + 1).kind == TokenKind::String) {
                const QString path = tokens.at(index + 1).text.toLower();
                if (path.startsWith(QStringLiteral("http:"))
                    || path.startsWith(QStringLiteral("https:"))
                    || path.startsWith(QStringLiteral("file:"))) {
                    addUnique(result, QStringLiteral("remote-import"));
                }
            }
        }
        if (!isAlias(qtAliases, token) || index + 1 >= tokens.size()) continue;
        if (tokens.at(index + 1).text == QStringLiteral("[")) {
            qsizetype end = index + 2;
            while (end < tokens.size() && tokens.at(end).text != QStringLiteral("]")) ++end;
            addUnique(result, QStringLiteral("qt-dynamic-member"));
            const QString member = concatenatedString(tokens, index + 2, end);
            if (member == QStringLiteral("createComponent"))
                addUnique(result, QStringLiteral("qml-create-component"));
            if (member == QStringLiteral("createQmlObject"))
                addUnique(result, QStringLiteral("qml-create-object"));
            if (member == QStringLiteral("include"))
                addUnique(result, QStringLiteral("qt-include"));
        } else if (tokens.at(index + 1).text == QStringLiteral(".")
                   && index + 2 < tokens.size()) {
            const QString member = tokens.at(index + 2).text;
            if (member == QStringLiteral("createComponent"))
                addUnique(result, QStringLiteral("qml-create-component"));
            else if (member == QStringLiteral("createQmlObject"))
                addUnique(result, QStringLiteral("qml-create-object"));
            else if (member == QStringLiteral("include"))
                addUnique(result, QStringLiteral("qt-include"));
            else if (member == QStringLiteral("openUrlExternally"))
                addUnique(result, QStringLiteral("external-url"));
        }
    }

    for (qsizetype index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens.at(index).text == QStringLiteral(".")
            && tokens.at(index + 1).text == QStringLiteral("setSource")
            && tokens.at(index + 2).text == QStringLiteral("(")) {
            addUnique(result, QStringLiteral("loader-set-source"));
        }
    }
    for (const Token &token : tokens) {
        if (token.kind != TokenKind::Identifier) continue;
        if (token.text == QStringLiteral("XMLHttpRequest"))
            addUnique(result, QStringLiteral("raw-network"));
        else if (token.text == QStringLiteral("WorkerScript"))
            addUnique(result, QStringLiteral("worker-network"));
        else if (token.text == QStringLiteral("QFile"))
            addUnique(result, QStringLiteral("native-file"));
        else if (token.text == QStringLiteral("FileDialog"))
            addUnique(result, QStringLiteral("native-file-dialog"));
        else if (token.text == QStringLiteral("plugin"))
            addUnique(result, QStringLiteral("native-plugin"));
    }
    return result;
}
