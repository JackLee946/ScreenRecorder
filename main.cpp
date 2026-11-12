// Qt5 录屏小工具：托盘常驻，基于 ffmpeg(gdigrab) 录制 MP4(H.264)
// 功能：全屏/区域录制、帧率选择、保存目录、ffmpeg 路径、双击托盘开始/停止
#include <QApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ---------------------------------------------------------------
// 区域选择遮罩：在主屏幕上拖拽出一个矩形，Esc / 右键取消
// ---------------------------------------------------------------
class RegionSelector : public QWidget
{
    Q_OBJECT
public:
    RegionSelector()
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::CrossCursor);
        m_origin = QGuiApplication::primaryScreen()->geometry().topLeft();
        setGeometry(QGuiApplication::primaryScreen()->geometry());
    }

signals:
    void selected(const QRect &globalRect);
    void canceled();

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_start = m_cur = e->pos();
            m_drag = true;
            update();
        } else {
            emit canceled();
            close();
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_drag) { m_cur = e->pos(); update(); }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_drag) return;
        m_drag = false;
        QRect r = QRect(m_start, e->pos()).normalized();
        if (r.width() < 16 || r.height() < 16) { update(); return; }   // 太小当作误操作
        emit selected(r.translated(m_origin));
        close();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) { emit canceled(); close(); }
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 110));
        if (!m_drag) {
            p.setPen(Qt::white);
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("拖拽选择录制区域    Esc / 右键取消"));
            return;
        }
        QRect r = QRect(m_start, m_cur).normalized();
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(r, QColor(0, 0, 0, 1));          // alpha=1：视觉透明但仍能接收鼠标
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(QColor(0, 174, 255), 2));
        p.drawRect(r);
        p.setPen(Qt::white);
        p.drawText(r.topLeft() + QPoint(4, -8),
                   QStringLiteral("%1 x %2").arg(r.width()).arg(r.height()));
    }

private:
    QPoint m_start, m_cur, m_origin;
    bool m_drag = false;
};

// ---------------------------------------------------------------
// 录屏控制 + 托盘
// ---------------------------------------------------------------
class Recorder : public QObject
{
    Q_OBJECT
public:
    Recorder()
    {
        m_tray = new QSystemTrayIcon(makeIcon(false), this);
        m_tray->setToolTip(QStringLiteral("录屏小工具（双击开始/停止）"));
        buildMenu();
        m_tray->setContextMenu(&m_menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::DoubleClick) toggle();
        });
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
            if (m_notifyOpensFile && !m_lastFile.isEmpty())
                showInFolder(m_lastFile);
        });
        m_tray->show();

        m_tick.setInterval(1000);
        connect(&m_tick, &QTimer::timeout, this, &Recorder::updateTooltip);
    }

