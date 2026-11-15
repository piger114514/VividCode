#pragma once
#include "UiLanguage.h"
#include <QtWidgets>
#include "SourceHighlighter.h"

// 临时源码速览：窗外点击或切换到其它应用后关闭，没有编辑/保存入口。
class SourcePopup : public QWidget {
public:
    explicit SourcePopup(QWidget *parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint) {
        setObjectName("sourcePopup");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 4, 6, 6); layout->setSpacing(2);
        auto *bar = new QHBoxLayout;
        auto *mode = new QComboBox; mode->setObjectName("sourceHighlightMode");
        mode->addItems({UiLanguage::text("Semantic highlighting"), UiLanguage::text("Syntax highlighting"), UiLanguage::text("Plain text")});
        mode->setToolTip(UiLanguage::text("Types: teal; functions: yellow; variables: blue; parameters: light blue; members: purple.\nSemantic mode uses indexed locations when available; other tokens use syntax highlighting."));
        bar->addWidget(mode); bar->addStretch();
        auto *closeButton = new QToolButton;
        closeButton->setObjectName("sourcePopupClose"); closeButton->setText(QStringLiteral("×"));
        closeButton->setToolTip(UiLanguage::text("Close")); closeButton->setFixedSize(28, 26);
        bar->addWidget(closeButton); layout->addLayout(bar);
        code = new QPlainTextEdit;
        code->setObjectName("sourceViewer"); code->setReadOnly(true);
        code->setLineWrapMode(QPlainTextEdit::NoWrap); code->setFont(QFont("Consolas", 11));
        code->setContextMenuPolicy(Qt::NoContextMenu);
        highlighter = new SourceHighlighter(code->document());
        connect(mode, &QComboBox::currentIndexChanged, this, [this](int index) { highlighter->setMode(static_cast<SourceHighlighter::Mode>(index)); });
        layout->addWidget(code);
        connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(escape, &QShortcut::activated, this, &QWidget::close);
        qApp->installEventFilter(this);
        resize(760, 480);
    }
    void showCode(const QString &text, int line, int column, const QPoint &anchor,
                  const QString &path = {}, const QJsonArray &positions = {}) {
        ++generation;
        highlighter->configure(path, positions);
        code->setPlainText(text);
        highlighter->rehighlight();
        QTextCursor cursor(code->document()->findBlockByNumber(qMax(0, line - 1)));
        if (!cursor.isNull()) {
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                qBound(0, column - 1, qMax(0, cursor.block().length() - 1)));
            code->setTextCursor(cursor);
        }
        const auto *screen = QGuiApplication::screenAt(anchor);
        const QRect area = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
        resize(qMin(760, area.width() - 20), qMin(480, area.height() - 20));
        move(qBound(area.left(), anchor.x(), area.right() - width() + 1),
             qBound(area.top(), anchor.y(), area.bottom() - height() + 1));
        show(); raise(); activateWindow(); code->setFocus(); code->centerCursor();
    }
    QPlainTextEdit *viewer() const { return code; }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (isVisible() && event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const auto *widget = qobject_cast<QWidget *>(object);
            if (!frameGeometry().contains(mouse->globalPosition().toPoint()) && (!widget || !isAncestorOf(widget))) close();
        }
        return QWidget::eventFilter(object, event);
    }
    void hideEvent(QHideEvent *event) override {
        highlighter->configure({}, {}); code->clear(); QWidget::hideEvent(event);
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::WindowDeactivate && isVisible()) {
            const auto showing = generation;
            QTimer::singleShot(0, this, [this, showing] { if (generation == showing && isVisible() && QApplication::activeWindow() != this) close(); });
        }
        return QWidget::event(event);
    }
private:
    QPlainTextEdit *code;
    SourceHighlighter *highlighter;
    quint64 generation = 0;
};
