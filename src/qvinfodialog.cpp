#include "qvinfodialog.h"
#include "ui_qvinfodialog.h"
#include "qvimagemetadata.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeDatabase>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

namespace {

constexpr int kShortValueMaxChars = 72;
constexpr int kPreviewMaxChars = 30;
constexpr int kMaxSectionHeight = 260;
constexpr int kMaxSectionWidth = 460;

int getGcd(int a, int b)
{
    return (b == 0) ? a : getGcd(b, a % b);
}

bool isLongValue(const QString &value)
{
    return value.length() > kShortValueMaxChars || value.contains(QLatin1Char('\n'))
            || value.contains(QLatin1Char('\r'));
}

QString makeSingleLinePreview(const QString &value)
{
    QString collapsed;
    collapsed.reserve(kPreviewMaxChars + 2);
    bool lastWasSpace = true;

    for (int i = 0; i < value.length() && collapsed.length() < kPreviewMaxChars; ++i) {
        const QChar ch = value.at(i);
        if (ch.isSpace()) {
            if (!lastWasSpace) {
                collapsed.append(QLatin1Char(' '));
                lastWasSpace = true;
            }
        } else {
            collapsed.append(ch);
            lastWasSpace = false;
        }
    }

    collapsed = collapsed.trimmed();
    if (collapsed.isEmpty())
        return QStringLiteral("…");
    return collapsed + QStringLiteral("…");
}

QString sanitizeFilePart(const QString &input, int maxLen)
{
    QString out;
    out.reserve(qMin(input.length(), maxLen));
    for (int i = 0; i < input.length() && out.length() < maxLen; ++i) {
        const QChar ch = input.at(i);
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            out.append(ch);
        else
            out.append(QLatin1Char('_'));
    }
    while (out.startsWith(QLatin1Char('_')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("data") : out;
}

} // namespace

class QVMetadataSection : public QWidget
{
public:
    QVMetadataSection(const QString &title,
                      const std::function<void(const QString &, const QString &)> &onOpenInEditor,
                      const std::function<void()> &onToggle, QWidget *parent = nullptr)
        : QWidget(parent), baseTitle(title), openCallback(onOpenInEditor), toggleCallback(onToggle)
    {
        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 4, 0, 0);
        rootLayout->setSpacing(4);

        auto *divider = new QFrame(this);
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Sunken);
        rootLayout->addWidget(divider);

        toggleButton = new QToolButton(this);
        toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggleButton->setArrowType(Qt::RightArrow);
        toggleButton->setIconSize(QSize(9, 9));
        toggleButton->setCheckable(true);
        toggleButton->setChecked(false);
        toggleButton->setAutoRaise(true);
        toggleButton->setCursor(Qt::PointingHandCursor);
        toggleButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        toggleButton->setStyleSheet(
                QStringLiteral("QToolButton { border: none; padding: 2px 0px; text-align: left; }"));
        rootLayout->addWidget(toggleButton);

        scrollArea = new QScrollArea(this);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->viewport()->setAutoFillBackground(false);
        scrollArea->setVisible(false);
        rootLayout->addWidget(scrollArea);

        QObject::connect(toggleButton, &QToolButton::toggled, this, [this](bool checked) {
            toggleButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
            scrollArea->setVisible(checked);
            if (toggleCallback)
                toggleCallback();
        });

        setVisible(false);
    }

