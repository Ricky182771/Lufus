/*
 * mainwindow.cpp — Main application window
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/mainwindow.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QIcon>
#include <QProcess>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QTemporaryFile>
#include <QStandardItemModel>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QTime>
#include <QUuid>
#include <unistd.h>

namespace RufusLinux {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Lufus %1").arg(APP_VERSION));

    m_deviceMgr   = new DeviceManager(this);
    m_isoAnalyzer = new IsoAnalyzer(this);

    setupUI();
    setupMenuBar();
    setupStatusBar();
    applyStyleSheet();
    initLogFile();

    connect(m_deviceMgr, &DeviceManager::devicesChanged,
            this, &MainWindow::onDevicesChanged);

    m_deviceMgr->startMonitoring();
    refreshDevices();
}

MainWindow::~MainWindow() {
    if (m_backendProcess && m_backendProcess->state() != QProcess::NotRunning) {
        m_backendProcess->kill();
        m_backendProcess->waitForFinished(5000);
    }
}

// ──────────────────────────────────────────────
// UI Construction
// ──────────────────────────────────────────────
void MainWindow::setupUI() {
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    // ── Device Section ──
    auto *deviceGroup = new QGroupBox(tr("Device"), this);
    auto *deviceLayout = new QHBoxLayout(deviceGroup);
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_refreshBtn = new QPushButton(this);
    m_refreshBtn->setIcon(QIcon(":/icons/refresh.png"));
    m_refreshBtn->setFixedWidth(36);
    m_refreshBtn->setToolTip(tr("Refresh device list"));
    m_showHardDrives = new QCheckBox(tr("List USB Hard Drives"), this);
    deviceLayout->addWidget(m_deviceCombo);
    deviceLayout->addWidget(m_refreshBtn);
    deviceLayout->addWidget(m_showHardDrives);
    mainLayout->addWidget(deviceGroup);

    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDeviceSelected);
    connect(m_showHardDrives, &QCheckBox::toggled, [this](bool checked) {
        m_deviceMgr->setShowFixedDrives(checked);
        refreshDevices();
    });

    // ── Boot Selection ──
    auto *bootGroup = new QGroupBox(tr("Boot selection"), this);
    auto *bootLayout = new QVBoxLayout(bootGroup);
    auto *bootRow = new QHBoxLayout();
    m_bootTypeCombo = new QComboBox(this);
    m_bootTypeCombo->addItems({tr("Disk or ISO image"), tr("FreeDOS"), tr("Non bootable")});
    m_selectIsoBtn = new QPushButton(tr("SELECT"), this);
    m_selectIsoBtn->setFixedWidth(80);
    bootRow->addWidget(m_bootTypeCombo, 1);
    bootRow->addWidget(m_selectIsoBtn);
    m_isoInfoLabel = new QLabel(tr("No ISO selected"), this);
    m_isoInfoLabel->setWordWrap(true);
    m_isoInfoLabel->setStyleSheet("color: #888; font-size: 11px;");
    bootLayout->addLayout(bootRow);
    bootLayout->addWidget(m_isoInfoLabel);
    mainLayout->addWidget(bootGroup);

    connect(m_selectIsoBtn, &QPushButton::clicked, this, &MainWindow::browseIso);

    // ── Image Option (Write Mode) ──
    auto *imageGroup = new QGroupBox(tr("Image option"), this);
    auto *imageLayout = new QHBoxLayout(imageGroup);
    m_writeModeCombo = new QComboBox(this);
    m_writeModeCombo->addItems({tr("Standard ISO extraction (Recommended)"),
                                 tr("DD Image (raw write)")});
    imageLayout->addWidget(new QLabel(tr("Write mode:"), this));
    imageLayout->addWidget(m_writeModeCombo, 1);

    connect(m_writeModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onWriteModeChanged);

    // ── Partition Scheme & Target ──
    auto *partGroup = new QGroupBox(tr("Partition scheme"), this);
    auto *partGrid = new QGridLayout(partGroup);
    m_partSchemeCombo = new QComboBox(this);
    m_partSchemeCombo->addItems({"MBR", "GPT"});
    m_targetCombo = new QComboBox(this);
    m_targetCombo->addItems({tr("BIOS (or UEFI-CSM)"), tr("UEFI (non CSM)"),
                              tr("BIOS + UEFI")});
    partGrid->addWidget(new QLabel(tr("Scheme:"), this), 0, 0);
    partGrid->addWidget(m_partSchemeCombo, 0, 1);
    partGrid->addWidget(new QLabel(tr("Target system:"), this), 1, 0);
    partGrid->addWidget(m_targetCombo, 1, 1);
    mainLayout->addWidget(partGroup);

    connect(m_partSchemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPartitionSchemeChanged);
    connect(m_targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTargetSystemChanged);

    // ── Advanced Drive Properties (collapsible) ──
    m_advancedToggleBtn = new QPushButton(tr("▶  Show advanced drive properties"), this);
    m_advancedToggleBtn->setCheckable(true);
    m_advancedToggleBtn->setObjectName("advancedToggle");
    mainLayout->addWidget(m_advancedToggleBtn);

    m_advancedPanel = new QWidget(this);
    auto *advLayout = new QVBoxLayout(m_advancedPanel);
    advLayout->setContentsMargins(16, 2, 8, 4);
    advLayout->setSpacing(4);
    m_badBlocksCheck = new QCheckBox(tr("Check device for bad blocks"), this);
    advLayout->addWidget(m_badBlocksCheck);
    m_advancedPanel->setVisible(false);
    mainLayout->addWidget(m_advancedPanel);

    connect(m_advancedToggleBtn, &QPushButton::toggled, this, &MainWindow::toggleAdvanced);

    // ── Format Options ──
    auto *formatGroup = new QGroupBox(tr("Format options"), this);
    auto *formatGrid = new QGridLayout(formatGroup);
    m_volumeLabel = new QLineEdit("Lufus", this);
    m_filesystemCombo = new QComboBox(this);
    m_filesystemCombo->addItem("FAT32", (int)FileSystem::FAT32);
    m_filesystemCombo->addItem("NTFS",  (int)FileSystem::NTFS);
    m_filesystemCombo->addItem("exFAT", (int)FileSystem::exFAT);
    m_clusterSizeCombo = new QComboBox(this);
    m_clusterSizeCombo->addItem(tr("Default"));
    formatGrid->addWidget(new QLabel(tr("Volume label:"), this), 0, 0);
    formatGrid->addWidget(m_volumeLabel, 0, 1);
    formatGrid->addWidget(new QLabel(tr("File system:"), this), 1, 0);
    formatGrid->addWidget(m_filesystemCombo, 1, 1);
    formatGrid->addWidget(new QLabel(tr("Cluster size:"), this), 2, 0);
    formatGrid->addWidget(m_clusterSizeCombo, 2, 1);
    mainLayout->addWidget(formatGroup);

    connect(m_filesystemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFileSystemChanged);
    connect(m_volumeLabel, &QLineEdit::textChanged,
            this, &MainWindow::sanitizeVolumeLabel);

    // ── Progress & Controls ──
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    mainLayout->addWidget(m_progressBar);

    // ── Bottom Row: Icon buttons + START/CANCEL ──
    auto *bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(4);

    m_langBtn = new QPushButton("🌐", this);
    m_langBtn->setObjectName("iconBtn");
    m_langBtn->setFixedSize(34, 38);
    m_langBtn->setToolTip(tr("Language"));
    m_langBtn->setEnabled(false);

    m_infoBtn = new QPushButton("ℹ", this);
    m_infoBtn->setObjectName("iconBtn");
    m_infoBtn->setFixedSize(34, 38);
    m_infoBtn->setToolTip(tr("About"));

    m_settingsBtn = new QPushButton("⚙", this);
    m_settingsBtn->setObjectName("iconBtn");
    m_settingsBtn->setFixedSize(34, 38);
    m_settingsBtn->setToolTip(tr("Settings"));
    m_settingsBtn->setEnabled(false);

    m_logBtn = new QPushButton("📋", this);
    m_logBtn->setObjectName("iconBtn");
    m_logBtn->setFixedSize(34, 38);
    m_logBtn->setToolTip(tr("Show log"));

    bottomRow->addWidget(m_langBtn);
    bottomRow->addWidget(m_infoBtn);
    bottomRow->addWidget(m_settingsBtn);
    bottomRow->addWidget(m_logBtn);
    bottomRow->addStretch();

    m_startBtn = new QPushButton(tr("START"), this);
    m_startBtn->setEnabled(false);
    m_startBtn->setMinimumHeight(38);
    m_startBtn->setMinimumWidth(90);
    m_startBtn->setObjectName("startBtn");

    m_cancelBtn = new QPushButton(tr("CANCEL"), this);
    m_cancelBtn->setEnabled(false);
    m_cancelBtn->setMinimumHeight(38);
    m_cancelBtn->setMinimumWidth(90);

    bottomRow->addWidget(m_startBtn);
    bottomRow->addWidget(m_cancelBtn);
    mainLayout->addLayout(bottomRow);

    connect(m_startBtn,  &QPushButton::clicked, this, &MainWindow::startWrite);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::cancelWrite);
    connect(m_infoBtn,   &QPushButton::clicked, this, &MainWindow::showAbout);
    connect(m_logBtn,    &QPushButton::clicked, this, &MainWindow::toggleLog);

    mainLayout->setSizeConstraint(QLayout::SetFixedSize);
    setCentralWidget(central);

    // ── Log Window (ventana separada) ──
    m_logWindow = new QWidget(this, Qt::Window);
    m_logWindow->setWindowTitle(tr("Lufus — Log"));
    m_logWindow->resize(560, 320);
    auto *logLayout = new QVBoxLayout(m_logWindow);
    logLayout->setContentsMargins(8, 8, 8, 8);
    m_logView = new QTextEdit(m_logWindow);
    m_logView->setReadOnly(true);
    logLayout->addWidget(m_logView);
}

void MainWindow::setupMenuBar() {
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"),           this, &MainWindow::showAbout);
    helpMenu->addAction(tr("Show &Log"),        this, &MainWindow::toggleLog);
    helpMenu->addAction(tr("Open Log &Folder"), this, &MainWindow::openLogFolder);
}

void MainWindow::setupStatusBar() {
    m_statusLabel = new QLabel(tr("Ready"), this);
    statusBar()->addWidget(m_statusLabel, 1);

    m_timerLabel = new QLabel("00:00", this);
    m_timerLabel->setStyleSheet("color: #6c7086; font-family: monospace; font-size: 11px;");
    m_timerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(m_timerLabel);

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &MainWindow::tickTimer);
}

void MainWindow::applyStyleSheet() {
    setStyleSheet(R"(
        QMainWindow { background-color: #1e1e2e; }
        QGroupBox {
            font-weight: bold; color: #cdd6f4;
            border: 1px solid #45475a; border-radius: 6px;
            margin-top: 10px; padding-top: 14px;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
        QLabel { color: #cdd6f4; }
        QComboBox {
            background: #313244; color: #cdd6f4; border: 1px solid #45475a;
            border-radius: 4px; padding: 4px 28px 4px 8px; min-height: 24px;
        }
        QComboBox:hover { border-color: #89b4fa; }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 20px;
            border-left: 1px solid #555;
        }
        QComboBox::down-arrow {
            image: url(:/icons/down-triangle.png);
            width: 10px; height: 6px;
        }
        QComboBox QAbstractItemView {
            background: #313244; color: #cdd6f4;
            selection-background-color: #45475a;
        }
        QLineEdit {
            background: #313244; color: #cdd6f4; border: 1px solid #45475a;
            border-radius: 4px; padding: 4px 8px;
        }
        QLineEdit:focus { border-color: #89b4fa; }
        QPushButton {
            background: #89b4fa; color: #1e1e2e; border: none;
            border-radius: 4px; padding: 6px 16px; font-weight: bold;
        }
        QPushButton:hover { background: #74c7ec; }
        QPushButton:pressed { background: #94e2d5; }
        QPushButton:disabled {
            background: #252535; color: #4a4a5e; border: 1px solid #35354a;
        }
        QPushButton#startBtn:enabled { background: #4CAF50; color: #ffffff; }
        QPushButton#startBtn:enabled:hover { background: #43a047; }
        QPushButton#startBtn:enabled:pressed { background: #388e3c; }
        QPushButton#iconBtn {
            background: transparent; border: 1px solid #45475a;
            border-radius: 4px; color: #cdd6f4;
            font-size: 15px; padding: 0; font-weight: normal;
        }
        QPushButton#iconBtn:hover { background: #313244; border-color: #89b4fa; }
        QPushButton#iconBtn:pressed { background: #45475a; }
        QPushButton#iconBtn:disabled {
            background: transparent; color: #45475a; border-color: #2a2a3a;
        }
        QPushButton#advancedToggle {
            background: transparent; border: none;
            color: #89b4fa; font-weight: normal; font-size: 11px;
            text-align: left; padding: 2px 4px;
        }
        QPushButton#advancedToggle:hover { color: #cba6f7; }
        QProgressBar {
            background: #252535; border: 1px solid #45475a; border-radius: 4px;
            text-align: center; color: #cdd6f4; min-height: 22px;
        }
        QProgressBar::chunk { background: #4CAF50; border-radius: 3px; }
        QTextEdit {
            background: #11111b; color: #a6adc8; border: 1px solid #45475a;
            border-radius: 4px; font-family: monospace; font-size: 11px;
        }
        QCheckBox { color: #cdd6f4; }
        QCheckBox::indicator { width: 16px; height: 16px; }
        QMenuBar { background: #181825; color: #cdd6f4; }
        QMenuBar::item:selected { background: #313244; }
        QMenu { background: #313244; color: #cdd6f4; border: 1px solid #45475a; }
        QMenu::item:selected { background: #45475a; }
        QStatusBar { background: #181825; color: #6c7086; }
    )");

    m_logWindow->setStyleSheet(
        "QWidget { background: #1e1e2e; }"
        "QTextEdit { background: #11111b; color: #a6adc8; border: 1px solid #45475a;"
        " border-radius: 4px; font-family: monospace; font-size: 11px; }"
        "QScrollBar:vertical { background: #1e1e2e; width: 8px; border-radius: 4px; }"
        "QScrollBar::handle:vertical { background: #45475a; border-radius: 4px; }"
    );
}

// ──────────────────────────────────────────────
// Device Slots
// ──────────────────────────────────────────────
void MainWindow::refreshDevices() {
    m_devices = m_deviceMgr->scanDevices();
    m_deviceCombo->clear();
    for (const auto &dev : m_devices)
        m_deviceCombo->addItem(dev.displayName());
    if (m_devices.isEmpty())
        m_deviceCombo->addItem(tr("No USB devices detected"));
    updateStartButtonState();
    log(tr("Found %1 USB device(s)").arg(m_devices.size()));
}

void MainWindow::onDevicesChanged(const QList<UsbDevice> &devices) {
    m_devices = devices;
    int prevIdx = m_deviceCombo->currentIndex();
    m_deviceCombo->clear();
    for (const auto &dev : m_devices)
        m_deviceCombo->addItem(dev.displayName());
    if (m_devices.isEmpty())
        m_deviceCombo->addItem(tr("No USB devices detected"));
    if (prevIdx >= 0 && prevIdx < m_deviceCombo->count())
        m_deviceCombo->setCurrentIndex(prevIdx);
    updateStartButtonState();
}

void MainWindow::onDeviceSelected(int) {
    updateClusterSizes();
    updateStartButtonState();
}

// ──────────────────────────────────────────────
// ISO Slots
// ──────────────────────────────────────────────
void MainWindow::browseIso() {
    QString path = QFileDialog::getOpenFileName(this,
        tr("Select ISO Image"), QDir::homePath(),
        tr("ISO Images (*.iso);;All Files (*)"));
    if (path.isEmpty()) return;

    m_bootTypeCombo->setItemText(0, QFileInfo(path).fileName());
    m_isoInfoLabel->setText(tr("Analyzing…"));
    m_isoInfoLabel->setStyleSheet("color: #f9e2af; font-size: 11px;");
    log(tr("Analyzing ISO: %1").arg(path));

    auto *watcher = new QFutureWatcher<IsoAnalysis>(this);
    connect(watcher, &QFutureWatcher<IsoAnalysis>::finished, [this, watcher]() {
        onIsoAnalyzed(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([this, path]() {
        return m_isoAnalyzer->analyze(path);
    }));
}

void MainWindow::onIsoAnalyzed(const IsoAnalysis &analysis) {
    m_currentAnalysis = analysis;

    if (!analysis.isValid) {
        m_bootTypeCombo->setItemText(0, tr("Disk or ISO image"));
        m_isoInfoLabel->setText(tr("Invalid ISO: %1").arg(analysis.description));
        m_isoInfoLabel->setStyleSheet("color: #f38ba8; font-size: 11px;");
        log(tr("ERROR: %1").arg(analysis.description));
        return;
    }

    m_bootTypeCombo->setItemText(0, QFileInfo(analysis.filePath).fileName());
    m_isoInfoLabel->setText(analysis.description);
    m_isoInfoLabel->setStyleSheet("color: #a6e3a1; font-size: 11px;");
    log(tr("ISO: %1").arg(analysis.description));

    autoConfigureForIso(analysis);
    updateStartButtonState();
}

// FIX 3: autoConfigureForIso usa los métodos type-safe del nuevo BootMode
// (hasBios, hasUefi, hasBoth) en lugar de comparaciones con el enum antiguo.
void MainWindow::autoConfigureForIso(const IsoAnalysis &analysis) {
    if (!analysis.volumeLabel.isEmpty())
        m_volumeLabel->setText(analysis.volumeLabel);

    // onPartitionSchemeChanged fires synchronously when the scheme changes,
    // repopulating m_targetCombo before the next line runs.
    // GPT items: 0="UEFI (non CSM)"  1="BIOS + UEFI"
    // MBR items: 0="BIOS (or UEFI-CSM)"  1="UEFI (non CSM)"  2="BIOS + UEFI"
    if (analysis.bootMode.hasBoth()) {
        m_partSchemeCombo->setCurrentIndex(1); // GPT
        m_targetCombo->setCurrentIndex(1);     // BIOS + UEFI  (GPT index 1)
    } else if (analysis.bootMode.hasUefi()) {
        m_partSchemeCombo->setCurrentIndex(1); // GPT
        m_targetCombo->setCurrentIndex(0);     // UEFI (non CSM)  (GPT index 0)
    } else if (analysis.bootMode.hasBios()) {
        m_partSchemeCombo->setCurrentIndex(0); // MBR
        m_targetCombo->setCurrentIndex(0);     // BIOS (or UEFI-CSM)  (MBR index 0)
    }
    // Si isNone(), no tocamos la selección actual

    constexpr uint64_t kFat32FileSizeLimit = 4ULL * 1024 * 1024 * 1024;
    bool isWindowsIso = (analysis.isoType == IsoType::WindowsInstaller
                      || analysis.isoType == IsoType::WindowsPE);
    // Windows ISOs always use FAT32 — wimlib handles oversized install.wim via split.
    // Only block FAT32 for Linux ISOs that contain a file exceeding the 4 GB limit.
    bool fat32Blocked = !isWindowsIso
                     && (analysis.needsSplitWim
                         || analysis.largestFileSize >= kFat32FileSizeLimit);

    auto *model = qobject_cast<QStandardItemModel*>(m_filesystemCombo->model());
    if (model) {
        for (int i = 0; i < model->rowCount(); ++i)
            if (auto *item = model->item(i)) item->setEnabled(true);

        if (fat32Blocked) {
            if (auto *item = model->item(0)) item->setEnabled(false); // FAT32
            m_filesystemCombo->setCurrentIndex(2); // exFAT for Linux large files
        } else {
            m_filesystemCombo->setCurrentIndex(0); // FAT32
        }
    } else {
        m_filesystemCombo->setCurrentIndex(fat32Blocked ? 2 : 0);
    }

    if (analysis.isIsoHybrid && analysis.isoType != IsoType::WindowsInstaller)
        m_writeModeCombo->setCurrentIndex(0);
}

// ──────────────────────────────────────────────
// Config change slots
// ──────────────────────────────────────────────
void MainWindow::onPartitionSchemeChanged(int index) {
    if (index == 0) { // MBR
        m_targetCombo->clear();
        m_targetCombo->addItems({tr("BIOS (or UEFI-CSM)"), tr("UEFI (non CSM)"),
                                  tr("BIOS + UEFI")});
    } else { // GPT
        m_targetCombo->clear();
        m_targetCombo->addItems({tr("UEFI (non CSM)"), tr("BIOS + UEFI")});
    }
}

void MainWindow::onTargetSystemChanged(int) {
    updateStartButtonState();
}

void MainWindow::onFileSystemChanged(int) {
    updateClusterSizes();
    sanitizeVolumeLabel();
}

void MainWindow::onWriteModeChanged(int index) {
    bool isDd = (index == 1);
    m_partSchemeCombo->setEnabled(!isDd);
    m_targetCombo->setEnabled(!isDd);
    m_filesystemCombo->setEnabled(!isDd);
    m_clusterSizeCombo->setEnabled(!isDd);
    m_volumeLabel->setEnabled(!isDd);
}

void MainWindow::updateClusterSizes() {
    m_clusterSizeCombo->clear();
    m_clusterSizeCombo->addItem(tr("Default"));

    FileSystem fs = static_cast<FileSystem>(m_filesystemCombo->currentData().toInt());
    uint64_t devSize = 0;
    int devIdx = m_deviceCombo->currentIndex();
    if (devIdx >= 0 && devIdx < m_devices.size())
        devSize = m_devices[devIdx].sizeBytes;

    auto sizes = PartitionManager::recommendedClusterSizes(fs, devSize);
    for (uint32_t s : sizes) {
        if (s < 1024)
            m_clusterSizeCombo->addItem(QString("%1 bytes").arg(s));
        else
            m_clusterSizeCombo->addItem(QString("%1 KB").arg(s / 1024));
    }
}

// ──────────────────────────────────────────────
// Write Operation
// ──────────────────────────────────────────────
void MainWindow::startWrite() {
    int devIdx = m_deviceCombo->currentIndex();
    if (devIdx < 0 || devIdx >= m_devices.size()) return;
    if (!m_currentAnalysis.isValid) return;

    const UsbDevice &dev = m_devices[devIdx];

    if (m_currentAnalysis.isIsoHybrid) {
        QMessageBox msgBox(this);
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setWindowTitle(tr("ISOHybrid image detected"));
        msgBox.setText(tr("The image you have selected is an 'ISOHybrid' image. "
                          "This means it can be written either in ISO Image (file copy) "
                          "mode or DD Image (disk image) mode."));
        msgBox.setInformativeText(tr("Lufus recommends using ISO Image mode so that "
                                     "you always have full access to the USB drive after "
                                     "writing it.\n\nHowever, if you encounter issues during "
                                     "boot, you can try writing this image again in DD Image mode."));

        QPushButton *isoBtn = msgBox.addButton(
            tr("Write in ISO Image mode (Recommended)"), QMessageBox::AcceptRole);
        QPushButton *ddBtn = msgBox.addButton(
            tr("Write in DD Image mode"), QMessageBox::AcceptRole);
        msgBox.addButton(QMessageBox::Cancel);

        msgBox.exec();

        if (msgBox.clickedButton() == isoBtn) {
            m_writeModeCombo->setCurrentIndex(0);
        } else if (msgBox.clickedButton() == ddBtn) {
            m_writeModeCombo->setCurrentIndex(1);
        } else {
            return;
        }
    }

    // Check wimlib availability before the confirmation dialog so the user
    // isn't surprised after saying Yes.
    {
        bool isWinIso = (m_currentAnalysis.isoType == IsoType::WindowsInstaller ||
                         m_currentAnalysis.isoType == IsoType::WindowsPE);
        if (isWinIso && m_currentAnalysis.needsSplitWim) {
            QProcess check;
            check.start("which", {"wimlib-imagex"});
            check.waitForFinished(3000);
            if (check.exitCode() != 0) {
                QString hint = "wimtools";
                QFile osRelease("/etc/os-release");
                if (osRelease.open(QIODevice::ReadOnly)) {
                    QString content = QString::fromUtf8(osRelease.readAll());
                    if (content.contains("ID=debian") || content.contains("ID=ubuntu") ||
                        content.contains("ID_LIKE=debian"))
                        hint = "sudo apt install wimtools";
                    else if (content.contains("ID=fedora") || content.contains("ID_LIKE=fedora") ||
                             content.contains("ID=rhel"))
                        hint = "sudo dnf install wimlib-utils";
                    else if (content.contains("ID=arch") || content.contains("ID_LIKE=arch"))
                        hint = "sudo pacman -S wimlib";
                    else if (content.contains("ID=opensuse") || content.contains("ID_LIKE=suse"))
                        hint = "sudo zypper install wimlib";
                }
                QMessageBox::critical(this, tr("wimlib-imagex not found"),
                    tr("This Windows ISO has an install.wim larger than 4 GB.\n\n"
                       "Lufus needs wimlib to split install.wim into FAT32-compatible "
                       "chunks so that Windows 11 boots correctly from a single partition.\n\n"
                       "Please install it first:\n\n%1").arg(hint));
                return;
            }
        }
    }

    auto reply = QMessageBox::warning(this, tr("Confirm Write"),
        tr("ALL DATA on %1 (%2) will be DESTROYED.\n\nAre you sure you want to continue?")
            .arg(dev.displayName(), dev.formattedSize()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    m_isWriting = true;
    m_startBtn->setEnabled(false);
    m_cancelBtn->setEnabled(true);
    m_progressBar->setValue(0);

    m_lastProgressMsg.clear();
    m_elapsedTimer.start();
    m_timerLabel->setText("00:00");
    m_timerLabel->setStyleSheet("color: #a6e3a1; font-family: monospace; font-size: 11px;");
    m_tickTimer->start();

    // FIX: config JSON va a un archivo con nombre único por sesión,
    // no a una ruta hardcoded que colisionaría en entornos multi-usuario.
    QJsonObject configObj;
    configObj["isoPath"]      = m_currentAnalysis.filePath;
    configObj["deviceNode"]   = dev.deviceNode;
    configObj["mode"]         = (int)((m_writeModeCombo->currentIndex() == 1)
                                      ? WriteMode::DD : WriteMode::ISO);
    configObj["scheme"]       = (int)((m_partSchemeCombo->currentIndex() == 0)
                                      ? PartitionScheme::MBR : PartitionScheme::GPT);
    configObj["filesystem"]   = m_filesystemCombo->currentData().toInt();
    configObj["volumeLabel"]  = m_volumeLabel->text();

    int targetIdx = m_targetCombo->currentIndex();
    if (m_partSchemeCombo->currentIndex() == 0) { // MBR
        configObj["target"] = targetIdx;
    } else { // GPT: índice 0 = UEFI only, índice 1 = BIOS+UEFI
        configObj["target"] = (targetIdx == 0)
                              ? (int)TargetSystem::UefiOnly
                              : (int)TargetSystem::BiosAndUefi;
    }

    configObj["dualPartition"] = false;  // Windows uses single FAT32 + wimlib split now
    configObj["needsSplitWim"] = m_currentAnalysis.needsSplitWim;
    configObj["isoType"]       = (int)m_currentAnalysis.isoType;

    // FIX: nombre de archivo único por sesión para evitar colisiones multi-usuario
    QString uuid    = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    QString jsonPath = QString("/tmp/rufus_backend_%1.json").arg(uuid);

    QFile file(jsonPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(configObj).toJson());
        file.close();
    }

    log(tr("Starting backend write to %1...").arg(dev.deviceNode));

    m_backendProcess = new QProcess(this);
    connect(m_backendProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        while (m_backendProcess->canReadLine()) {
            QString line = QString::fromUtf8(m_backendProcess->readLine()).trimmed();
            if (line.startsWith("PROGRESS:")) {
                QStringList parts = line.split(':');
                if (parts.size() >= 3) {
                    int pct = parts[1].toInt();
                    QString msg = parts.mid(2).join(':');
                    onWriteProgress(pct, msg);
                }
            } else if (line.startsWith("ERROR:")) {
                onWriteError(line.mid(6));
            } else {
                log("[Backend] " + line);
            }
        }
    });

    connect(m_backendProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, jsonPath](int exitCode) {
        QFile::remove(jsonPath); // limpiar config temporal
        onWriteCompleted(exitCode == 0);
    });

    QString appPath = QCoreApplication::applicationFilePath();
    if (geteuid() != 0) {
        m_backendProcess->start("pkexec", {appPath, "--write-backend", jsonPath});
    } else {
        m_backendProcess->start(appPath, {"--write-backend", jsonPath});
    }
}

void MainWindow::cancelWrite() {
    if (m_backendProcess && m_backendProcess->state() != QProcess::NotRunning)
        m_backendProcess->kill();
    log(tr("Cancellation requested..."));
}

void MainWindow::onWriteProgress(int percent, const QString &message) {
    m_progressBar->setValue(percent);
    m_statusLabel->setText(message);
    if (message != m_lastProgressMsg) {
        m_lastProgressMsg = message;
        log(message);
    }
}

void MainWindow::onWriteError(const QString &message) {
    log(tr("ERROR: %1").arg(message));
}

void MainWindow::onWriteCompleted(bool success) {
    m_isWriting = false;
    m_startBtn->setEnabled(true);
    m_cancelBtn->setEnabled(false);

    m_tickTimer->stop();
    tickTimer(); // freeze on final elapsed time
    m_timerLabel->setStyleSheet("color: #6c7086; font-family: monospace; font-size: 11px;");

    if (success) {
        m_progressBar->setValue(100);
        m_statusLabel->setText(tr("READY"));
        QMessageBox::information(this, tr("Success"),
            tr("USB drive created successfully!"));
        log(tr("Write completed successfully."));
    } else {
        m_statusLabel->setText(tr("FAILED"));
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to create USB drive. Check the log for details."));
        log(tr("Write FAILED."));
    }

    if (m_backendProcess) {
        m_backendProcess->deleteLater();
        m_backendProcess = nullptr;
    }
}

// ──────────────────────────────────────────────
// UI Helpers
// ──────────────────────────────────────────────
void MainWindow::updateStartButtonState() {
    bool canStart = !m_devices.isEmpty() &&
                    m_currentAnalysis.isValid &&
                    !m_isWriting;
    m_startBtn->setEnabled(canStart);
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About Lufus"),
        tr("<h2>Lufus %1</h2>"
           "<p>A Linux-native USB bootable media creator, "
           "inspired by <a href='https://rufus.ie'>Rufus</a>.</p>"
           "<p>License: GPL-3.0-or-later</p>").arg(APP_VERSION));
}

void MainWindow::toggleLog() {
    if (m_logWindow->isVisible()) {
        m_logWindow->hide();
    } else {
        m_logWindow->show();
        m_logWindow->raise();
        m_logWindow->activateWindow();
    }
}

void MainWindow::log(const QString &message) {
    QString timestamp = QTime::currentTime().toString("HH:mm:ss");
    QString logLine   = QString("[%1] %2").arg(timestamp, message);
    m_logView->append(logLine);

    if (m_logFile.isOpen()) {
        m_logFile.write((logLine + "\n").toUtf8());
        m_logFile.flush();
    }
}

void MainWindow::initLogFile() {
    QString logDir  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(logDir);
    QString logPath = logDir + "/rufus.log";

    m_logFile.setFileName(logPath);
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        log("--- Application started ---");
        log(tr("Log file: %1").arg(logPath));
    }
}

void MainWindow::openLogFolder() {
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDesktopServices::openUrl(QUrl::fromLocalFile(logDir));
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    setFixedSize(sizeHint());
}

void MainWindow::sanitizeVolumeLabel() {
    int fsVal = m_filesystemCombo->currentData().toInt();
    bool isFat = (fsVal == (int)FileSystem::FAT32 || fsVal == (int)FileSystem::exFAT);
    if (!isFat) return;

    // FAT volume labels forbid: control chars and  " * + , . / : ; < = > ? [ \ ] |
    static const QString kInvalid = QStringLiteral("\"*+,./:;<=>?[\\]|");
    QString text = m_volumeLabel->text();
    QString cleaned;
    cleaned.reserve(text.size());
    for (QChar c : text) {
        if (c >= QChar(0x20) && !kInvalid.contains(c))
            cleaned += c.toUpper();
    }
    if (cleaned == text) return;

    QSignalBlocker blocker(m_volumeLabel);
    int pos = m_volumeLabel->cursorPosition();
    m_volumeLabel->setText(cleaned);
    m_volumeLabel->setCursorPosition(qMin(pos, cleaned.length()));
}

void MainWindow::tickTimer() {
    qint64 secs = m_elapsedTimer.elapsed() / 1000;
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int s = secs % 60;
    if (h > 0)
        m_timerLabel->setText(QString("%1:%2:%3")
            .arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
    else
        m_timerLabel->setText(QString("%1:%2")
            .arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
}

void MainWindow::toggleAdvanced() {
    bool expanded = m_advancedToggleBtn->isChecked();
    m_advancedPanel->setVisible(expanded);
    m_advancedToggleBtn->setText(
        expanded ? tr("▼  Hide advanced drive properties")
                 : tr("▶  Show advanced drive properties"));
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    adjustSize();
    setFixedSize(sizeHint());
}

} // namespace RufusLinux
