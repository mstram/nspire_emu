#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QTextBlock>
#include <QMessageBox>
#include <QGraphicsItem>
#include <QDropEvent>
#include <QMimeData>

#ifdef Q_OS_MAC
#include "os/os-mac.h"
#endif

MainWindow *main_window;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    emu_thread = &emu;

    ui->setupUi(this);

    connect(&refresh_timer, SIGNAL(timeout()), this, SLOT(refresh()));

    //Emu -> GUI (QueuedConnection as they're different threads)
    connect(&emu, SIGNAL(serialChar(char)), this, SLOT(serialChar(char)), Qt::QueuedConnection);
    connect(&emu, SIGNAL(debugStr(QString)), this, SLOT(debugStr(QString)), Qt::QueuedConnection);
    connect(&emu, SIGNAL(statusMsg(QString)), ui->statusbar, SLOT(showMessage(QString)), Qt::QueuedConnection);
    connect(&emu, SIGNAL(setThrottleTimer(bool)), this, SLOT(setThrottleTimer(bool)), Qt::QueuedConnection);
    connect(&emu, SIGNAL(usblinkChanged(bool)), this, SLOT(usblinkChanged(bool)), Qt::QueuedConnection);

    //Menu
    connect(ui->actionReset, SIGNAL(triggered()), &emu, SLOT(reset()));
    connect(ui->actionRestart, SIGNAL(triggered()), this, SLOT(restart()));
    connect(ui->actionDebugger, SIGNAL(triggered()), &emu, SLOT(enterDebugger()));
    connect(ui->actionPause, SIGNAL(toggled(bool)), &emu, SLOT(setPaused(bool)));
    connect(ui->actionSpeed, SIGNAL(triggered(bool)), this, SLOT(setThrottleTimerDeactivated(bool)));
    connect(ui->actionScreenshot, SIGNAL(triggered()), this, SLOT(screenshot()));
    connect(ui->actionConnect, SIGNAL(triggered()), this, SLOT(connectUSB()));

    //Debugging
    connect(ui->lineEdit, SIGNAL(returnPressed()), this, SLOT(debugCommand()));

    //Settings
    connect(ui->checkDebugger, SIGNAL(toggled(bool)), this, SLOT(setDebuggerOnStartup(bool)));
    connect(ui->checkWarning, SIGNAL(toggled(bool)), this, SLOT(setDebuggerOnWarning(bool)));
    connect(ui->checkAutostart, SIGNAL(toggled(bool)), this, SLOT(setAutostart(bool)));
    connect(ui->fileBoot1, SIGNAL(pressed()), this, SLOT(selectBoot1()));
    connect(ui->fileFlash, SIGNAL(pressed()), this, SLOT(selectFlash()));
    connect(ui->pathTransfer, SIGNAL(textEdited(QString)), this, SLOT(setUSBPath(QString)));
    connect(ui->spinGDB, SIGNAL(valueChanged(int)), this, SLOT(setGDBPort(int)));
    connect(ui->spinRDBG, SIGNAL(valueChanged(int)), this, SLOT(setRDBGPort(int)));

    refresh_timer.setInterval(1000 / 60); //60 fps
    refresh_timer.start();

    ui->lcdView->setScene(&lcd_scene);

#ifdef Q_OS_ANDROID
    //On android the settings file is deleted everytime you update or uninstall,
    //so choose a better, safer, location
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    settings = new QSettings(path + "/nspire_emu.ini", QSettings::IniFormat);
#else
    settings = new QSettings();
#endif

    //Load settings
    selectBoot1(settings->value("boot1", "").toString());
    selectFlash(settings->value("flash", "").toString());
    setDebuggerOnStartup(settings->value("debugOnStart", false).toBool());
    setDebuggerOnWarning(settings->value("debugOnWarn", false).toBool());
    setUSBPath(settings->value("usbdir", QString("ndless")).toString());
    setGDBPort(settings->value("gdbPort", 3333).toUInt());
    setRDBGPort(settings->value("rdbgPort", 3334).toUInt());

    bool autostart = settings->value("emuAutostart", false).toBool();
    setAutostart(autostart);
    if(emu.emu_path_boot1 != "" && emu.emu_path_flash != "" && autostart)
        emu.start();
}

