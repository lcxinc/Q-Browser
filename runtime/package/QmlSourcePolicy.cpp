#include "QmlSourcePolicy.h"

#include <QSet>
#include <QUrl>
#include <QVector>

namespace {

enum class TokenKind { Identifier, Numeric, String, Template, Regex, Punctuation };
struct Token final { TokenKind kind; QString text; int line; bool regexMayFollow = false; };

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
    if (token.kind == TokenKind::Identifier || token.kind == TokenKind::Numeric
        || token.kind == TokenKind::String || token.kind == TokenKind::Template
        || token.kind == TokenKind::Regex) {
        return token.text != QStringLiteral("return")
            && token.text != QStringLiteral("case")
            && token.text != QStringLiteral("throw");
    }
    return token.text == QStringLiteral("++")
        || token.text == QStringLiteral("--")
        || (token.text == QStringLiteral(")") && !token.regexMayFollow)
        || token.text == QStringLiteral("]")
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
                if (value == u'\n') ++line_;
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
                       && !(source_.at(index_) == u'*' && peek(1) == u'/')) {
                    if (source_.at(index_) == u'\n') ++line_;
                    ++index_;
                }
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
            if (value.isDigit() || (value == u'.' && peek(1).isDigit())) {
                scanNumber();
                continue;
            }
            if (value == u'/' && peek(1) == u'=') {
                tokens_.push_back({TokenKind::Punctuation,
                                   QStringLiteral("/="), line_});
                index_ += 2;
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
                                   source_.sliced(start, index_ - start), line_});
                continue;
            }
            if ((value == u'+' && peek(1) == u'+')
                || (value == u'-' && peek(1) == u'-')) {
                tokens_.push_back({TokenKind::Punctuation,
                                   source_.sliced(index_, 2), line_});
                index_ += 2;
                continue;
            }
            bool regexMayFollow = false;
            if (value == u'(') {
                static const QSet<QString> controlKeywords{
                    QStringLiteral("if"), QStringLiteral("while"),
                    QStringLiteral("for"), QStringLiteral("with"),
                    QStringLiteral("switch"), QStringLiteral("catch")};
                controlParens_.push_back(!tokens_.isEmpty()
                    && tokens_.back().kind == TokenKind::Identifier
                    && controlKeywords.contains(tokens_.back().text));
            } else if (value == u')' && !controlParens_.isEmpty()) {
                regexMayFollow = controlParens_.back();
                controlParens_.pop_back();
            }
            if (value == u'{') ++braceDepth;
            else if (value == u'}' && braceDepth > 0) --braceDepth;
            tokens_.push_back({TokenKind::Punctuation, QString(value), line_, regexMayFollow});
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

    bool scanDigits(const int base)
    {
        bool sawDigit = false;
        bool lastWasSeparator = false;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_);
            int digit = -1;
            if (value.isDigit()) digit = value.digitValue();
            else if (value >= u'a' && value <= u'f') digit = 10 + value.unicode() - u'a';
            else if (value >= u'A' && value <= u'F') digit = 10 + value.unicode() - u'A';
            if (digit >= 0 && digit < base) {
                sawDigit = true;
                lastWasSeparator = false;
                ++index_;
                continue;
            }
            if (value == u'_') {
                if (!sawDigit || lastWasSeparator) malformed_ = true;
                lastWasSeparator = true;
                ++index_;
                continue;
            }
            break;
        }
        if (lastWasSeparator) malformed_ = true;
        return sawDigit;
    }

    void scanNumber()
    {
        const qsizetype start = index_;
        const int startLine = line_;
        bool fractional = false;
        bool exponent = false;
        if (source_.at(index_) == u'.') {
            fractional = true;
            ++index_;
            if (!scanDigits(10)) malformed_ = true;
        } else if (source_.at(index_) == u'0'
                   && (peek(1) == u'x' || peek(1) == u'X'
                       || peek(1) == u'b' || peek(1) == u'B'
                       || peek(1) == u'o' || peek(1) == u'O')) {
            const QChar prefix = peek(1).toLower();
            const int base = prefix == u'x' ? 16 : (prefix == u'b' ? 2 : 8);
            index_ += 2;
            if (!scanDigits(base)) malformed_ = true;
        } else {
            (void)scanDigits(10);
            if (index_ < source_.size() && source_.at(index_) == u'.') {
                fractional = true;
                ++index_;
                (void)scanDigits(10);
            }
            if (index_ < source_.size()
                && (source_.at(index_) == u'e' || source_.at(index_) == u'E')) {
                exponent = true;
                ++index_;
                if (index_ < source_.size()
                    && (source_.at(index_) == u'+' || source_.at(index_) == u'-'))
                    ++index_;
                if (!scanDigits(10)) malformed_ = true;
            }
        }
        if (index_ < source_.size() && source_.at(index_) == u'n') {
            if (fractional || exponent) malformed_ = true;
            ++index_;
        }
        if (index_ < source_.size() && identifierStart(source_.at(index_)))
            malformed_ = true;
        tokens_.push_back({TokenKind::Numeric,
                           source_.sliced(start, index_ - start), startLine});
    }

    void scanString(const QChar quote)
    {
        QString decoded;
        const int startLine = line_;
        ++index_;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_++);
            if (value == quote) {
                tokens_.push_back({TokenKind::String, std::move(decoded), startLine});
                return;
            }
            if (value != u'\\') {
                if (value == u'\n') ++line_;
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
        const int startLine = line_;
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
                tokens_.push_back({TokenKind::Regex, QStringLiteral("regex"), startLine});
                return;
            }
            if (value == u'\n' || value == u'\r') break;
        }
        malformed_ = true;
    }

    void scanTemplate()
    {
        const int startLine = line_;
        ++index_;
        while (index_ < source_.size()) {
            const QChar value = source_.at(index_++);
            if (value == u'\\' && index_ < source_.size()) {
                ++index_;
                continue;
            }
            if (value == u'`') {
                tokens_.push_back({TokenKind::Template,
                                   QStringLiteral("template"), startLine});
                return;
            }
            if (value == u'\n') ++line_;
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
    int line_ = 1;
    QVector<bool> controlParens_;
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

bool staticSourceBinding(const QVector<Token> &tokens, const qsizetype valueIndex)
{
    if (valueIndex >= tokens.size()
        || tokens.at(valueIndex).kind != TokenKind::String
        || !safeSourceUrl(tokens.at(valueIndex).text)) return false;
    const qsizetype next = valueIndex + 1;
    if (next >= tokens.size()) return true;
    if (tokens.at(next).text == QStringLiteral(";")
        || tokens.at(next).text == QStringLiteral("}")) return true;
    if (tokens.at(next).line <= tokens.at(valueIndex).line) return false;
    if (next + 1 >= tokens.size()) return false;
    return tokens.at(next).kind == TokenKind::Identifier
        && (tokens.at(next + 1).text == QStringLiteral(":")
            || tokens.at(next + 1).text == QStringLiteral("{"));
}

} // namespace

bool QmlSourcePolicy::isQmlSourcePath(const QByteArrayView path)
{
    const QByteArray lower = path.toByteArray().toLower();
    return lower.endsWith(QByteArrayLiteral(".qml"))
        || lower.endsWith(QByteArrayLiteral(".js"))
        || lower.endsWith(QByteArrayLiteral(".mjs"));
}

QStringList QmlSourcePolicy::violations(const QByteArray &source)
{
    Lexer lexer(QString::fromUtf8(source));
    const QVector<Token> tokens = lexer.tokens();
    QStringList result;
    if (lexer.malformed()) addUnique(result, QStringLiteral("malformed-source"));

    QSet<QString> qtAliases{QStringLiteral("Qt")};
    QSet<QString> loaderAliases{QStringLiteral("Loader"), QStringLiteral("loader")};
    for (qsizetype index = 0; index + 4 < tokens.size(); ++index) {
        if (tokens.at(index).text != QStringLiteral("Loader")
            || tokens.at(index + 1).text != QStringLiteral("{")) continue;
        int depth = 1;
        for (qsizetype cursor = index + 2; cursor < tokens.size() && depth > 0; ++cursor) {
            if (tokens.at(cursor).text == QStringLiteral("{")) ++depth;
            else if (tokens.at(cursor).text == QStringLiteral("}")) --depth;
            else if (depth == 1 && tokens.at(cursor).text == QStringLiteral("id")
                     && cursor + 2 < tokens.size()
                     && tokens.at(cursor + 1).text == QStringLiteral(":")
                     && tokens.at(cursor + 2).kind == TokenKind::Identifier) {
                loaderAliases.insert(tokens.at(cursor + 2).text);
            }
        }
    }
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
            const bool inheritedLoader = !loaderScopes.isEmpty() && loaderScopes.back();
            loaderScopes.push_back(inheritedLoader || nextScopeIsLoader);
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
            const bool isStatic = staticSourceBinding(tokens, index + 2);
            if (!isStatic && inLoader)
                addUnique(result, QStringLiteral("dynamic-loader-source"));
            if (!isStatic)
                addUnique(result, QStringLiteral("dynamic-url-source"));
            else if (!safeSourceUrl(tokens.at(index + 2).text))
                addUnique(result, QStringLiteral("unsafe-url-source"));
            if (index + 2 < tokens.size()
                && tokens.at(index + 2).kind == TokenKind::String
                && !safeSourceUrl(tokens.at(index + 2).text))
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
        if (token.text == QStringLiteral(".") && index + 1 < tokens.size()
            && tokens.at(index + 1).text == QStringLiteral("setSource"))
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
