// Copyright (c) 2015, Axel Gembe <axel@gembe.net>
// All rights reserved.

// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:

// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice, this
//   list of conditions and the following disclaimer in the documentation and/or other
//   materials provided with the distribution.
// * The name of the contributors may not be used to endorse or promote products
//   derived from this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.

#include "QsLogDestWindow.h"
#include "QsLogMessage.h"
#include "QsLog.h"

#include "ui_QsLogDestWindow.h"

#include <QAbstractTableModel>
#include <QString>
#include <QReadWriteLock>
#include <QSortFilterProxyModel>
#include <QClipboard>
#include <QKeyEvent>
#include <QFileDialog>

#include <deque>

class Resources
{
public:
    Resources()
    {
        initialized = false;
    }

    void init()
    {
        if (initialized)
            return;

        pauseIcon.addFile(QStringLiteral(":/QsLogDestWindow/images/icon-pause-16.png"));
        playIcon.addFile(QStringLiteral(":/QsLogDestWindow/images/icon-resume-16.png"));

        initialized = true;
    }

    bool initialized;
    QIcon pauseIcon;
    QIcon playIcon;
};

static Resources g_resources;

class QsLogging::WindowLogDataModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    WindowLogDataModel(QObject* parent, size_t max_items = std::numeric_limits<size_t>::max()) :
        QAbstractTableModel(parent),
        max_items_(max_items)
    {
    }

    virtual int columnCount(const QModelIndex& parent = QModelIndex()) const
    {
        Q_UNUSED(parent);
        return 3;
    }

    virtual int rowCount(const QModelIndex& parent = QModelIndex()) const
    {
        Q_UNUSED(parent);
        QReadLocker lock(&data_lock_);

        return static_cast<int>(data_.size());
    }

    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const
    {
        if (!index.isValid())
            return QVariant();

        if (role == Qt::DisplayRole) {
            QReadLocker lock(&data_lock_);

            const LogMessage& item = data_.at(index.row());

            switch (index.column()) {
            case 0:
                return item.time.toLocalTime().toString();
            case 1:
                return LevelName(item.level);
            case 2:
                return item.message;
            case 100:
                return item.formatted;
            default:
                return QVariant();
            }

            return QString();
        }

        if (role == Qt::BackgroundColorRole) {
            QReadLocker lock(&data_lock_);

            const LogMessage& item = data_.at(index.row());

            switch (item.level)
            {
            case QsLogging::WarnLevel:
                return QVariant(QColor(255, 255, 128));
            case QsLogging::ErrorLevel:
                return QVariant(QColor(255, 128, 128));
            case QsLogging::FatalLevel:
                return QVariant(QColor(255, 0, 0));
            default:
                break;
            }
        }

        return QVariant();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const
    {
        if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
            switch (section) {
            case 0: return tr("Time");
            case 1: return tr("Level");
            case 2: return tr("Message");
            default: return QVariant();
            }
        }

        return QVariant();
    }

    void addEntry(const LogMessage& message)
    {
        const int next_idx = static_cast<int>(data_.size());
        beginInsertRows(QModelIndex(), next_idx, next_idx);
        {
            QWriteLocker lock(&data_lock_);
            data_.push_back(message);
        }
        endInsertRows();

        if (max_items_ < std::numeric_limits<size_t>::max() && data_.size() > max_items_) {
            {
                QWriteLocker lock(&data_lock_);
                data_.pop_front();
            }
            /* Every item changed */
            const QModelIndex idx1 = index(0, 0);
            const QModelIndex idx2 = index(static_cast<int>(data_.size()), rowCount());
            emit dataChanged(idx1, idx2);
        }
    }

    void clear()
    {
        beginResetModel();
        {
            QWriteLocker lock(&data_lock_);
            data_.clear();
        }
        endResetModel();
    }

    LogMessage at(size_t index)
    {
        return data_[index];
    }

private:
    std::deque<LogMessage> data_;
    mutable QReadWriteLock data_lock_;
    size_t max_items_;
};

class QsLogging::WindowLogFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    WindowLogFilterProxyModel(Level level, QObject *parent = 0) :
        QSortFilterProxyModel(parent),
        level_(level)
    {
    }

    Level level() const
    {
        return level_;
    }

    void setLevel(const Level level)
    {
        level_ = level;
        invalidateFilter();
    }

protected:
    virtual bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
    {
        Q_UNUSED(source_parent);
        WindowLogDataModel* model = dynamic_cast<WindowLogDataModel*>(sourceModel());
        const LogMessage& d = model->at(source_row);
        return (d.level >= level_);
    }

private:
    Level level_;
};

const char* const QsLogging::WindowDestination::Type = "window";