MainWindow::~MainWindow()
{
    delete settings;
    delete ui;
}

extern "C"
{
#include "lcd.h"
#include "usblink.h"
}

void MainWindow::refresh()
{
    lcd_scene.clear();

    QByteArray framebuffer(320 * 240 * 2, 0);
    uint32_t bitfields[3];
    lcd_cx_draw_frame(reinterpret_cast<uint16_t*>(framebuffer.data()), bitfields);
    QImage::Format format = bitfields[0] == 0x00F ? QImage::Format_RGB444 : QImage::Format_RGB16;
    if(!emulate_cx)
    {
        format = QImage::Format_RGB444;
        uint16_t *px = reinterpret_cast<uint16_t*>(framebuffer.data());
        for(unsigned int i = 0; i < 320*240; ++i)
        {
            uint16_t n = *px << 8 | *px << 4 | *px;
            *px = ~n;
            ++px;
        }
    }
    QImage image(reinterpret_cast<const uchar*>(framebuffer.data()), 320, 240, 320 * 2, format);

    lcd_scene.addPixmap(QPixmap::fromImage(image));
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QMimeData* mime_data = e->mimeData();
    if(!mime_data->hasUrls())
        return;

    QUrl url = mime_data->urls().at(0).toLocalFile();

#ifdef Q_OS_MAC
    // For Mac OS X Yosemite...
    if (url.path().startsWith("/.file/id="))
        url = get_good_url_from_fileid_url("file://" + url.toString());
#endif

    usblink_put_file(url.toString().toStdString().c_str(), settings->value("usbdir", QString("ndless")).toString().toLocal8Bit().data());
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    e->accept();
}

