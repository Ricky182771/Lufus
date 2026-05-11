/*
 * partition_manager.cpp — Disk partitioning and formatting
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/partition_manager.h"

#include <QDebug>
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>

namespace RufusLinux {

    PartitionManager::PartitionManager(QObject *parent) : QObject(parent) {}
    PartitionManager::~PartitionManager() = default;

    // ──────────────────────────────────────────────
    // Unmount
    // ──────────────────────────────────────────────
    bool PartitionManager::unmountAll(const QString &deviceNode) {
        emit progressChanged(0, tr("Unmounting partitions on %1...").arg(deviceNode));

        QString baseName = QFileInfo(deviceNode).fileName();
        QDir sysBlock(QString("/sys/block/%1").arg(baseName));

        // Enumerate partitions from sysfs, then try to unmount each via UDisks2.
        // Failures are non-fatal: the subsequent Format call overwrites everything.
        for (const auto &entry : sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (entry.startsWith(baseName))
                udisksUnmount(udisksBlockPath("/dev/" + entry));
        udisksUnmount(udisksBlockPath(deviceNode));
        return true;
    }

    // ──────────────────────────────────────────────
    // Wipe (no-op — Format("gpt"/"dos") wipes implicitly)
    // ──────────────────────────────────────────────
    bool PartitionManager::wipeDisk(const QString &) {
        return true;
    }

    // ──────────────────────────────────────────────
    // Partition
    // ──────────────────────────────────────────────
    bool PartitionManager::partitionDisk(const QString &deviceNode,
                                         const PartitionConfig &config,
                                         bool dualPartition, uint64_t espSizeMB)
    {
        if (config.scheme == PartitionScheme::GPT)
            return partitionGPT(deviceNode, config, dualPartition, espSizeMB);
        else
            return partitionMBR(deviceNode, config, dualPartition, espSizeMB);
    }

    bool PartitionManager::partitionMBR(const QString &deviceNode,
                                        const PartitionConfig &config,
                                        bool dualPartition, uint64_t espSizeMB)
    {
        emit progressChanged(20, tr("Creating MBR partition table on %1...").arg(deviceNode));
        const QString blockPath = udisksBlockPath(deviceNode);

        // Create an empty MBR partition table (also wipes existing signatures).
        if (!udisksFormat(blockPath, QStringLiteral("dos")))  {
            emit errorOccurred(tr("Failed to create MBR partition table on %1").arg(deviceNode));
            return false;
        }
        QThread::msleep(500);

        if (dualPartition) {
            // Partition 1: FAT32 boot (espSizeMB), bootable
            const QString p1 = udisksCreatePartition(blockPath,
                0, espSizeMB * 1024 * 1024,
                QStringLiteral("0x0c"), QStringLiteral("Boot"));
            if (p1.isEmpty()) {
                emit errorOccurred(tr("Failed to create boot partition on %1").arg(deviceNode));
                return false;
            }
            // Partition 2: data (fill rest)
            QString dataType;
            switch (config.filesystem) {
                case FileSystem::NTFS: dataType = QStringLiteral("0x07"); break;
                case FileSystem::ext4: dataType = QStringLiteral("0x83"); break;
                default:               dataType = QStringLiteral("0x0c"); break;
            }
            const quint64 p1End = udisksPartitionEnd(udisksBlockPath(p1));
            const QString p2 = udisksCreatePartition(blockPath,
                p1End, 0, dataType, QStringLiteral("Data"));
            if (p2.isEmpty()) {
                emit errorOccurred(tr("Failed to create data partition on %1").arg(deviceNode));
                return false;
            }
        } else {
            QString typeHex;
            switch (config.filesystem) {
                case FileSystem::FAT32: typeHex = QStringLiteral("0x0c"); break;
                case FileSystem::NTFS:  typeHex = QStringLiteral("0x07"); break;
                case FileSystem::exFAT: typeHex = QStringLiteral("0x07"); break;
                case FileSystem::ext4:  typeHex = QStringLiteral("0x83"); break;
                case FileSystem::UDF:   typeHex = QStringLiteral("0x07"); break;
            }
            if (udisksCreatePartition(blockPath, 0, 0, typeHex, QStringLiteral("LUFUS")).isEmpty()) {
                emit errorOccurred(tr("Failed to create partition on %1").arg(deviceNode));
                return false;
            }
        }
        QThread::msleep(500);
        return true;
    }

    bool PartitionManager::partitionGPT(const QString &deviceNode,
                                        const PartitionConfig &config,
                                        bool dualPartition, uint64_t espSizeMB)
    {
        emit progressChanged(20, tr("Creating GPT partition table on %1...").arg(deviceNode));
        const QString blockPath = udisksBlockPath(deviceNode);

        // GPT para Windows SIEMPRE usa dos particiones:
        //   1. FAT32 ESP — el firmware UEFI solo puede leer FAT32.
        //   2. NTFS/data — archivos de instalación.
        // Una sola partición NTFS en GPT nunca arranca por UEFI.
        // Para Linux single: FAT32 en EBD0A0A2 (el firmware permisivo arranca desde aquí).

        if (!udisksFormat(blockPath, QStringLiteral("gpt"))) {
            emit errorOccurred(tr("Failed to create GPT partition table on %1").arg(deviceNode));
            return false;
        }
        QThread::msleep(500);

        if (dualPartition) {
            const QString p1 = udisksCreatePartition(blockPath,
                0, espSizeMB * 1024 * 1024,
                QStringLiteral("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"),
                QStringLiteral("EFI System Partition"));
            if (p1.isEmpty()) {
                emit errorOccurred(tr("Failed to create EFI partition on %1").arg(deviceNode));
                return false;
            }
            QString dataType;
            switch (config.filesystem) {
                case FileSystem::ext4: dataType = QStringLiteral("0FC63DAF-8483-4772-8E79-3D69D8477DE4"); break;
                default:               dataType = QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"); break;
            }
            const quint64 p1End = udisksPartitionEnd(udisksBlockPath(p1));
            const QString p2 = udisksCreatePartition(blockPath,
                p1End, 0, dataType, QStringLiteral("Windows"));
            if (p2.isEmpty()) {
                emit errorOccurred(tr("Failed to create data partition on %1").arg(deviceNode));
                return false;
            }
        } else {
            if (udisksCreatePartition(blockPath, 0, 0,
                    QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"),
                    QStringLiteral("LUFUS")).isEmpty()) {
                emit errorOccurred(tr("Failed to create partition on %1").arg(deviceNode));
                return false;
            }
        }
        QThread::msleep(500);
        return true;
    }

    // ──────────────────────────────────────────────
    // Format
    // ──────────────────────────────────────────────
    bool PartitionManager::formatPartition(const QString &partNode, FileSystem fs,
                                           const QString &label, uint32_t /*clusterSize*/)
    {
        emit progressChanged(40, tr("Formatting %1...").arg(partNode));

        auto truncLabel = [&](int maxLen) -> QString {
            QString s = label.left(maxLen);
            return s;
        };

        QString type;
        QVariantMap opts;

        switch (fs) {
            case FileSystem::FAT32:
                type = QStringLiteral("vfat");
                opts[QStringLiteral("label")] = truncLabel(11).toUpper().trimmed();
                break;
            case FileSystem::NTFS:
                type = QStringLiteral("ntfs");
                opts[QStringLiteral("label")] = truncLabel(32);
                break;
            case FileSystem::exFAT:
                type = QStringLiteral("exfat");
                opts[QStringLiteral("label")] = truncLabel(15);
                break;
            case FileSystem::ext4:
                type = QStringLiteral("ext4");
                opts[QStringLiteral("label")] = truncLabel(16);
                break;
            case FileSystem::UDF:
                type = QStringLiteral("udf");
                opts[QStringLiteral("label")] = truncLabel(31);
                break;
        }

        if (!udisksFormat(udisksBlockPath(partNode), type, opts)) {
            emit errorOccurred(tr("Formatting failed on %1").arg(partNode));
            return false;
        }
        return true;
    }

    // ──────────────────────────────────────────────
    // High-level: prepare disk
    // ──────────────────────────────────────────────
    QStringList PartitionManager::prepareDisk(const QString &deviceNode,
                                              const PartitionConfig &config,
                                              bool dualPartition,
                                              uint64_t espSizeMB)
    {
        unmountAll(deviceNode);
        // wipeDisk is a no-op — partitionDisk's Format call wipes implicitly.
        if (!partitionDisk(deviceNode, config, dualPartition, espSizeMB)) return {};

        // Derive partition device nodes from the Block.Device property of the
        // newly created partitions via UDisks2 (avoids manual nvme/loop suffix logic).
        // UDisks2 object path for sdb1 is /org/.../block_devices/sdb1; the
        // Block.Device property holds the canonical /dev/sdb1 path.
        auto partDevNode = [&](int n) -> QString {
            // Fast path: construct the expected object path and read its Device property.
            QString sep  = deviceNode.contains(QLatin1String("nvme")) ? QStringLiteral("p") : QString();
            QString node = deviceNode + sep + QString::number(n);
            QDBusInterface blk(QStringLiteral("org.freedesktop.UDisks2"),
                               udisksBlockPath(node),
                               QStringLiteral("org.freedesktop.UDisks2.Block"),
                               QDBusConnection::systemBus());
            QByteArray dev = blk.property("Device").toByteArray();
            return dev.isEmpty() ? node : QString::fromUtf8(dev);
        };

        QStringList result;
        if (dualPartition) {
            const QString part1 = partDevNode(1);
            const QString part2 = partDevNode(2);
            if (!formatPartition(part1, FileSystem::FAT32, QStringLiteral("EFI"), 0)) return {};
            if (!formatPartition(part2, config.filesystem, config.volumeLabel, config.clusterSize)) return {};
            result << part1 << part2;
        } else {
            const QString part1 = partDevNode(1);
            if (!formatPartition(part1, config.filesystem, config.volumeLabel, config.clusterSize)) return {};
            result << part1;
        }

        emit progressChanged(60, tr("Disk preparation complete."));
        return result;
    }

    // ──────────────────────────────────────────────
    // Utilidades
    // ──────────────────────────────────────────────
    QString PartitionManager::mkfsCommand(FileSystem fs) {
        switch (fs) {
            case FileSystem::FAT32: return "mkfs.vfat";
            case FileSystem::NTFS:  return "mkfs.ntfs";
            case FileSystem::exFAT: return "mkfs.exfat";
            case FileSystem::ext4:  return "mkfs.ext4";
            case FileSystem::UDF:   return "mkudffs";
        }
        return "mkfs.vfat";
    }

    QList<uint32_t> PartitionManager::recommendedClusterSizes(FileSystem fs, uint64_t) {
        switch (fs) {
            case FileSystem::FAT32: return {512,1024,2048,4096,8192,16384,32768,65536};
            case FileSystem::NTFS:  return {512,1024,2048,4096,8192,16384,32768,65536};
            case FileSystem::exFAT: return {512,1024,2048,4096,8192,16384,32768,65536,131072,262144};
            case FileSystem::ext4:  return {1024,2048,4096};
            case FileSystem::UDF:   return {512,1024,2048,4096};
        }
        return {4096};
    }

    uint32_t PartitionManager::defaultClusterSize(FileSystem fs, uint64_t deviceSizeBytes) {
        constexpr uint64_t GB = 1024ULL * 1024 * 1024;
        switch (fs) {
            case FileSystem::FAT32:
                if (deviceSizeBytes <= 8  * GB) return 4096;
                if (deviceSizeBytes <= 16 * GB) return 8192;
                return 16384;
            case FileSystem::NTFS:  return 4096;
            case FileSystem::exFAT: return (deviceSizeBytes <= 32*GB) ? 32768u : 131072u;
            case FileSystem::ext4:  return 4096;
            case FileSystem::UDF:   return 2048;
        }
        return 4096;
    }

    // ──────────────────────────────────────────────
    // UDisks2 helpers
    // ──────────────────────────────────────────────
    QString PartitionManager::udisksBlockPath(const QString &devNode) {
        return QStringLiteral("/org/freedesktop/UDisks2/block_devices/")
               + QFileInfo(devNode).fileName();
    }

    void PartitionManager::udisksUnmount(const QString &objectPath) {
        QDBusInterface fs(QStringLiteral("org.freedesktop.UDisks2"), objectPath,
                          QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
                          QDBusConnection::systemBus());
        QVariantMap opts;
        opts[QStringLiteral("force")] = true;
        fs.call(QStringLiteral("Unmount"), opts); // errors are non-fatal (may not be mounted)
    }

    bool PartitionManager::udisksFormat(const QString &objectPath, const QString &type,
                                        const QVariantMap &opts)
    {
        QDBusInterface block(QStringLiteral("org.freedesktop.UDisks2"), objectPath,
                             QStringLiteral("org.freedesktop.UDisks2.Block"),
                             QDBusConnection::systemBus());
        block.setTimeout(300000); // 5 minutes for slow devices
        QDBusReply<void> reply = block.call(QStringLiteral("Format"), type, opts);
        if (!reply.isValid()) {
            qWarning() << "Block.Format(" << type << ") failed on" << objectPath
                       << ":" << reply.error().message();
            return false;
        }
        return true;
    }

    // Returns the device node (/dev/sdbN) of the newly created partition, or "".
    QString PartitionManager::udisksCreatePartition(const QString &tableObjectPath,
                                                    quint64 offset, quint64 size,
                                                    const QString &type, const QString &name)
    {
        QDBusInterface pt(QStringLiteral("org.freedesktop.UDisks2"), tableObjectPath,
                          QStringLiteral("org.freedesktop.UDisks2.PartitionTable"),
                          QDBusConnection::systemBus());
        pt.setTimeout(60000);
        QDBusReply<QDBusObjectPath> reply =
            pt.call(QStringLiteral("CreatePartition"),
                    offset, size, type, name, QVariantMap());
        if (!reply.isValid()) {
            qWarning() << "CreatePartition failed on" << tableObjectPath
                       << ":" << reply.error().message();
            return {};
        }
        // Read the Block.Device property to get the canonical /dev/sdbN path.
        const QString partObjectPath = reply.value().path();
        QDBusInterface blk(QStringLiteral("org.freedesktop.UDisks2"), partObjectPath,
                           QStringLiteral("org.freedesktop.UDisks2.Block"),
                           QDBusConnection::systemBus());
        QByteArray dev = blk.property("Device").toByteArray();
        return dev.isEmpty() ? QString() : QString::fromUtf8(dev);
    }

    quint64 PartitionManager::udisksDeviceSize(const QString &objectPath) {
        QDBusInterface block(QStringLiteral("org.freedesktop.UDisks2"), objectPath,
                             QStringLiteral("org.freedesktop.UDisks2.Block"),
                             QDBusConnection::systemBus());
        return block.property("Size").toULongLong();
    }

    // Returns offset + size of a partition (= start of the next partition).
    quint64 PartitionManager::udisksPartitionEnd(const QString &partObjectPath) {
        QDBusInterface part(QStringLiteral("org.freedesktop.UDisks2"), partObjectPath,
                            QStringLiteral("org.freedesktop.UDisks2.Partition"),
                            QDBusConnection::systemBus());
        const quint64 offset = part.property("Offset").toULongLong();
        const quint64 size   = part.property("Size").toULongLong();
        return offset + size;
    }

} // namespace RufusLinux
