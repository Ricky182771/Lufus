/*
 * mainwindow.h — Main application window (Rufus-style layout)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QLineEdit>
#include <QGroupBox>
#include <QCheckBox>
#include <QThread>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>

#include "core/iso_analyzer.h"
#include "core/device_manager.h"
#include "core/partition_manager.h"
#include "core/disk_writer.h"
#include "core/bootloader.h"

namespace RufusLinux {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // Device management
    void refreshDevices();
    void onDevicesChanged(const QList<UsbDevice> &devices);
    void onDeviceSelected(int index);

    // ISO selection
    void browseIso();
    void onIsoAnalyzed(const IsoAnalysis &analysis);

    // Configuration changes
    void onPartitionSchemeChanged(int index);
    void onTargetSystemChanged(int index);
    void onFileSystemChanged(int index);
    void onWriteModeChanged(int index);
    void updateClusterSizes();

    // Write operation
    void startWrite();
    void cancelWrite();
    void onWriteProgress(int percent, const QString &message);
    void onWriteError(const QString &message);
    void onWriteCompleted(bool success);

    // UI helpers
    void showAbout();
    void toggleLog();
    void openLogFolder();
    void updateStartButtonState();
    void toggleAdvanced();
    void sanitizeVolumeLabel();
    void tickTimer();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void applyStyleSheet();
    void autoConfigureForIso(const IsoAnalysis &analysis);
    void log(const QString &message);
    void initLogFile();

    // ── Widgets ──
    // Device section
    QComboBox   *m_deviceCombo      = nullptr;
    QPushButton *m_refreshBtn       = nullptr;
    QLabel      *m_deviceInfoLabel  = nullptr;
    QCheckBox   *m_showHardDrives   = nullptr;

    // Boot selection
    QComboBox   *m_bootTypeCombo    = nullptr;    // Disk or ISO image
    QPushButton *m_selectIsoBtn     = nullptr;
    QLabel      *m_isoInfoLabel     = nullptr;

    // Image option
    QComboBox   *m_writeModeCombo   = nullptr;    // ISO mode / DD mode

    // Partition scheme
    QComboBox   *m_partSchemeCombo  = nullptr;    // MBR / GPT

    // Target system
    QComboBox   *m_targetCombo      = nullptr;    // BIOS / UEFI / BIOS+UEFI

    // Format options
    QLineEdit   *m_volumeLabel      = nullptr;
    QComboBox   *m_filesystemCombo  = nullptr;
    QComboBox   *m_clusterSizeCombo = nullptr;

    // Status & Controls
    QProgressBar *m_progressBar     = nullptr;
    QPushButton  *m_startBtn        = nullptr;
    QPushButton  *m_cancelBtn       = nullptr;
    QTextEdit    *m_logView         = nullptr;
    QLabel       *m_statusLabel     = nullptr;
    QLabel       *m_timerLabel      = nullptr;
    QTimer       *m_tickTimer       = nullptr;
    QElapsedTimer m_elapsedTimer;

    // Log window (separate)
    QWidget      *m_logWindow        = nullptr;

    // Bottom icon buttons
    QPushButton  *m_langBtn         = nullptr;
    QPushButton  *m_infoBtn         = nullptr;
    QPushButton  *m_settingsBtn     = nullptr;
    QPushButton  *m_logBtn          = nullptr;

    // Advanced drive properties (collapsible)
    QPushButton  *m_advancedToggleBtn = nullptr;
    QWidget      *m_advancedPanel     = nullptr;
    QCheckBox    *m_badBlocksCheck    = nullptr;

    // ── Core components ──
    DeviceManager  *m_deviceMgr    = nullptr;
    IsoAnalyzer    *m_isoAnalyzer  = nullptr;
    QThread        *m_writeThread  = nullptr;
    DiskWriter     *m_writer       = nullptr;
    QFile           m_logFile;

    // ── State ──
    IsoAnalysis     m_currentAnalysis;
    QList<UsbDevice> m_devices;
    bool            m_isWriting     = false;
    QString         m_lastProgressMsg;
};

} // namespace RufusLinux
