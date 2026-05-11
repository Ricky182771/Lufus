/*
 * device_manager.cpp — USB block device enumeration via libudev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/device_manager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>

#include <fcntl.h>
#include <cstring>

namespace RufusLinux {

// ──────────────────────────────────────────────
// UsbDevice helpers
// ──────────────────────────────────────────────
QString UsbDevice::displayName() const {
    QString name;
    if (!vendor.isEmpty() || !model.isEmpty()) {
        if (!vendor.isEmpty()) name += vendor.trimmed();
        if (!model.isEmpty()) {
            if (!name.isEmpty()) name += ' ';
            name += model.trimmed();
        }
    } else {
        name = deviceNode;
    }
    return QString("%1 (%2) [%3]").arg(name, formattedSize(), deviceNode);
}

QString UsbDevice::formattedSize() const {
    constexpr uint64_t TB = 1000ULL * 1024 * 1024 * 1024;
    constexpr uint64_t GB = 1024ULL * 1024 * 1024;
    constexpr uint64_t MB = 1024ULL * 1024;

    if (sizeBytes >= TB)
        return QString::number(sizeBytes / double(TB), 'f', 1) + " TB";
    if (sizeBytes >= GB)
        return QString::number(sizeBytes / double(GB), 'f', 1) + " GB";
    if (sizeBytes >= MB)
        return QString::number(sizeBytes / double(MB), 'f', 1) + " MB";
    return QString::number(sizeBytes / double(1024), 'f', 0) + " KB";
}

// ──────────────────────────────────────────────
// Helper: detect root filesystem device node
// ──────────────────────────────────────────────
static QString systemRootDisk() {
    QFile mounts("/proc/mounts");
    if (!mounts.open(QIODevice::ReadOnly)) return {};

    const auto lines = QString::fromUtf8(mounts.readAll()).split('\n');
    for (const QString &line : lines) {
        const QStringList parts = line.split(' ');
        if (parts.size() < 2 || parts.at(1) != "/") continue;

        QString dev = parts.at(0);
        if (!dev.startsWith("/dev/")) continue;

        // Strip partition suffix to get the base disk device.
        // nvme0n1p1 → nvme0n1,  sda1 → sda,  mmcblk0p1 → mmcblk0
        static const QRegularExpression nvmeRe(R"((.*n\d+)p\d+$)");
        static const QRegularExpression sdRe(R"((.*[a-z])\d+$)");
        static const QRegularExpression mmcRe(R"((.*mmcblk\d+)p\d+$)");

        QRegularExpressionMatch m;
        if ((m = nvmeRe.match(dev)).hasMatch()) return m.captured(1);
        if ((m = mmcRe.match(dev)).hasMatch())  return m.captured(1);
        if ((m = sdRe.match(dev)).hasMatch())   return m.captured(1);
        return dev;
    }
    return {};
}

// ──────────────────────────────────────────────
// DeviceManager
// ──────────────────────────────────────────────
DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    m_udev = udev_new();
    if (!m_udev)
        qCritical() << "DeviceManager: failed to create udev context";
}

DeviceManager::~DeviceManager() {
    stopMonitoring();
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
}

// ──────────────────────────────────────────────
// Scan
// ──────────────────────────────────────────────
QList<UsbDevice> DeviceManager::scanDevices() {
    QList<UsbDevice> result;
    if (!m_udev) return result;

    const QString sysDisk = systemRootDisk();

    udev_enumerate *en = udev_enumerate_new(m_udev);
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_add_match_property(en, "DEVTYPE", "disk");
    udev_enumerate_scan_devices(en);

    udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(entry);
        udev_device *dev = udev_device_new_from_syspath(m_udev, path);
        if (!dev) continue;

        if (isUsbDevice(dev)) {
            UsbDevice info = buildDeviceInfo(dev);
            if (!info.deviceNode.isEmpty() && info.deviceNode != sysDisk)
                result.append(info);
        }
        udev_device_unref(dev);
    }
    udev_enumerate_unref(en);

    m_currentDevices = result;
    return result;
}

// ──────────────────────────────────────────────
// Monitoring
// ──────────────────────────────────────────────
void DeviceManager::startMonitoring() {
    if (!m_udev || m_monitor) return;

    m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_monitor) {
        qWarning() << "DeviceManager: could not create udev monitor";
        return;
    }

    udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "block", "disk");
    udev_monitor_enable_receiving(m_monitor);

    // Set non-blocking so pollUdevMonitor() doesn't stall
    int fd = udev_monitor_get_fd(m_monitor);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);
    connect(m_pollTimer, &QTimer::timeout, this, &DeviceManager::pollUdevMonitor);
    m_pollTimer->start();
}

void DeviceManager::stopMonitoring() {
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();
        m_pollTimer = nullptr;
    }
    if (m_monitor) {
        udev_monitor_unref(m_monitor);
        m_monitor = nullptr;
    }
}

// ──────────────────────────────────────────────
// Poll slot
// ──────────────────────────────────────────────
void DeviceManager::pollUdevMonitor() {
    if (!m_monitor) return;

    const QString sysDisk = systemRootDisk();
    bool changed = false;

    udev_device *dev;
    while ((dev = udev_monitor_receive_device(m_monitor)) != nullptr) {
        const char *actionRaw = udev_device_get_action(dev);
        const QString action  = actionRaw ? QString::fromUtf8(actionRaw) : QString();
        const char *devNodeRaw = udev_device_get_devnode(dev);
        const QString devNode  = devNodeRaw ? QString::fromUtf8(devNodeRaw) : QString();

        if (action == "add" && isUsbDevice(dev)) {
            UsbDevice info = buildDeviceInfo(dev);
            if (!info.deviceNode.isEmpty() && info.deviceNode != sysDisk) {
                m_currentDevices.append(info);
                emit deviceAdded(info);
                changed = true;
            }
        } else if (action == "remove" && !devNode.isEmpty()) {
            int removed = 0;
            for (int i = m_currentDevices.size() - 1; i >= 0; --i) {
                if (m_currentDevices.at(i).deviceNode == devNode) {
                    m_currentDevices.removeAt(i);
                    ++removed;
                }
            }
            if (removed > 0) {
                emit deviceRemoved(devNode);
                changed = true;
            }
        }

        udev_device_unref(dev);
    }

    if (changed)
        emit devicesChanged(m_currentDevices);
}

// ──────────────────────────────────────────────
// Private helpers
// ──────────────────────────────────────────────
bool DeviceManager::isUsbDevice(struct udev_device *dev) const {
    // Only process whole disks, not partitions
    const char *devtype = udev_device_get_property_value(dev, "DEVTYPE");
    if (!devtype || strcmp(devtype, "disk") != 0) return false;

    // Primary check: ID_BUS property (set by udev rules for USB devices)
    const char *idBus = udev_device_get_property_value(dev, "ID_BUS");
    bool isUsb = (idBus && strcmp(idBus, "usb") == 0);

    if (!isUsb) {
        // Fallback: walk parent chain looking for USB interface
        udev_device *parent = udev_device_get_parent_with_subsystem_devtype(
            dev, "usb", "usb_device");
        isUsb = (parent != nullptr);
    }

    if (!isUsb) return false;

    if (!m_showFixed) {
        // Exclude non-removable USB devices (USB HDDs) unless user opts in
        const char *rem = udev_device_get_sysattr_value(dev, "removable");
        if (!rem || strcmp(rem, "1") != 0) return false;
    }

    return true;
}

UsbDevice DeviceManager::buildDeviceInfo(struct udev_device *dev) const {
    UsbDevice info;

    const char *devnode = udev_device_get_devnode(dev);
    if (!devnode) return info;
    info.deviceNode = QString::fromUtf8(devnode);

    const char *syspath = udev_device_get_syspath(dev);
    if (syspath) info.sysPath = QString::fromUtf8(syspath);

    auto prop = [&](const char *key) -> QString {
        const char *v = udev_device_get_property_value(dev, key);
        return v ? QString::fromUtf8(v) : QString();
    };

    info.vendor = prop("ID_VENDOR").replace('_', ' ').trimmed();
    info.model  = prop("ID_MODEL").replace('_', ' ').trimmed();
    info.serial = prop("ID_SERIAL_SHORT").trimmed();

    // Flatpak sandbox: udev properties are empty; fall back to UDisks2 Drive object.
    if (info.vendor.isEmpty() && info.model.isEmpty()) {
        const QString blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/")
                                  + QFileInfo(info.deviceNode).fileName();
        QDBusInterface block(QStringLiteral("org.freedesktop.UDisks2"), blockPath,
                             QStringLiteral("org.freedesktop.UDisks2.Block"),
                             QDBusConnection::systemBus());
        const QDBusObjectPath drivePath = block.property("Drive").value<QDBusObjectPath>();
        if (!drivePath.path().isEmpty() && drivePath.path() != QStringLiteral("/")) {
            QDBusInterface drive(QStringLiteral("org.freedesktop.UDisks2"), drivePath.path(),
                                 QStringLiteral("org.freedesktop.UDisks2.Drive"),
                                 QDBusConnection::systemBus());
            info.vendor = drive.property("Vendor").toString().trimmed();
            info.model  = drive.property("Model").toString().trimmed();
        }
    }

    const char *rem = udev_device_get_sysattr_value(dev, "removable");
    info.isRemovable = (rem && strcmp(rem, "1") == 0);

    const char *ro = udev_device_get_sysattr_value(dev, "ro");
    info.isReadOnly = (ro && strcmp(ro, "1") == 0);

    info.sizeBytes  = readSysfsSize(info.deviceNode);
    info.partitions = findPartitions(info.deviceNode);

    const QString busnumStr = prop("BUSNUM");
    const QString devnumStr = prop("DEVNUM");
    if (!busnumStr.isEmpty()) info.busNum = busnumStr.toInt();
    if (!devnumStr.isEmpty()) info.devNum = devnumStr.toInt();

    return info;
}

uint64_t DeviceManager::readSysfsSize(const QString &devNode) const {
    const QString devName  = QFileInfo(devNode).fileName();
    const QString sizePath = QString("/sys/class/block/%1/size").arg(devName);

    QFile f(sizePath);
    if (!f.open(QIODevice::ReadOnly)) return 0;

    bool ok = false;
    const uint64_t sectors = QString::fromUtf8(f.readLine()).trimmed().toULongLong(&ok);
    return ok ? sectors * 512ULL : 0;
}

QStringList DeviceManager::findPartitions(const QString &parentDevNode) const {
    const QString devName = QFileInfo(parentDevNode).fileName();
    QDir sysBlock(QString("/sys/block/%1").arg(devName));

    QStringList parts;
    for (const QString &entry : sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.startsWith(devName))
            parts.append("/dev/" + entry);
    }
    parts.sort();
    return parts;
}

} // namespace RufusLinux
