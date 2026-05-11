/*
 * partition_manager.h — Disk partitioning and formatting via libfdisk / mkfs
 *
 * Handles creating MBR and GPT partition tables on USB devices,
 * formatting partitions (FAT32, NTFS, ext4, exFAT), and performing
 * pre-write cleanup (wipefs).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <cstdint>

namespace RufusLinux {

// ──────────────────────────────────────────────
// Enums
// ──────────────────────────────────────────────
enum class PartitionScheme {
    MBR,
    GPT,
};

enum class TargetSystem {
    BiosOnly,
    UefiOnly,
    BiosAndUefi,    // GPT with protective MBR + EFI partition
};

enum class FileSystem {
    FAT32,
    NTFS,
    UDF,
    exFAT,
    ext4,
};

struct PartitionConfig {
    PartitionScheme scheme      = PartitionScheme::GPT;
    TargetSystem    target      = TargetSystem::UefiOnly;
    FileSystem      filesystem  = FileSystem::FAT32;
    uint32_t        clusterSize = 0;        // 0 = auto
    QString         volumeLabel = "RUFUS";
};

// ──────────────────────────────────────────────
// Partition Manager
// ──────────────────────────────────────────────
class PartitionManager : public QObject {
    Q_OBJECT

public:
    explicit PartitionManager(QObject *parent = nullptr);
    ~PartitionManager() override;

    /**
     * Unmount all partitions on @p deviceNode (e.g. /dev/sdc).
     * Returns true if all partitions were successfully unmounted.
     */
    bool unmountAll(const QString &deviceNode);

    /**
     * Wipe all filesystem signatures from @p deviceNode.
     * Equivalent to `wipefs -a /dev/sdX`.
     */
    bool wipeDisk(const QString &deviceNode);

    /**
     * Create a partition table and partitions according to @p config.
     *
     * For a typical Windows ISO (UEFI + GPT):
     *   - Partition 1: FAT32 EFI system partition (entire disk)
     * For a Windows ISO with install.wim > 4 GB:
     *   - Partition 1: FAT32 EFI (1 GB)
     *   - Partition 2: NTFS (remainder)
     * For BIOS (MBR):
     *   - Partition 1: Primary, active, FAT32 or NTFS
     */
    bool partitionDisk(const QString &deviceNode, const PartitionConfig &config,
                       bool dualPartition = false, uint64_t espSizeMB = 1024);

    /**
     * Format a single partition (e.g. /dev/sdc1) with the given filesystem.
     */
    bool formatPartition(const QString &partNode, FileSystem fs,
                         const QString &label, uint32_t clusterSize = 0);

    /**
     * High-level: prepare the entire disk for writing.
     * 1. Unmount → 2. Wipe → 3. Partition → 4. Format
     * Returns the device node(s) of the formatted partition(s).
     */
    QStringList prepareDisk(const QString &deviceNode, const PartitionConfig &config,
                            bool dualPartition = false, uint64_t espSizeMB = 1024);

    /** Return the mkfs command name for a filesystem */
    static QString mkfsCommand(FileSystem fs);

    /** Recommended cluster sizes for a given filesystem and device size */
    static QList<uint32_t> recommendedClusterSizes(FileSystem fs, uint64_t deviceSizeBytes);

    /** Default cluster size for a filesystem and device size */
    static uint32_t defaultClusterSize(FileSystem fs, uint64_t deviceSizeBytes);

signals:
    void progressChanged(int percent, const QString &message);
    void errorOccurred(const QString &message);

private:
    // UDisks2 helpers — all operations requiring root go through these.
    static QString udisksBlockPath(const QString &devNode);
    void udisksUnmount(const QString &objectPath);
    bool udisksFormat(const QString &objectPath, const QString &type,
                      const QVariantMap &opts = QVariantMap());
    // Creates a partition on an existing table; returns the new partition's device node
    // (e.g. "/dev/sdb1") or "" on failure.
    QString udisksCreatePartition(const QString &tableObjectPath,
                                  quint64 offset, quint64 size,
                                  const QString &type, const QString &name);
    quint64 udisksDeviceSize(const QString &objectPath);
    // Returns offset+size of a created partition (to compute the next partition's offset).
    quint64 udisksPartitionEnd(const QString &partObjectPath);

    bool partitionMBR(const QString &deviceNode, const PartitionConfig &config,
                      bool dualPartition, uint64_t espSizeMB);
    bool partitionGPT(const QString &deviceNode, const PartitionConfig &config,
                      bool dualPartition, uint64_t espSizeMB);
};

} // namespace RufusLinux
