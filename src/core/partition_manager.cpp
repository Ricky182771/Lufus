/*
 * partition_manager.cpp — Disk partitioning and formatting
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/partition_manager.h"

#include <QProcess>
#include <QDebug>
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <iostream>

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

        QStringList partitions;
        for (const auto &entry : sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (entry.startsWith(baseName))
                partitions.append("/dev/" + entry);
        partitions.prepend(deviceNode);

        bool allOk = true;
        for (const auto &part : partitions) {
            int ret = runCommand("umount", {part}, 10000);
            if (ret != 0 && ret != 32) {
                ret = runCommand("umount", {"-l", part}, 10000);
                if (ret != 0 && ret != 32) {
                    qWarning() << "Failed to unmount" << part;
                    allOk = false;
                }
            }
        }
        return allOk;
    }

    // ──────────────────────────────────────────────
    // Wipe
    // ──────────────────────────────────────────────
    bool PartitionManager::wipeDisk(const QString &deviceNode) {
        emit progressChanged(10, tr("Wiping filesystem signatures on %1...").arg(deviceNode));
        if (runCommand("wipefs", {"--all", "--force", deviceNode}, 15000) != 0) {
            emit errorOccurred(tr("Failed to wipe disk signatures on %1").arg(deviceNode));
            return false;
        }
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

        // Para MBR+Windows: necesitamos una partición FAT32 pequeña que contenga
        // bootmgr + BCD, y una NTFS grande para los archivos de instalación.
        // Para MBR+Linux con single partition: una partición activa ocupa todo.

        QProcess proc;
        proc.setProgram("sfdisk");
        proc.setArguments({"--force", deviceNode});

        QString script = "label: dos\n";
        if (dualPartition) {
            // Partición 1: FAT32 para boot (1 GB), activa (bootable)
            script += QString("size=%1M, type=0c, bootable\n").arg(espSizeMB);
            // Partición 2: NTFS/data para el resto
            QString typeHex;
            switch (config.filesystem) {
                case FileSystem::NTFS: typeHex = "07"; break;
                case FileSystem::ext4: typeHex = "83"; break;
                default:               typeHex = "0c"; break;
            }
            script += QString("type=%1\n").arg(typeHex);
        } else {
            QString typeHex;
            switch (config.filesystem) {
                case FileSystem::FAT32: typeHex = "0c"; break;
                case FileSystem::NTFS:  typeHex = "07"; break;
                case FileSystem::exFAT: typeHex = "07"; break;
                case FileSystem::ext4:  typeHex = "83"; break;
                case FileSystem::UDF:   typeHex = "07"; break;
            }
            script += QString("type=%1, bootable\n").arg(typeHex);
        }

        proc.start();
        if (!proc.waitForStarted(5000)) { emit errorOccurred(tr("Failed to start sfdisk")); return false; }
        proc.write(script.toUtf8());
        proc.closeWriteChannel();
        if (!proc.waitForFinished(30000) || proc.exitCode() != 0) {
            emit errorOccurred(tr("sfdisk failed: %1")
            .arg(QString::fromUtf8(proc.readAllStandardError())));
            return false;
        }
        runCommand("partprobe", {deviceNode}, 10000);
        QThread::msleep(800);
        return true;
    }

    bool PartitionManager::partitionGPT(const QString &deviceNode,
                                        const PartitionConfig &config,
                                        bool dualPartition, uint64_t espSizeMB)
    {
        emit progressChanged(20, tr("Creating GPT partition table on %1...").arg(deviceNode));

        // FIX: En GPT para Windows SIEMPRE creamos dos particiones:
        //   1. FAT32 ESP (EFI System Partition, tipo C12A7328...) — aquí va
        //      EFI/BOOT/BOOTx64.EFI y bootmgr. El firmware UEFI SOLO puede
        //      leer FAT32, así que esta partición es obligatoria.
        //   2. NTFS/data (tipo EBD0...) — aquí van los archivos de instalación.
        //
        // Una sola partición NTFS en GPT NUNCA arranca por UEFI porque el
        // firmware no puede leer NTFS. El código anterior creaba solo una
        // partición EBD0 para Windows, lo que hacía imposible el arranque.
        //
        // Para Linux con single partition: FAT32 o ext4 en EBD0 (el firmware
        // permisivo puede arrancar desde FAT32 en EBD0, y GRUB maneja el resto).

        QProcess proc;
        proc.setProgram("sfdisk");
        proc.setArguments({"--force", deviceNode});

        QString script = "label: gpt\n";

        if (dualPartition) {
            // Windows dual: ESP (FAT32) + DATA (NTFS)
            script += QString("size=%1M, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI System Partition\"\n")
            .arg(espSizeMB);
            QString dataType;
            switch (config.filesystem) {
                case FileSystem::ext4: dataType = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"; break;
                default:               dataType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; break;
            }
            script += QString("type=%1, name=\"Windows\"\n").arg(dataType);
        } else {
            // Single partition: always Microsoft basic data (EBD0A0A2).
            // WinPE only mounts partitions of this type as drive letters; ESP-typed
            // partitions are ignored by setup.exe. UEFI can boot from any FAT32
            // partition containing EFI/BOOT/BOOTx64.efi regardless of type.
            script += "type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"LUFUS\"\n";
        }

        proc.start();
        if (!proc.waitForStarted(5000)) { emit errorOccurred(tr("Failed to start sfdisk")); return false; }
        proc.write(script.toUtf8());
        proc.closeWriteChannel();
        if (!proc.waitForFinished(30000) || proc.exitCode() != 0) {
            emit errorOccurred(tr("sfdisk GPT partitioning failed: %1")
            .arg(QString::fromUtf8(proc.readAllStandardError()).trimmed()));
            return false;
        }
        runCommand("partprobe", {deviceNode}, 10000);
        QThread::msleep(800);
        return true;
    }

    // ──────────────────────────────────────────────
    // Format
    // ──────────────────────────────────────────────
    bool PartitionManager::formatPartition(const QString &partNode, FileSystem fs,
                                           const QString &label, uint32_t clusterSize)
    {
        emit progressChanged(40, tr("Formatting %1 as %2...").arg(partNode, mkfsCommand(fs)));

        auto truncLabel = [&](int maxLen) -> QString {
            QString s = label;
            if (s.length() > maxLen) s.truncate(maxLen);
            return s;
        };

        QStringList args;
        QString program = mkfsCommand(fs);

        switch (fs) {

            case FileSystem::FAT32: {
                QString lbl = truncLabel(11).toUpper().trimmed();
                args << "-F" << "32";
                if (!lbl.isEmpty()) args << "-n" << lbl;
                if (clusterSize > 0) args << "-s" << QString::number(clusterSize / 512);
                args << partNode;
                break;
            }

            case FileSystem::NTFS: {
                QString lbl = truncLabel(32);
                args << "-f" << "-Q";
                if (!lbl.isEmpty()) args << "-L" << lbl;
                if (clusterSize > 0) args << "-c" << QString::number(clusterSize);
                args << partNode;
                break;
            }

            case FileSystem::exFAT: {
                QString lbl = truncLabel(15);
                // Intentar con -L (versiones modernas), fallback a -n
                QStringList a1; if (!lbl.isEmpty()) a1 << "-L" << lbl;
                if (clusterSize > 0) a1 << "-c" << QString::number(clusterSize);
                a1 << partNode;
                if (runCommand(program, a1, 120000) == 0) return true;
                qWarning() << "mkfs.exfat -L failed, retrying with -n";
                QStringList a2; if (!lbl.isEmpty()) a2 << "-n" << lbl;
                if (clusterSize > 0) a2 << "-c" << QString::number(clusterSize);
                a2 << partNode;
                if (runCommand(program, a2, 120000) != 0) {
                    emit errorOccurred(tr("Formatting failed on %1").arg(partNode));
                    return false;
                }
                return true;
            }

            case FileSystem::ext4: {
                QString lbl = truncLabel(16);
                args << "-F";
                if (!lbl.isEmpty()) args << "-L" << lbl;
                args << partNode;
                break;
            }

            case FileSystem::UDF: {
                QString lbl = truncLabel(31);
                if (!lbl.isEmpty()) args << ("--label=" + lbl);
                args << "--blocksize=512" << partNode;
                if (runCommand(program, args, 120000) == 0) return true;
                qWarning() << "mkudffs failed, trying mkfs.udf";
                if (runCommand("mkfs.udf", args, 120000) != 0) {
                    emit errorOccurred(tr("Formatting failed on %1").arg(partNode));
                    return false;
                }
                return true;
            }

        }

        if (runCommand(program, args, 120000) != 0) {
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
        QStringList result;

        if (!unmountAll(deviceNode)) return result;
        if (!wipeDisk(deviceNode))   return result;
        if (!partitionDisk(deviceNode, config, dualPartition, espSizeMB)) return result;

        QString sep   = deviceNode.contains("nvme") ? "p" : "";
        QString part1 = deviceNode + sep + "1";

        if (dualPartition) {
            QString part2 = deviceNode + sep + "2";
            // Partición 1: siempre FAT32 (EFI/boot), sin label visible al usuario
            if (!formatPartition(part1, FileSystem::FAT32, "EFI", 0)) return {};
            // Partición 2: filesystem del usuario con su label
            if (!formatPartition(part2, config.filesystem, config.volumeLabel, config.clusterSize)) return {};
            result << part1 << part2;
        } else {
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

    int PartitionManager::runCommand(const QString &program, const QStringList &args, int timeoutMs) {
        qDebug() << "Running:" << program << args.join(' ');
        QProcess proc;
        proc.setProgram(program);
        proc.setArguments(args);
        proc.start();
        if (!proc.waitForStarted(5000)) { qWarning() << "Failed to start" << program; return -1; }
        if (!proc.waitForFinished(timeoutMs)) { qWarning() << program << "timed out"; proc.kill(); return -1; }
        if (proc.exitCode() != 0) {
            QString e = QString::fromUtf8(proc.readAllStandardError()).trimmed();
            qWarning() << program << "exited" << proc.exitCode() << ":" << e;
            std::cout << "ERROR: [" << program.toStdString() << "] " << e.toStdString() << std::endl;
        }
        return proc.exitCode();
    }

} // namespace RufusLinux