    void setEntries(const QList<QVMetadataEntry> &entries)
    {
        if (entries.isEmpty()) {
            delete scrollArea->takeWidget();
            setVisible(false);
            return;
        }

        toggleButton->setText(QStringLiteral("%1 (%2)").arg(baseTitle).arg(entries.size()));

        auto *content = new QWidget;
        content->setAutoFillBackground(false);

        auto *form = new QFormLayout(content);
        form->setContentsMargins(14, 2, 8, 2);
        form->setVerticalSpacing(4);
        form->setHorizontalSpacing(10);

        for (const QVMetadataEntry &entry : entries) {
            auto *keyLabel = new QLabel(entry.key + QLatin1Char(':'), content);
            keyLabel->setTextFormat(Qt::PlainText);

            if (!isLongValue(entry.value)) {
                auto *valueLabel = new QLabel(entry.value, content);
                valueLabel->setTextFormat(Qt::PlainText);
                valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                valueLabel->setCursor(Qt::IBeamCursor);
                form->addRow(keyLabel, valueLabel);
            } else {
                auto *rowWidget = new QWidget(content);
                auto *rowLayout = new QHBoxLayout(rowWidget);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(6);

                auto *previewLabel = new QLabel(makeSingleLinePreview(entry.value), rowWidget);
                previewLabel->setTextFormat(Qt::PlainText);
                QPalette previewPal = previewLabel->palette();
                QColor muted = previewPal.color(QPalette::WindowText);
                muted.setAlphaF(0.65);
                previewPal.setColor(QPalette::WindowText, muted);
                previewLabel->setPalette(previewPal);

                auto *openButton = new QPushButton(
                        QCoreApplication::translate("QVInfoDialog", "Open in Editor"), rowWidget);
                openButton->setAutoDefault(false);
                openButton->setDefault(false);
                openButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
                openButton->setStyleSheet(QStringLiteral("QPushButton { padding: 2px 8px; }"));

                const bool json = isJsonMetadataValue(entry.value);
                const qint64 byteCount = entry.value.toUtf8().size();
                openButton->setToolTip(
                        QCoreApplication::translate("QVInfoDialog",
                                                    "Open as temporary %1 file (%2)")
                                .arg(json ? QStringLiteral(".json") : QStringLiteral(".txt"),
                                     QVInfoDialog::formatBytes(byteCount)));

                const QString keyCopy = entry.key;
                const QString valueCopy = entry.value;
                QObject::connect(openButton, &QPushButton::clicked, openButton,
                                 [this, keyCopy, valueCopy]() {
                                     if (openCallback)
                                         openCallback(keyCopy, valueCopy);
                                 });

                rowLayout->addWidget(previewLabel);
                rowLayout->addWidget(openButton);
                rowLayout->addStretch(1);
                form->addRow(keyLabel, rowWidget);
            }
        }

        scrollArea->setWidget(content);
        content->adjustSize();

        const QSize contentHint = content->sizeHint();
        const int sbExtent =
                scrollArea->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scrollArea);
        const bool needsVScroll = contentHint.height() > kMaxSectionHeight;
        const bool needsHScroll = contentHint.width() > kMaxSectionWidth;

        const int targetH =
                qMin(contentHint.height() + (needsHScroll ? sbExtent : 0), kMaxSectionHeight);
        const int targetW =
                qMin(contentHint.width() + (needsVScroll ? sbExtent + 12 : 12), kMaxSectionWidth);

        scrollArea->setFixedHeight(targetH);
        scrollArea->setMinimumWidth(targetW);
        scrollArea->setVisible(toggleButton->isChecked());
        setVisible(true);
    }

private:
    QString baseTitle;
    std::function<void(const QString &, const QString &)> openCallback;
    std::function<void()> toggleCallback;
    QToolButton *toggleButton;
    QScrollArea *scrollArea;
};

QVInfoDialog::QVInfoDialog(QWidget *parent) : QDialog(parent), ui(new Ui::QVInfoDialog)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() & (~Qt::WindowContextHelpButtonHint | Qt::CustomizeWindowHint));
    ui->verticalLayout->setSizeConstraint(QLayout::SetFixedSize);

    width = 0;
    height = 0;
    frameCount = 0;

    auto onOpen = [this](const QString &key, const QString &value) {
        openMetadataInEditor(key, value);
    };
    auto onToggle = [this]() {
        if (layout())
            layout()->activate();
        window()->adjustSize();
    };

    textSection = new QVMetadataSection(tr("PNG Text"), onOpen, onToggle, this);
    exifSection = new QVMetadataSection(tr("EXIF"), onOpen, onToggle, this);
    ui->verticalLayout->addWidget(textSection);
    ui->verticalLayout->addWidget(exifSection);
}