private:
    // ---------- 设置 ----------
    QString saveDir() const
    {
        return m_set.value("saveDir",
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    }
    int fps() const { return m_set.value("fps", 25).toInt(); }
    bool regionMode() const { return m_set.value("regionMode", false).toBool(); }

    QString ffmpegPath() const
    {
        QString p = m_set.value("ffmpeg").toString();
        if (!p.isEmpty() && QFileInfo::exists(p)) return p;
        QString local = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
        if (QFileInfo::exists(local)) return local;
        return QStandardPaths::findExecutable("ffmpeg");
    }

    // ---------- 菜单 ----------
    void buildMenu()
    {
        m_actToggle = m_menu.addAction(QStringLiteral("开始录制"), this, &Recorder::toggle);
        m_menu.addSeparator();

        // 录制范围
        auto *modeGroup = new QActionGroup(this);
        QAction *aFull = m_menu.addAction(QStringLiteral("录制范围：全屏（所有显示器）"));
        QAction *aReg  = m_menu.addAction(QStringLiteral("录制范围：选择区域"));
        for (QAction *a : {aFull, aReg}) { a->setCheckable(true); modeGroup->addAction(a); }
        (regionMode() ? aReg : aFull)->setChecked(true);
        connect(aFull, &QAction::triggered, this, [this]() { m_set.setValue("regionMode", false); });
        connect(aReg,  &QAction::triggered, this, [this]() { m_set.setValue("regionMode", true); });

        // 帧率
        QMenu *fpsMenu = m_menu.addMenu(QStringLiteral("帧率"));
        auto *fpsGroup = new QActionGroup(this);
        for (int f : {10, 15, 25, 30, 60}) {
            QAction *a = fpsMenu->addAction(QStringLiteral("%1 fps").arg(f));
            a->setCheckable(true);
            a->setChecked(f == fps());
            fpsGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, f]() { m_set.setValue("fps", f); });
        }

        m_menu.addSeparator();
        m_menu.addAction(QStringLiteral("设置保存目录…"), this, &Recorder::chooseSaveDir);
        m_menu.addAction(QStringLiteral("设置 ffmpeg 路径…"), this, &Recorder::chooseFfmpeg);
        m_menu.addAction(QStringLiteral("打开保存目录"), this, [this]() {
            QDir().mkpath(saveDir());
            QDesktopServices::openUrl(QUrl::fromLocalFile(saveDir()));
        });
        m_menu.addSeparator();
        m_menu.addAction(QStringLiteral("退出"), this, &Recorder::quit);
    }

    void chooseSaveDir()
    {
        QString d = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("选择保存目录"), saveDir());
        if (!d.isEmpty()) m_set.setValue("saveDir", d);
    }
    void chooseFfmpeg()
    {
        QString f = QFileDialog::getOpenFileName(nullptr, QStringLiteral("选择 ffmpeg.exe"),
                                                 QString(), "ffmpeg (ffmpeg.exe);;All (*.*)");
        if (!f.isEmpty()) m_set.setValue("ffmpeg", f);
    }

    // ---------- 图标 ----------
    static QIcon makeIcon(bool recording)
    {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(recording ? QColor(230, 40, 40) : QColor(110, 110, 120));
        p.drawEllipse(QRect(3, 3, 26, 26));
        p.setBrush(Qt::white);
        if (recording) p.drawRect(QRect(11, 11, 10, 10));              // 停止方块
        else           p.drawEllipse(QRect(10, 10, 12, 12));           // 录制圆点
        return QIcon(pm);
    }

    // ---------- 录制流程 ----------
    void toggle()
    {
        if (m_proc) { stopRecording(); return; }
        if (m_selector) return;                       // 正在选区
        if (regionMode()) {
            m_selector = new RegionSelector();
            connect(m_selector, &RegionSelector::selected, this, [this](const QRect &r) {
                m_selector = nullptr;
                QTimer::singleShot(150, this, [this, r]() { startRecording(r); });  // 等遮罩消失
            });
            connect(m_selector, &RegionSelector::canceled, this, [this]() { m_selector = nullptr; });
            m_selector->show();
            m_selector->activateWindow();
            m_selector->setFocus();
        } else {
            startRecording(QRect());
        }
    }

    void startRecording(const QRect &region)
    {
        const QString ffmpeg = ffmpegPath();
        if (ffmpeg.isEmpty()) {
            notify(QStringLiteral("未找到 ffmpeg"),
                   QStringLiteral("请把 ffmpeg.exe 放到程序目录，或在菜单里设置 ffmpeg 路径"), false);
            return;
        }
        QDir().mkpath(saveDir());
        m_lastFile = QDir(saveDir()).filePath(
            "rec_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4");
        m_stderrTail.clear();

        QStringList args;
        args << "-y" << "-f" << "gdigrab"
             << "-framerate" << QString::number(fps())
             << "-draw_mouse" << "1";
        if (!region.isNull()) {
            args << "-offset_x" << QString::number(region.x())
                 << "-offset_y" << QString::number(region.y())
                 << "-video_size" << QString("%1x%2").arg(region.width()).arg(region.height());
        }
        args << "-i" << "desktop"
             << "-vf" << "scale=trunc(iw/2)*2:trunc(ih/2)*2"       // yuv420p 要求宽高为偶数
             << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-crf" << "23"
             << "-pix_fmt" << "yuv420p" << "-movflags" << "+faststart"
             << m_lastFile;

        m_proc = new QProcess(this);
#ifdef Q_OS_WIN
        m_proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
            a->flags |= CREATE_NO_WINDOW;                           // 不弹黑框
        });