void MainWindow::serialChar(const char c)
{
    ui->serialConsole->moveCursor(QTextCursor::End);

    static char previous = 0;

    switch(c)
    {
        case 0:

        case '\r':
            previous = c;
            break;

        case '\b':
            ui->serialConsole->textCursor().deletePreviousChar();
            break;

        default:
            if(c != '\n' && previous == '\r')
            {
                ui->serialConsole->moveCursor(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
                ui->serialConsole->moveCursor(QTextCursor::End, QTextCursor::KeepAnchor);
                ui->serialConsole->textCursor().removeSelectedText();
                previous = 0;
            }
            ui->serialConsole->insertPlainText(QString(c));
    }
}

void MainWindow::debugStr(QString str)
{
    ui->debugConsole->moveCursor(QTextCursor::End);
    ui->debugConsole->insertPlainText(str);

    ui->tabWidget->setCurrentWidget(ui->tabDebugger);
}

void MainWindow::debugCommand()
{
    debug_command = ui->lineEdit->text().toLatin1();
    emit debuggerCommand();
}

void MainWindow::selectBoot1(QString path)
{
    QFileInfo f(path);
    emu.emu_path_boot1 = path.toStdString();
    ui->filenameBoot1->setText(f.fileName());

    settings->setValue("boot1", path);
}

void MainWindow::selectBoot1()
{
    QFileInfo f(QString::fromStdString(emu.emu_path_flash));
    QString path = QFileDialog::getOpenFileName(this, "Select boot1 file", f.dir().absolutePath());
    if(!path.isNull())
        selectBoot1(path);
}

void MainWindow::selectFlash(QString path)
{
    QFileInfo f(path);
    emu.emu_path_flash = path.toStdString();
    ui->filenameFlash->setText(f.fileName());

    settings->setValue("flash", path);
}

void MainWindow::selectFlash()
{
    QFileInfo f(QString::fromStdString(emu.emu_path_flash));
    QString path = QFileDialog::getOpenFileName(this, "Select flash file", f.dir().absolutePath());
    if(!path.isNull())
        selectFlash(path);
}

void MainWindow::setDebuggerOnStartup(bool b)
{
    debug_on_start = b;
    settings->setValue("debugOnStart", b);
    if(ui->checkDebugger->isChecked() != b)
        ui->checkDebugger->setChecked(b);
}

void MainWindow::setDebuggerOnWarning(bool b)
{
    debug_on_warn = b;
    settings->setValue("debugOnWarn", b);
    if(ui->checkWarning->isChecked() != b)
        ui->checkWarning->setChecked(b);
}

void MainWindow::setAutostart(bool b)
{
    settings->setValue("emuAutostart", b);
    if(ui->checkAutostart->isChecked() != b)
        ui->checkAutostart->setChecked(b);
}

void MainWindow::setUSBPath(QString path)
{
    settings->setValue("usbdir", path);
    if(ui->pathTransfer->text() != path)
        ui->pathTransfer->setText(path);
}

void MainWindow::setGDBPort(int port)
{
    settings->setValue("gdbPort", port);
    emu_thread->port_gdb = port;
    //valueChanged signal will only be emitted if the value actually changed
    ui->spinGDB->setValue(port);
}

void MainWindow::setRDBGPort(int port)
{
    settings->setValue("rdbgPort", port);
    emu_thread->port_rdbg = port;
    //valueChanged signal will only be emitted if the value actually changed
    ui->spinRDBG->setValue(port);
}

void MainWindow::showSpeed(double percent)
{
    ui->actionSpeed->setText(tr("Speed: %1 %").arg(percent, 1, 'f', 0));
    ui->actionSpeed->setChecked(!throttle_timer.isActive());
}

void MainWindow::setThrottleTimerDeactivated(bool b)
{
    setThrottleTimer(!b);
}

void MainWindow::screenshot()
{
    QImage image(320, 240, QImage::Format_RGB16);
    QPainter painter(&image);
    ui->lcdView->scene()->render(&painter);

    QString filename = QFileDialog::getSaveFileName(this, tr("Save Screenshot"), QString(), "PNG images (*.png)");
    if(filename.isNull())
        return;

    if(!image.save(filename, "PNG"))
        QMessageBox::critical(this, tr("Screenshot failed"), tr("Failed to save screenshot!"));
}

void MainWindow::connectUSB()
{
    if(usblink_connected)
        usblink_reset();
    else
        usblink_connect();

    usblinkChanged(false);
}

void MainWindow::usblinkChanged(bool state)
{
    ui->actionConnect->setText(state ? tr("Disconnect USB") : tr("Connect USB"));
    ui->actionConnect->setChecked(state);
}

void MainWindow::setThrottleTimer(bool b)
{
    if(b)
    {
        throttle_timer.setInterval(throttle_delay);
        throttle_timer.start();
    }
    else
    {
        throttle_timer.stop();
        //We abuse a signal here to quit the event loop whenever we want
        throttle_timer.setObjectName(throttle_timer.objectName().isEmpty() ? "throttle_timer" : "");
    }
}

void MainWindow::throttleTimerWait()
{
    if(!throttle_timer.isActive())
        return;

    QEventLoop e;
    connect(&throttle_timer, SIGNAL(timeout()), &e, SLOT(quit()));
    connect(&throttle_timer, SIGNAL(objectNameChanged(QString)), &e, SLOT(quit()));
    e.exec();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    qDebug("Terminating emulator thread...");

    if(emu.stop())
        qDebug("Successful!");
    else
        qDebug("Failed.");

    QMainWindow::closeEvent(e);
}

void MainWindow::restart()
{
    if(emu.stop())
        emu.start();
    else
        debugStr("Failed to restart emulator. Close and reopen this app.\n");
}