QVInfoDialog::~QVInfoDialog()
{
    delete ui;
}

void QVInfoDialog::setInfo(const QFileInfo &value, const int &value2, const int &value3,
                           const int &value4)
{
    selectedFileInfo = value;
    width = value2;
    height = value3;
    frameCount = value4;

    // If the dialog is visible, it means we've just navigated to a new image. Instead of running
    // updateInfo immediately, add it to the event queue. This is a workaround for a
    // (Windows-specific?) delay when calling adjustSize on the window if the font contains certain
    // characters (e.g. Chinese) the first time that happens for a given font. At least on Windows,
    // by making the work happen later in the event loop, it allows the main window to repaint
    // first, giving the appearance of better responsiveness. If the dialog is not visible, however,
    // it means we're preparing to display for an image already opened. In this case there is no
    // urgency to repaint the main window, and we need to process the updates here synchronously to
    // avoid the caller showing the dialog before it's ready (i.e. to avoid showing outdated info or
    // placeholder text).
    if (isVisible())
        QTimer::singleShot(0, this, &QVInfoDialog::updateInfo);
    else
        updateInfo();
}

void QVInfoDialog::updateInfo()
{
    QLocale locale = QLocale::system();
    QMimeDatabase mimedb;
    QMimeType mime = mimedb.mimeTypeForFile(selectedFileInfo.absoluteFilePath(),
                                            QMimeDatabase::MatchContent);
    // this is just math to figure the megapixels and then round it to the tenths place
    const double megapixels =
            static_cast<double>(
                    qRound(((static_cast<double>((width * height)))) / 1000000 * 10 + 0.5))
            / 10;

    ui->nameLabel->setText(selectedFileInfo.fileName());
    ui->typeLabel->setText(mime.name());
    ui->locationLabel->setText(selectedFileInfo.path());
    ui->sizeLabel->setText(tr("%1 (%2 bytes)")
                                   .arg(formatBytes(selectedFileInfo.size()),
                                        locale.toString(selectedFileInfo.size())));
    ui->modifiedLabel->setText(selectedFileInfo.lastModified().toString(locale.dateTimeFormat()));
    ui->dimensionsLabel->setText(tr("%1 x %2 (%3 MP)")
                                         .arg(QString::number(width), QString::number(height),
                                              QString::number(megapixels)));
    int gcd = getGcd(width, height);
    if (gcd != 0)
        ui->ratioLabel->setText(QString::number(width / gcd) + ":" + QString::number(height / gcd));
    if (frameCount != 0) {
        ui->framesLabel2->show();
        ui->framesLabel->show();
        ui->framesLabel->setText(QString::number(frameCount));
    } else {
        ui->framesLabel2->hide();
        ui->framesLabel->hide();
    }

    const QVImageMetadata metadata = readImageMetadata(selectedFileInfo.absoluteFilePath());
    textSection->setEntries(metadata.textEntries);
    exifSection->setEntries(metadata.exifEntries);

    if (layout())
        layout()->activate();
    window()->adjustSize();
}

void QVInfoDialog::openMetadataInEditor(const QString &key, const QString &value)
{
    const bool isJson = isJsonMetadataValue(value);
    const QString extension = isJson ? QStringLiteral("json") : QStringLiteral("txt");
    const QString tempDirPath = QDir::tempPath() + QStringLiteral("/qView-metadata");
    QDir().mkpath(tempDirPath);

    const QString basePart = sanitizeFilePart(selectedFileInfo.completeBaseName(), 32);
    const QString keyPart = sanitizeFilePart(key, 32);
    const QString fileTemplate = QStringLiteral("%1/%2_%3_XXXXXX.%4")
                                         .arg(tempDirPath, basePart, keyPart, extension);

    QTemporaryFile tempFile(fileTemplate);
    tempFile.setAutoRemove(false);
    if (!tempFile.open())
        return;

    const QString contentToWrite = isJson ? formatJsonMetadataValue(value, 1) : value;
    tempFile.write(contentToWrite.toUtf8());
    tempFile.flush();
    const QString path = tempFile.fileName();
    tempFile.close();

    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
