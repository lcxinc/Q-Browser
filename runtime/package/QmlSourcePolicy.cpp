#include "QmlSourcePolicy.h"

#include <QSet>
#include <QUrl>
#include <QVector>

namespace {

enum class TokenKind { Identifier, String, Regex, Punctuation };
struct Token final { TokenKind kind; QString text; };

bool identifierStart(const QChar value)
{
    return value == u'_' || value == u'$' || value.isLetter();
}

bool identifierPart(const QChar value)
{
    return identifierStart(value) || value.isDigit();
}

bool tokenCanEndExpression(const Token &token)
{
    if (token.kind == TokenKind::Identifier || token.kind == TokenKind::String
        || token.kind == TokenKind::Regex) {
        return token.text != QStringLiteral("return")
            && token.text != QStringLiteral("case")
            && token.text != QStringLiteral("throw");
    }
    return token.text == QStringLiteral(")") || token.text == QStringLiteral("]")
        || token.text == QStringLiteral("}");
}

class Lexer final
{
public:
    explicit Lexer(QString source) : source_(std::move(source)) {}

    QVector<Token> tokens()
    {
        scan(false);
        return std::move(tokens_);
    }

    bool malformed() const noexcept { return malformed_; }

private:
    void scan(const bool templateExpression)
    {
        int braceDepth = 0;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_);
            if (templateExpression && value == u'}' && braceDepth == 0) {
                ++index_;
                return;
            }
            if (value.isSpace()) {
                ++index_;
                continue;
            }
            if (value == u'/' && peek(1) == u'/') {
                index_ += 2;
                while (index_ < source_.size() && source_.at(index_) != u'\n') ++index_;
                continue;
            }
            if (value == u'/' && peek(1) == u'*') {
                index_ += 2;
                while (index_ + 1 < source_.size()
                       && !(source_.at(index_) == u'*' && peek(1) == u'/')) ++index_;
                if (index_ + 1 >= source_.size()) {
                    malformed_ = true;
                    return;
                }
                index_ += 2;
                continue;
            }
            if (value == u'\'' || value == u'"') {
                scanString(value);
                continue;
            }
            if (value == u'`') {
                scanTemplate();
                continue;
            }
            if (value == u'/' && beginsRegex()) {
                scanRegex();
                continue;
            }
            if (identifierStart(value)) {
                const qsizetype start = index_++;
                while (index_ < source_.size() && identifierPart(source_.at(index_))) ++index_;
                tokens_.push_back({TokenKind::Identifier,
                                   source_.sliced(start, index_ - start)});
                continue;
            }
            if (value == u'{') ++braceDepth;
            else if (value == u'}' && braceDepth > 0) --braceDepth;
            tokens_.push_back({TokenKind::Punctuation, QString(value)});
            ++index_;
        }
        if (templateExpression) malformed_ = true;
    }

    QChar peek(const qsizetype offset) const
    {
        const qsizetype position = index_ + offset;
        return position < source_.size() ? source_.at(position) : QChar{};
    }

    bool beginsRegex() const
    {
        return tokens_.isEmpty() || !tokenCanEndExpression(tokens_.back());
    }

    void scanString(const QChar quote)
    {
        QString decoded;
        ++index_;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_++);
            if (value == quote) {
                tokens_.push_back({TokenKind::String, std::move(decoded)});
                return;
            }
            if (value != u'\\') {
                decoded += value;
                continue;
            }
            if (index_ >= source_.size()) break;
            const QChar escaped = source_.at(index_++);
            if (escaped == u'u' && index_ + 4 <= source_.size()) {
                bool ok = false;
                const ushort code = source_.sliced(index_, 4).toUShort(&ok, 16);
                if (ok) {
                    decoded += QChar(code);
                    index_ += 4;
                    continue;
                }
            }
            if (escaped == u'n') decoded += u'\n';
            else if (escaped == u'r') decoded += u'\r';
            else if (escaped == u't') decoded += u'\t';
            else decoded += escaped;
        }
        malformed_ = true;
    }

    void scanRegex()
    {
        ++index_;
        bool inCharacterClass = false;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_++);
            if (value == u'\\' && index_ < source_.size()) {
                ++index_;
                continue;
            }
            if (value == u'[') inCharacterClass = true;
            else if (value == u']') inCharacterClass = false;
            else if (value == u'/' && !inCharacterClass) {
                while (index_ < source_.size() && source_.at(index_).isLetter()) ++index_;
                tokens_.push_back({TokenKind::Regex, QStringLiteral("regex")});
                return;
            }
            if (value == u'\n' || value == u'\r') break;
        }
        malformed_ = true;
    }

    void scanTemplate()
    {
        ++index_;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_++);
            if (value == u'\\' && index_ < source_.size()) {
                ++index_;
                continue;
            }
            if (value == u'`') return;
            if (value == u'$' && index_ < source_.size() && source_.at(index_) == u'{') {
                ++index_;
                scan(true);
                if (malformed_) return;
            }
        }
        malformed_ = true;
    }

    QString source_;
    qsizetype index_ = 0;
    QVector<Token> tokens_;
    bool malformed_ = false;
};

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