#endif
        connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
            m_stderrTail += m_proc->readAllStandardError();
            if (m_stderrTail.size() > 2048) m_stderrTail = m_stderrTail.right(2048);
        });
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &Recorder::onFinished);
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
                notify(QStringLiteral("启动 ffmpeg 失败"), m_proc->errorString(), false);
                cleanupProc();
            }
        });

        m_proc->start(ffmpeg, args);
        m_elapsed.start();
        m_tick.start();
        m_tray->setIcon(makeIcon(true));
        m_actToggle->setText(QStringLiteral("停止录制"));
        updateTooltip();
        notify(QStringLiteral("开始录制"), QDir::toNativeSeparators(m_lastFile), false);
    }

    void stopRecording()
    {
        if (!m_proc) return;
        m_actToggle->setEnabled(false);
        m_proc->write("q\n");                          // 让 ffmpeg 正常收尾，MP4 才能播放
        m_proc->closeWriteChannel();
        QTimer::singleShot(8000, this, [this]() {      // 兜底：8 秒还没退就强杀
            if (m_proc) m_proc->kill();
        });
    }

    void onFinished(int code, QProcess::ExitStatus)
    {
        const bool ok = QFileInfo(m_lastFile).size() > 0 && code == 0;
        const QString tail = QString::fromLocal8Bit(m_stderrTail.right(400));
        cleanupProc();
        if (ok) notify(QStringLiteral("录制完成（点击打开所在目录）"),
                       QDir::toNativeSeparators(m_lastFile), true);
        else    notify(QStringLiteral("录制失败"), tail.trimmed(), false);
        if (m_quitAfter) qApp->quit();
    }

    void cleanupProc()
    {
        m_tick.stop();
        if (m_proc) { m_proc->deleteLater(); m_proc = nullptr; }
        m_tray->setIcon(makeIcon(false));
        m_tray->setToolTip(QStringLiteral("录屏小工具（双击开始/停止）"));
        m_actToggle->setText(QStringLiteral("开始录制"));
        m_actToggle->setEnabled(true);
    }

    void updateTooltip()
    {
        const int s = int(m_elapsed.elapsed() / 1000);
        m_tray->setToolTip(QStringLiteral("录制中 %1:%2")
            .arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
    }

    void notify(const QString &title, const QString &msg, bool opensFile)
    {
        m_notifyOpensFile = opensFile;
        m_tray->showMessage(title, msg, QSystemTrayIcon::Information, 4000);
    }

    static void showInFolder(const QString &file)
    {
#ifdef Q_OS_WIN
        QProcess::startDetached("explorer.exe",
            QStringList() << "/select," + QDir::toNativeSeparators(file));
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
#endif
    }

    void quit()
    {
        if (m_proc) { m_quitAfter = true; stopRecording(); }
        else qApp->quit();
    }

private:
    QSystemTrayIcon *m_tray = nullptr;
    QMenu m_menu;
    QAction *m_actToggle = nullptr;
    QProcess *m_proc = nullptr;
    RegionSelector *m_selector = nullptr;
    QSettings m_set{"RecTools", "ScreenRecorder"};
    QTimer m_tick;
    QElapsedTimer m_elapsed;
    QString m_lastFile;
    QByteArray m_stderrTail;
    bool m_quitAfter = false;
    bool m_notifyOpensFile = false;
};

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setQuitOnLastWindowClosed(false);               // 没有窗口也不退出，常驻托盘

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "ScreenRecorder", QStringLiteral("系统托盘不可用"));
        return 1;
    }
    Recorder r;
    return a.exec();
}

#include "main.moc"
