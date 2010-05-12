#include "ChatEdit.h"
#include "WulforUtil.h"

#include "dcpp/HashManager.h"

#include <QCompleter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QUrl>
#include <QFileInfo>

ChatEdit::ChatEdit(QWidget *parent) : QPlainTextEdit(parent), cc(NULL)
{}

ChatEdit::~ChatEdit()
{}

void ChatEdit::setCompleter(QCompleter *completer, UserListModel *model)
{
    if (cc)
        QObject::disconnect(cc, 0, this, 0);

    cc = completer;

    if (!cc || !model)
        return;

    cc->setWidget(this);
    cc->setWrapAround(false);
    cc->setCaseSensitivity(Qt::CaseInsensitive);
    cc->setCompletionMode(QCompleter::PopupCompletion);

    cc_model = model;

    QObject::connect(cc, SIGNAL(activated(const QModelIndex&)),
                     this, SLOT(insertCompletion(const QModelIndex&)));
}

void ChatEdit::insertCompletion(const QModelIndex & index)
{
    if (cc->widget() != this || !index.isValid())
        return;

    QString nick = cc->completionModel()->index(index.row(), index.column()).data().toString();
    int begin = textCursor().position() - cc->completionPrefix().length();

    insertToPos(nick, begin);
}

void ChatEdit::insertToPos(const QString & completeText, int begin)
{
    if (completeText.isEmpty())
        return;

    if (begin < 0)
        begin = 0;

    QTextCursor cursor = textCursor();
    int end = cursor.position();
    cursor.setPosition(begin);
    cursor.setPosition(end, QTextCursor::KeepAnchor);

    if (!begin)
        cursor.insertText(completeText + ": ");
    else
        cursor.insertText(completeText + " ");

    setTextCursor(cursor);
}

QString ChatEdit::textUnderCursor() const
{
    QTextCursor cursor = textCursor();

    int curpos = cursor.position();
    QString text = cursor.block().text().left(curpos);

    QStringList wordList = text.split(QRegExp("\\s"));

    if (wordList.isEmpty())
        return QString();

    return wordList.last();
}

void ChatEdit::focusInEvent(QFocusEvent *e)
{
    if (cc)
        cc->setWidget(this);

    QPlainTextEdit::focusInEvent(e);
}

void ChatEdit::keyPressEvent(QKeyEvent *e)
{
    const bool ctrlOrShift = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    bool hasModifier = (e->modifiers() != Qt::NoModifier) &&
                       (e->modifiers() != Qt::KeypadModifier) &&
                       !ctrlOrShift;

    if (e->key() == Qt::Key_Tab) {
        if (!toPlainText().isEmpty()) {
            if (cc && cc->popup()->isVisible()) {
                int row = cc->popup()->currentIndex().row() + 1;
                if (cc->completionModel()->rowCount() == row)
                    row = 0;
                cc->popup()->setCurrentIndex(cc->completionModel()->index(row, 0));
            }
            e->accept();
        } else {
            e->ignore();
        }
        return;
    }

    if (cc && cc->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            e->ignore();
            return;
        default:
            break;
        }
    }

    if (!cc || !cc->popup()->isVisible() || !hasModifier)
        QPlainTextEdit::keyPressEvent(e);

    if (ctrlOrShift && e->text().isEmpty())
        return;

    if (cc->popup()->isVisible() && (hasModifier || e->text().isEmpty())) {
        cc->popup()->hide();
        return;
    }

    if (cc->popup()->isVisible())
        complete();
}

void ChatEdit::keyReleaseEvent(QKeyEvent *e)
{
    bool hasModifier = (e->modifiers() != Qt::NoModifier);

    switch (e->key()) {
    case Qt::Key_Tab:
        if (cc && !hasModifier && !cc->popup()->isVisible())
            complete();

    case Qt::Key_Enter:
    case Qt::Key_Return:
        e->ignore();
        return;
    default:
        break;
    }
}

void ChatEdit::complete()
{
    QString completionPrefix = textUnderCursor();

    if (completionPrefix.isEmpty()) {
        if (cc->popup()->isVisible())
            cc->popup()->hide();

        return;
    }

    if (!cc->popup()->isVisible() || completionPrefix.length() < cc->completionPrefix().length()) {
        QString pattern = QString("(\\[.*\\])?%1.*").arg( QRegExp::escape(completionPrefix) );
        QStringList nicks = cc_model->findItems(pattern, Qt::MatchRegExp, 0);

        if (nicks.isEmpty())
            return;

        if (nicks.count() == 1) {
            insertToPos(nicks.last(), textCursor().position() - completionPrefix.length());
            return;
        }

        NickCompletionModel *tmpModel = new NickCompletionModel(nicks, cc);
        cc->setModel(tmpModel);
    }

    if (completionPrefix != cc->completionPrefix()) {
        cc->setCompletionPrefix(completionPrefix);
        cc->popup()->setCurrentIndex(cc->completionModel()->index(0, 0));
    }

    QRect cr = cursorRect();
    cr.setWidth(cc->popup()->sizeHintForColumn(0)
                + cc->popup()->verticalScrollBar()->sizeHint().width());

    cc->complete(cr);
}

void ChatEdit::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText()) {
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void ChatEdit::dropEvent(QDropEvent *e)
{
    if (e->mimeData()->hasUrls()) {

        e->setDropAction(Qt::IgnoreAction);

        QStringList fileNames;
        foreach (QUrl url, e->mimeData()->urls()) {
            QString urlStr = url.toString();
            do {
                if (url.scheme().toLower() != "file")
                    break;

                QString str = url.toLocalFile();

                if (!str.isEmpty()) {
                    QFileInfo fi(str);
                    if (!fi.isFile())
                        break;

                    const TTHValue *tth = HashManager::getInstance()->getFileTTHif(str.toStdString());
                    if (tth != NULL)
                        urlStr = WulforUtil::getInstance()->makeMagnet(fi.fileName(), fi.size(), _q(tth->toBase32()));
                }
            } while(false);

            if (!urlStr.isEmpty())
                fileNames << urlStr;
        }

        if (!fileNames.isEmpty()) {

            QString dropText = (fileNames.count() == 1) ? fileNames.last() : "\n" + fileNames.join("\n");

            QMimeData mime;
            mime.setText(dropText);
            QDropEvent drop(e->pos(), Qt::CopyAction, &mime, e->mouseButtons(),
                            e->keyboardModifiers(), e->type());

            QPlainTextEdit::dropEvent(&drop);
            return;
        }
    }
    QPlainTextEdit::dropEvent(e);
}