QsLogging::WindowDestination::WindowDestination()
{
    ui_ = new Ui::LogWindow();
    ui_->setupUi(this);

    g_resources.init();

    paused_ = false;
    auto_scroll_ = true;

    model_ = new WindowLogDataModel(this, 1024);

    connect(ui_->toolButtonPause, SIGNAL(clicked()), SLOT(OnPauseClicked()));
    connect(ui_->toolButtonSave, SIGNAL(clicked()), SLOT(OnSaveClicked()));
    connect(ui_->toolButtonClear, SIGNAL(clicked()), SLOT(OnClearClicked()));
    connect(ui_->toolButtonCopy, SIGNAL(clicked()), SLOT(OnCopyClicked()));
    connect(ui_->comboBoxLevel, SIGNAL(currentIndexChanged(int)), SLOT(OnLevelChanged(int)));
    connect(ui_->checkBoxAutoScroll, SIGNAL(toggled(bool)), SLOT(OnAutoScrollChanged(bool)));
    connect(model_, SIGNAL(rowsInserted(const QModelIndex&, int, int)), SLOT(ModelRowsInserted(const QModelIndex&, int, int)));

    /* Install the sort / filter model */
    sort_filter_ = new WindowLogFilterProxyModel(InfoLevel, this);
    sort_filter_->setSourceModel(model_);
    ui_->tableViewMessages->setModel(sort_filter_);

    ui_->tableViewMessages->installEventFilter(this);

    ui_->tableViewMessages->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui_->tableViewMessages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui_->tableViewMessages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui_->tableViewMessages->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui_->tableViewMessages->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    /* Initialize log level selection */
    for (int l = TraceLevel; l < OffLevel; l++) {
        const QString ln = LevelName(static_cast<Level>(l));
        ui_->comboBoxLevel->addItem(ln, l);
    }
    ui_->comboBoxLevel->setCurrentIndex(InfoLevel);
}

QsLogging::WindowDestination::~WindowDestination()
{
    delete ui_;
}

void QsLogging::WindowDestination::write(const LogMessage& message)
{
    model_->addEntry(message);
}

bool QsLogging::WindowDestination::isValid()
{
    return true;
}

QString QsLogging::WindowDestination::type() const
{
    return QString::fromLatin1(Type);
}

bool QsLogging::WindowDestination::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui_->tableViewMessages) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_C && (keyEvent->modifiers() & Qt::ControlModifier)) {
                copySelection();
                return true;
            }
        }

        return false;
    } else {
        return QDialog::eventFilter(obj, event);
    }
}

void QsLogging::WindowDestination::OnPauseClicked()
{
    ui_->toolButtonPause->setIcon(paused_ ? g_resources.pauseIcon : g_resources.playIcon);
    ui_->toolButtonPause->setText(paused_ ? tr("&Pause") : tr("&Resume"));

    paused_ = !paused_;

    ui_->tableViewMessages->setUpdatesEnabled(!paused_);
}

void QsLogging::WindowDestination::OnSaveClicked()
{
    saveSelection();
}

void QsLogging::WindowDestination::OnClearClicked()
{
    model_->clear();
}

void QsLogging::WindowDestination::OnCopyClicked()
{
    copySelection();
}

void QsLogging::WindowDestination::OnLevelChanged(int value)
{
    sort_filter_->setLevel(static_cast<Level>(value));
}

void QsLogging::WindowDestination::OnAutoScrollChanged(bool checked)
{
    auto_scroll_ = checked;
}

void QsLogging::WindowDestination::ModelRowsInserted(const QModelIndex& parent, int start, int last)
{
    Q_UNUSED(parent);
    Q_UNUSED(start);
    Q_UNUSED(last);
    if (auto_scroll_)
        ui_->tableViewMessages->scrollToBottom();
}

void QsLogging::WindowDestination::copySelection() const
{
    const QString text = getSelectionText();
    if (text.isEmpty())
        return;

    QApplication::clipboard()->setText(text);
}

void QsLogging::WindowDestination::saveSelection()
{
    const QString text = getSelectionText();
    if (text.isEmpty())
        return;

    QFileDialog dialog(this);
    dialog.setWindowTitle(tr("Save log"));
    dialog.setNameFilter(tr("Log file (*.log)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("log");
    dialog.exec();

    const QStringList sel = dialog.selectedFiles();
    if (sel.size() < 1)
        return;

    QFile file(sel.at(0));
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << text;
        file.close();
    }
}

QString QsLogging::WindowDestination::getSelectionText() const
{
    QModelIndexList rows = ui_->tableViewMessages->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end());

    QString text;

    if (rows.count() == 0) {
        for (int i = 0; i < sort_filter_->rowCount(); i++) {
            const int srow = sort_filter_->mapToSource(sort_filter_->index(i, 0)).row();
            text += model_->at(srow).formatted + "\n";
        }
    } else {
        for (QModelIndexList::const_iterator i = rows.begin();i != rows.end();++i) {
            const int srow = sort_filter_->mapToSource(*i).row();
            text += model_->at(srow).formatted + "\n";
        }
    }

    return text;
}

#include "QsLogDestWindow.moc"
