#pragma once
#include <QtWidgets>

// Presentation only: never changes source text or asks a language server to run.
class SourceHighlighter : public QSyntaxHighlighter {
public:
    enum Role { Plain, Keyword, Control, Type, Function, Variable, Parameter, Member, Number, String, Comment, Directive };
    enum Mode { Semantic, Syntax, Disabled };
    explicit SourceHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
    static QColor color(Role role) {
        static const char *colors[] = {"#d4d4d4", "#569cd6", "#c586c0", "#4ec9b0", "#dcdcaa", "#9cdcfe",
            "#b3d9ff", "#c8b6ff", "#b5cea8", "#ce9178", "#6a9955", "#c586c0"};
        return QColor(colors[role]);
    }
    void configure(const QString &path, const QJsonArray &positions) {
        const auto extension = QFileInfo(path).suffix().toLower();
        language = QSet<QString>{"py", "pyw", "pyi"}.contains(extension) ? "python" : extension == "cs" ? "c#" :
            extension == "java" ? "java" : QSet<QString>{"js", "jsx", "ts", "tsx", "mjs", "cjs", "mts", "cts"}.contains(extension) ? "javascript" :
            QSet<QString>{"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp"}.contains(extension) ? "c++" : "";
        marks.clear();
        for (const auto &value : positions) { const auto mark = value.toObject(); marks[mark.value("line").toInt() - 1].append(mark); }
    }
    void setMode(Mode value) { mode = value; rehighlight(); }
    static Role symbolRole(const QString &kind) {
        if (QSet<QString>{"ClassDecl", "StructDecl", "InterfaceDecl", "EnumDecl", "ClassTemplate", "TypeAliasDecl", "TypedefDecl", "DelegateDecl", "TypeParameter"}.contains(kind)) return Type;
        if (QSet<QString>{"FunctionDecl", "CXXMethod", "FunctionTemplate", "Constructor", "Destructor"}.contains(kind)) return Function;
        if (kind == "ParmDecl") return Parameter;
        if (QSet<QString>{"FieldDecl", "PropertyDecl", "EventDecl", "EnumConstantDecl"}.contains(kind)) return Member;
        if (kind == "VarDecl") return Variable;
        return Plain;
    }
protected:
    void highlightBlock(const QString &text) override {
        setCurrentBlockState(0); setCurrentBlockUserData(nullptr);
        if (mode == Disabled || language.isEmpty()) return;
        setFormat(0, int(text.size()), color(Plain));
        struct Token { int start, length; QString word; Role role; };
        QList<Token> tokens;
        int i = 0;
        auto stringEnd = [&](int from, const QString &end, bool doubled, bool escaped) {
            int at = from;
            while ((at = text.indexOf(end, at)) >= 0) {
                if (doubled && text.mid(at + end.size(), end.size()) == end) { at += int(end.size() * 2); continue; }
                int slashes = 0; if (escaped) for (int p = at - 1; p >= 0 && text[p] == '\\'; --p) ++slashes;
                if (slashes % 2) { ++at; continue; }
                return at + int(end.size());
            }
            return -1;
        };
        auto span = [&](int start, int contentStart, const QString &end, Role role, bool multiline, bool doubled = false, bool escaped = true) {
            const int finish = stringEnd(contentStart, end, doubled, escaped);
            i = finish < 0 ? int(text.size()) : finish;
            setFormat(start, i - start, color(role));
            if (finish < 0 && multiline) {
                auto *state = new State; state->end = end; state->role = role; state->doubled = doubled; state->escaped = escaped;
                setCurrentBlockUserData(state); setCurrentBlockState(1);
            }
        };
        if (previousBlockState() == 1) {
            if (const auto *state = dynamic_cast<State *>(currentBlock().previous().userData()))
                span(0, 0, state->end, state->role, true, state->doubled, state->escaped);
        }
        QString previousWord;
        while (i < text.size()) {
            const int start = i;
            const QChar ch = text[i];
            if (ch.isSpace()) { ++i; continue; }
            if ((language == "python" && ch == '#') || (language != "python" && text.mid(i, 2) == "//")) {
                setFormat(i, int(text.size()) - i, color(Comment)); break;
            }
            if (language != "python" && text.mid(i, 2) == "/*") { span(i, i + 2, "*/", Comment, true, false, false); continue; }
            if (language == "python" && (text.mid(i, 3) == "\"\"\"" || text.mid(i, 3) == "'''")) {
                span(i, i + 3, text.mid(i, 3), String, true); continue;
            }
            if (language == "c++" && text.mid(i, 2) == "R\"") {
                const int open = text.indexOf('(', i + 2);
                if (open >= 0 && open - i <= 18) {
                    const QString delimiter = text.mid(i + 2, open - i - 2);
                    if (!delimiter.contains(QRegularExpression("[\\s\\\\()]"))) { span(i, open + 1, ")" + delimiter + '"', String, true, false, false); continue; }
                }
            }
            if (language == "c#" && (text.mid(i, 2) == "@\"" || text.mid(i, 3) == "$@\"" || text.mid(i, 3) == "@$\"")) {
                const int prefix = text.mid(i, 2) == "@\"" ? 2 : 3; span(i, i + prefix, "\"", String, true, true, false); continue;
            }
            if ((language == "c#" || language == "java") && text.mid(i, 3) == "\"\"\"") {
                int quotes = 3; if (language == "c#") while (i + quotes < text.size() && text[i + quotes] == '"') ++quotes;
                span(i, i + quotes, QString(quotes, '"'), String, true, false, language == "java"); continue;
            }
            if (ch == '\'' || ch == '"' || (language == "javascript" && ch == '`')) {
                span(i, i + 1, QString(ch), String, ch == '`' || ((language == "c++" || language == "javascript") && text.endsWith('\\'))); continue;
            }
            // Conservative JS regular-expression detection; division stays an operator.
            if (language == "javascript" && ch == '/') {
                int before = i - 1; while (before >= 0 && text[before].isSpace()) --before;
                if (before < 0 || QString("=(:,[!&|?;{").contains(text[before]) || previousWord == "return" || previousWord == "yield") {
                    bool characterClass = false; int end = i + 1;
                    for (; end < text.size(); ++end) {
                        if (text[end] == '\\') { ++end; continue; }
                        if (text[end] == '[') characterClass = true;
                        if (text[end] == ']') characterClass = false;
                        if (text[end] == '/' && !characterClass) break;
                    }
                    if (end < text.size()) { i = end + 1; while (i < text.size() && text[i].isLetter()) ++i; setFormat(start, i - start, color(String)); previousWord.clear(); continue; }
                }
            }
            if (ch.isDigit()) {
                ++i;
                while (i < text.size() && (text[i].isLetterOrNumber() || text[i] == '_' || text[i] == '.' ||
                    ((text[i] == '+' || text[i] == '-') && QString("eEpP").contains(text[i - 1])))) ++i;
                setFormat(start, i - start, color(Number)); previousWord.clear(); continue;
            }
            if (identifierStart(ch)) {
                ++i; while (i < text.size() && identifierPart(text[i])) ++i;
                const QString word = text.mid(start, i - start);
                Role role = Variable;
                if (controls().contains(word)) role = Control;
                else if (keywords().contains(word)) role = Keyword;
                else if (types().contains(word)) role = Type;
                else if (typeContexts().contains(previousWord)) role = Type;
                else if (previousWord == "def" || previousWord == "function") role = Function;
                else {
                    int next = i; while (next < text.size() && text[next].isSpace()) ++next;
                    int before = start - 1; while (before >= 0 && text[before].isSpace()) --before;
                    if (next < text.size() && text[next] == '(') role = Function;
                    else if (before >= 0 && text[before] == '.') role = Member;
                }
                tokens.append({start, i - start, word, role}); setFormat(start, i - start, color(role)); previousWord = word; continue;
            }
            if (ch == '#' && language != "python" && text.left(i).trimmed().isEmpty()) {
                ++i; while (i < text.size() && text[i].isSpace()) ++i;
                while (i < text.size() && identifierPart(text[i])) ++i;
                setFormat(start, i - start, color(Directive)); continue;
            }
            if (ch != '.' && ch != ':') previousWord.clear();
            ++i;
        }
        if (mode != Semantic) return;
        QMap<int, Role> roles; QSet<int> ambiguous;
        static const QRegularExpression declarationBoundary("[;={}()\\\"']");
        static const QRegularExpression referenceBoundary("[^\\p{L}\\p{N}_$\\s.:>\\-]");
        for (const auto &mark : marks.value(currentBlock().blockNumber())) {
            const Role role = symbolRole(mark.value("kind").toString());
            if (role == Plain) continue;
            const QString name = mark.value("name").toString();
            int column = mark.value("col").toInt() - 1;
            if (mark.value("language") == "c++") {
                // libclang columns are UTF-8 bytes; Qt and the other backends use UTF-16.
                const auto bytes = text.toUtf8();
                if (column < 0 || column > bytes.size() || (column < bytes.size() && (uchar(bytes[column]) & 0xc0) == 0x80)) continue;
                column = int(QString::fromUtf8(bytes.left(column)).size());
            }
            if (column < 0 || column >= text.size()) continue;
            if (format(column).foreground().color() == color(Comment) || format(column).foreground().color() == color(String)) continue;
            for (const auto &token : tokens) {
                if (token.start < column) continue;
                const QString gap = text.mid(column, token.start - column);
                // AST declaration locations may start at modifiers/def/class; occurrence
                // locations may start at the receiver of a.b(). Never search the whole file.
                const bool declaration = mark.value("declaration").toBool();
                if (gap.contains(declaration ? declarationBoundary : referenceBoundary)) break;
                if (token.word != name) continue;
                if (token.role == Keyword || token.role == Control) break;
                if (roles.contains(token.start) && roles.value(token.start) != role) ambiguous.insert(token.start);
                else roles[token.start] = role;
                break;
            }
        }
        for (const auto &token : tokens) if (roles.contains(token.start) && !ambiguous.contains(token.start))
            setFormat(token.start, token.length, color(roles.value(token.start)));
    }
private:
    struct State : QTextBlockUserData { QString end; Role role = String; bool doubled = false, escaped = true; };
    QString language;
    Mode mode = Semantic;
    QMap<int, QList<QJsonObject>> marks;
    static bool identifierStart(QChar c) { return c.isLetter() || c == '_' || c == '$'; }
    static bool identifierPart(QChar c) { return identifierStart(c) || c.isNumber() || c.category() == QChar::Mark_NonSpacing; }
    static QSet<QString> words(const char *text) { const auto list = QString::fromLatin1(text).split(' '); return QSet<QString>(list.begin(), list.end()); }
    static const QSet<QString> &typeContexts() {
        static const auto value = words("class struct interface enum namespace new extends implements"); return value;
    }
    const QSet<QString> &controls() const {
        static const auto python = words("if else elif for while break continue return yield try except finally raise with match case await async import from");
        static const auto script = words("if else for while do switch case default break continue return yield throw try catch finally await async import from export");
        static const auto native = words("if else for foreach while do switch case default break continue return yield throw try catch finally await async using import package co_await co_return co_yield");
        return language == "python" ? python : language == "javascript" ? script : native;
    }
    const QSet<QString> &keywords() const {
        static const auto python = words("class def None True False is in not and or as pass lambda global nonlocal assert del self cls");
        static const auto script = words("class function var let const static public private protected abstract readonly new delete this super null true false in of as extends implements instanceof void typeof interface type enum namespace declare keyof infer is satisfies");
        static const auto native = words("class struct interface enum namespace const static public private protected internal virtual override abstract final sealed readonly volatile mutable extern inline constexpr consteval constinit template typename typedef operator new delete this base super null nullptr true false is in not and or as extends implements instanceof synchronized native transient throws void auto dynamic get set init required out ref params sizeof typeof noexcept friend union record restrict unsigned signed long short");
        return language == "python" ? python : language == "javascript" ? script : native;
    }
    const QSet<QString> &types() const {
        static const auto python = words("int float complex bool str bytes bytearray list tuple dict set frozenset object type");
        static const auto script = words("number string boolean bigint symbol any unknown never object");
        static const auto native = words("int float double bool boolean byte sbyte char wchar_t char8_t char16_t char32_t decimal string String object Object uint ulong ushort int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t");
        return language == "python" ? python : language == "javascript" ? script : native;
    }
};
