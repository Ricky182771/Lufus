/*
 * device_manager.h — USB block device enumeration via libudev
 *
 * Provides real-time discovery and monitoring of removable USB storage
 * devices, equivalent to Rufus's SetupAPI / WMI device enumeration on
 * Windows.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QTimer>

#include <libudev.h>
#include <cstdint>
#include <memory>

namespace RufusLinux {

// ──────────────────────────────────────────────
// Data model for a USB block device
// ──────────────────────────────────────────────
struct UsbDevice {
    QString  deviceNode;        // /dev/sdc
    QString  sysPath;           // /sys/devices/pci0000:00/...
    QString  vendor;            // "SanDisk"
    QString  model;             // "Ultra Fit"
    QString  serial;
    uint64_t sizeBytes  = 0;    // total capacity
    bool     isReadOnly = false;
    bool     isRemovable = true;
    int      busNum     = -1;
    int      devNum     = -1;
    QStringList partitions;     // /dev/sdc1, /dev/sdc2, ...

    /** Human-readable label like "SanDisk Ultra Fit (14.3 GB) [/dev/sdc]" */
    QString displayName() const;

    /** Size formatted as "14.3 GB" */
    QString formattedSize() const;

    bool operator==(const UsbDevice &other) const {
        return deviceNode == other.deviceNode;
    }
};

// ──────────────────────────────────────────────
// Device manager
// ──────────────────────────────────────────────
class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    /** Perform a full scan and return the current list of USB devices. */
    QList<UsbDevice> scanDevices();

    /** Start monitoring for hotplug events (insert / remove). */
    void startMonitoring();

    /** Stop monitoring. */
    void stopMonitoring();

    /** Whether to include fixed (non-removable) USB drives. Default: false. */
    void setShowFixedDrives(bool show) { m_showFixed = show; }

signals:
    /** Emitted when the device list changes (add/remove). */
    void devicesChanged(const QList<UsbDevice> &devices);

    /** Emitted when a specific device is added. */
    void deviceAdded(const UsbDevice &dev);

    /** Emitted when a specific device is removed. */
    void deviceRemoved(const QString &deviceNode);

private slots:
    void pollUdevMonitor();

private:
    bool isUsbDevice(struct udev_device *dev) const;
    UsbDevice buildDeviceInfo(struct udev_device *dev) const;
    uint64_t readSysfsSize(const QString &devNode) const;
    QStringList findPartitions(const QString &parentDevNode) const;

    struct udev         *m_udev         = nullptr;
    struct udev_monitor *m_monitor      = nullptr;
    QTimer              *m_pollTimer    = nullptr;
    QList<UsbDevice>     m_currentDevices;
    bool                 m_showFixed    = false;
};

} // namespace RufusLinux