bool safeSourceUrl(const QString &value)
{
    if (value.startsWith(QStringLiteral("qrc:/"), Qt::CaseInsensitive)) return true;
    if (value.isEmpty() || value.startsWith(u'/') || value.startsWith(u'\\')) return false;
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && url.scheme().isEmpty();
}

} // namespace

QStringList QmlSourcePolicy::violations(const QByteArray &source)
{
    Lexer lexer(QString::fromUtf8(source));
    const QVector<Token> tokens = lexer.tokens();
    QStringList result;
    if (lexer.malformed()) addUnique(result, QStringLiteral("malformed-source"));

    QSet<QString> qtAliases{QStringLiteral("Qt")};
    QSet<QString> loaderAliases{QStringLiteral("Loader"), QStringLiteral("loader")};
    for (qsizetype index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens.at(index).kind == TokenKind::Identifier
            && (tokens.at(index + 1).text == QStringLiteral("=")
                || tokens.at(index + 1).text == QStringLiteral(":"))
            && isAlias(qtAliases, tokens.at(index + 2))) {
            qtAliases.insert(tokens.at(index).text);
        }
        if (tokens.at(index).kind == TokenKind::Identifier
            && tokens.at(index).text.contains(QStringLiteral("loader"),
                                              Qt::CaseInsensitive)) {
            loaderAliases.insert(tokens.at(index).text);
        }
        if (tokens.at(index).kind == TokenKind::Identifier
            && (tokens.at(index + 1).text == QStringLiteral("=")
                || tokens.at(index + 1).text == QStringLiteral(":"))
            && isAlias(loaderAliases, tokens.at(index + 2))) {
            loaderAliases.insert(tokens.at(index).text);
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

        if (token.kind == TokenKind::Identifier && token.text == QStringLiteral("source")
            && index + 1 < tokens.size() && tokens.at(index + 1).text == QStringLiteral(":")) {
            if (inLoader) addUnique(result, QStringLiteral("dynamic-loader-source"));
            if (index + 2 >= tokens.size() || tokens.at(index + 2).kind != TokenKind::String)
                addUnique(result, QStringLiteral("dynamic-url-source"));
            else if (!safeSourceUrl(tokens.at(index + 2).text))
                addUnique(result, QStringLiteral("unsafe-url-source"));
        }

        if (token.kind == TokenKind::Identifier && token.text == QStringLiteral("import")) {
            if (index + 1 < tokens.size() && tokens.at(index + 1).text == QStringLiteral("("))
                addUnique(result, QStringLiteral("dynamic-import"));
            else if (index + 1 < tokens.size()
                     && tokens.at(index + 1).kind == TokenKind::String
                     && !safeSourceUrl(tokens.at(index + 1).text))
                addUnique(result, QStringLiteral("remote-import"));
        }

        if (isAlias(qtAliases, token) && index + 1 < tokens.size()) {
            if (tokens.at(index + 1).text == QStringLiteral("[")) {
                qsizetype end = index + 2;
                while (end < tokens.size() && tokens.at(end).text != QStringLiteral("]")) ++end;
                addUnique(result, QStringLiteral("qt-dynamic-member"));
                const QString member = concatenatedString(tokens, index + 2, end);
                if (member == QStringLiteral("createComponent"))
                    addUnique(result, QStringLiteral("qml-create-component"));
                else if (member == QStringLiteral("createQmlObject"))
                    addUnique(result, QStringLiteral("qml-create-object"));
                else if (member == QStringLiteral("include"))
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

        if (token.text == QStringLiteral("[") && index > 0) {
            qsizetype end = index + 1;
            while (end < tokens.size() && tokens.at(end).text != QStringLiteral("]")) ++end;
            const QString member = concatenatedString(tokens, index + 1, end);
            if (member == QStringLiteral("setSource"))
                addUnique(result, QStringLiteral("loader-set-source"));
            if (isAlias(loaderAliases, tokens.at(index - 1)))
                addUnique(result, QStringLiteral("loader-dynamic-member"));
        }
        if (token.text == QStringLiteral(".") && index + 2 < tokens.size()
            && tokens.at(index + 1).text == QStringLiteral("setSource")
            && tokens.at(index + 2).text == QStringLiteral("("))
            addUnique(result, QStringLiteral("loader-set-source"));

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
