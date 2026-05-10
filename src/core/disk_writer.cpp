/*
 * disk_writer.cpp — ISO-to-USB writer engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/disk_writer.h"
#include "core/bootloader.h"

#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QTemporaryDir>
#include <QRegularExpression>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <cstring>
#include <cerrno>

namespace RufusLinux {

    static constexpr size_t DD_BUFFER_SIZE = 4 * 1024 * 1024;

    DiskWriter::DiskWriter(QObject *parent) : QObject(parent) {}
    DiskWriter::~DiskWriter() = default;
    void DiskWriter::cancel() { m_cancelled.store(true); }

    bool DiskWriter::write(const WriteConfig &config, const IsoAnalysis &analysis) {
        m_cancelled.store(false);
        m_totalBytesWritten = 0;
        bool success = (config.mode == WriteMode::DD)
        ? writeDdMode(config)
        : writeIsoMode(config, analysis);
        emit writeCompleted(success);
        return success;
    }

    // ──────────────────────────────────────────────
    // DD Mode
    // ──────────────────────────────────────────────
    bool DiskWriter::writeDdMode(const WriteConfig &config) {
        emit progressChanged(0, tr("Opening ISO for raw write..."));

        int srcFd = ::open(config.isoPath.toUtf8().constData(), O_RDONLY);
        if (srcFd < 0) { emit errorOccurred(tr("Cannot open ISO: %1").arg(strerror(errno))); return false; }

        off_t isoSize = lseek(srcFd, 0, SEEK_END);
        lseek(srcFd, 0, SEEK_SET);
        if (isoSize <= 0) { ::close(srcFd); emit errorOccurred(tr("Cannot determine ISO size")); return false; }

        int dstFd = ::open(config.deviceNode.toUtf8().constData(), O_WRONLY | O_SYNC);
        if (dstFd < 0) {
            ::close(srcFd);
            emit errorOccurred(tr("Cannot open device %1: %2").arg(config.deviceNode, strerror(errno)));
            return false;
        }

        void *buf = nullptr;
        if (posix_memalign(&buf, 4096, DD_BUFFER_SIZE) != 0) {
            ::close(srcFd); ::close(dstFd);
            emit errorOccurred(tr("Memory allocation failed")); return false;
        }

        uint64_t totalWritten = 0;
        bool success = true;
        while (totalWritten < static_cast<uint64_t>(isoSize)) {
            if (m_cancelled.load()) { emit errorOccurred(tr("Cancelled.")); success = false; break; }
            size_t toRead = DD_BUFFER_SIZE;
            if (totalWritten + toRead > static_cast<uint64_t>(isoSize))
                toRead = static_cast<size_t>(isoSize - totalWritten);
            ssize_t nRead = ::read(srcFd, buf, toRead);
            if (nRead <= 0) {
                emit errorOccurred(tr("Read error at %1: %2").arg(totalWritten).arg(strerror(errno)));
                success = false; break;
            }
            ssize_t nWritten = ::write(dstFd, buf, static_cast<size_t>(nRead));
            if (nWritten != nRead) {
                emit errorOccurred(tr("Write error at %1: %2").arg(totalWritten).arg(strerror(errno)));
                success = false; break;
            }
            totalWritten += static_cast<uint64_t>(nWritten);
            int pct = static_cast<int>(totalWritten * 100 / static_cast<uint64_t>(isoSize));
            emit progressChanged(pct, tr("Writing... %1 / %2 MB")
            .arg(totalWritten/(1024*1024)).arg(isoSize/(1024*1024)));
        }

        free(buf);
        ::close(srcFd);
        if (success) { emit progressChanged(99, tr("Syncing...")); fsync(dstFd); }
        ::close(dstFd);
        return success;
    }

    // ──────────────────────────────────────────────
    // ISO Mode
    // ──────────────────────────────────────────────
    bool DiskWriter::writeIsoMode(const WriteConfig &config, const IsoAnalysis &analysis) {
        emit progressChanged(0, tr("Preparing disk..."));

        const bool isWindows = (analysis.isoType == IsoType::WindowsInstaller ||
                                analysis.isoType == IsoType::WindowsPE);
        const bool dualPartition = !isWindows && config.dualPartition;

        PartitionConfig partConfig = config.partConfig;
        if (isWindows)
            partConfig.filesystem = FileSystem::FAT32; // single FAT32 + wimlib split

        PartitionManager pm;
        connect(&pm, &PartitionManager::progressChanged, this, &DiskWriter::progressChanged);
        connect(&pm, &PartitionManager::errorOccurred,   this, &DiskWriter::errorOccurred);

        QStringList partitions = pm.prepareDisk(config.deviceNode, partConfig, dualPartition, 1024);
        if (partitions.isEmpty()) {
            emit errorOccurred(tr("Failed to prepare disk.")); return false;
        }

        if (m_cancelled.load()) return false;

        QTemporaryDir tmpMount;
        if (!tmpMount.isValid()) {
            emit errorOccurred(tr("Cannot create temporary mount point.")); return false;
        }

        // Montar ISO (siempre read-only)
        QString isoMount = tmpMount.path() + "/iso";
        QDir().mkpath(isoMount);
        if (!mountPartition(config.isoPath, isoMount)) {
            emit errorOccurred(tr("Cannot mount ISO.")); return false;
        }

        if (isWindows) {
            // ── Windows: single FAT32 partition + wimlib WIM split ─────────────
            //
            // boot.wim (WinPE) expects install.wim/install.swm to live on the same
            // partition it booted from. The old dual-partition scheme (FAT32 boot +
            // NTFS data) broke this on Windows 11, causing STOP CODE 0xc000021a.
            //
            // Fix: single FAT32 partition for all content. If install.wim exceeds
            // the 4 GB FAT32 file-size limit, wimlib-imagex splits it into .swm
            // chunks that the Windows installer reassembles automatically.

            QString fatMount = tmpMount.path() + "/fat";
            QDir().mkpath(fatMount);
            if (!mountPartition(partitions.at(0), fatMount)) {
                unmountPartition(isoMount);
                emit errorOccurred(tr("Failed to mount FAT32 partition.")); return false;
            }

            // 1. Copy all installer files except install.wim/esd (25–60 %)
            {
                emit progressChanged(25, tr("Scanning installer files..."));
                const QStringList wimExcludes = {"sources/install.wim", "sources/install.esd"};
                const quint64 fatTotal = calcDirSize(isoMount, wimExcludes);
                quint64 fatWritten = 0;
                QString failedAt;
                if (!copyDirRecursive(isoMount, isoMount, fatMount, wimExcludes,
                                      fatWritten, fatTotal, 25, 60, &failedAt)) {
                    unmountPartition(isoMount); unmountPartition(fatMount);
                    if (!m_cancelled.load())
                        emit errorOccurred(failedAt.isEmpty()
                            ? tr("File copy failed.")
                            : tr("File copy failed: %1").arg(failedAt));
                    return false;
                }
            }

            if (m_cancelled.load()) { unmountPartition(isoMount); unmountPartition(fatMount); return false; }

            // 2. Handle install.wim / install.esd (60–88 %)
            {
                QString wimSrc = isoMount + "/sources/install.wim";
                if (!QFile::exists(wimSrc)) wimSrc = isoMount + "/sources/install.esd";

                if (QFile::exists(wimSrc)) {
                    QDir().mkpath(fatMount + "/sources");
                    if (analysis.needsSplitWim) {
                        // Exceeds 4 GB: split into .swm chunks for FAT32 compatibility
                        const QString outSwm = fatMount + "/sources/install.swm";
                        if (!splitWimLib(wimSrc, outSwm, 60, 88)) {
                            unmountPartition(isoMount); unmountPartition(fatMount);
                            return false; // error already emitted by splitWimLib
                        }
                    } else {
                        // Fits in FAT32: copy directly
                        const quint64 wimSize = static_cast<quint64>(QFileInfo(wimSrc).size());
                        const QString wimDst = fatMount + "/sources/" + QFileInfo(wimSrc).fileName();
                        const QString wimDisplay = "sources/" + QFileInfo(wimSrc).fileName();
                        quint64 wimWritten = 0;
                        if (!copyFileChunked(wimSrc, wimDst, wimDisplay,
                                             wimWritten, wimSize, 60, 88)) {
                            unmountPartition(isoMount); unmountPartition(fatMount);
                            if (!m_cancelled.load())
                                emit errorOccurred(tr("Failed to copy %1.").arg(wimDisplay));
                            return false;
                        }
                    }
                }
            }

            unmountPartition(isoMount);
            if (m_cancelled.load()) { unmountPartition(fatMount); return false; }

            emit progressChanged(90, tr("Flushing partition..."));
            ::sync();
            unmountPartition(fatMount);

        } else {
            // ── Partición única o dual no-Windows ──────────────────────────────
            QString dataPartition  = dualPartition ? partitions.at(1) : partitions.at(0);
            QString dataMountPoint = tmpMount.path() + "/data";
            QDir().mkpath(dataMountPoint);

            if (!mountPartition(dataPartition, dataMountPoint)) {
                unmountPartition(isoMount);
                emit errorOccurred(tr("Failed to mount data partition %1").arg(dataPartition));
                return false;
            }

            {
                emit progressChanged(30, tr("Scanning ISO files..."));
                const QStringList wimExcludes = {"sources/install.wim", "sources/install.esd"};
                const quint64 dataTotal = calcDirSize(isoMount, wimExcludes);
                quint64 dataWritten = 0;
                QString failedAt;
                if (!copyDirRecursive(isoMount, isoMount, dataMountPoint, wimExcludes,
                                      dataWritten, dataTotal, 30, 57, &failedAt)) {
                    unmountPartition(isoMount); unmountPartition(dataMountPoint);
                    if (!m_cancelled.load())
                        emit errorOccurred(failedAt.isEmpty()
                            ? tr("File copy failed.")
                            : tr("File copy failed: %1").arg(failedAt));
                    return false;
                }
            }

            // WIM: copiar o hacer split
            {
                QString wimSrc = isoMount + "/sources/install.wim";
                if (!QFile::exists(wimSrc)) wimSrc = isoMount + "/sources/install.esd";

                if (QFile::exists(wimSrc)) {
                    QDir().mkpath(dataMountPoint + "/sources");
                    const quint64 wimSize = static_cast<quint64>(QFileInfo(wimSrc).size());
                    const QString wimDstFile = dataMountPoint + "/sources/" + QFileInfo(wimSrc).fileName();
                    const QString wimDisplayName = "sources/" + QFileInfo(wimSrc).fileName();
                    quint64 wimWritten = 0;
                    if (!copyFileChunked(wimSrc, wimDstFile, wimDisplayName,
                                         wimWritten, wimSize, 60, 70)) {
                        if (!m_cancelled.load() && wimSize >= 4000000000ULL) {
                            // Fallback para filesystems con límite de 4 GB por archivo
                            emit progressChanged(65, tr("Splitting install.wim with wimlib..."));
                            QProcess wim;
                            wim.setProgram("wimlib-imagex");
                            wim.setArguments({"split", wimSrc,
                                dataMountPoint + "/sources/install.swm", "3800"});
                            wim.start();
                            if (!wim.waitForFinished(3600000) || wim.exitCode() != 0) {
                                unmountPartition(isoMount); unmountPartition(dataMountPoint);
                                emit errorOccurred(tr("Failed to copy install.wim: %1")
                                    .arg(QString::fromUtf8(wim.readAllStandardError()).trimmed()));
                                return false;
                            }
                        } else {
                            unmountPartition(isoMount); unmountPartition(dataMountPoint);
                            if (!m_cancelled.load())
                                emit errorOccurred(tr("Failed to copy install.wim."));
                            return false;
                        }
                    }
                }
            }

            unmountPartition(isoMount);
            if (m_cancelled.load()) { unmountPartition(dataMountPoint); return false; }

            emit progressChanged(72, tr("Flushing data partition..."));
            ::sync();
            unmountPartition(dataMountPoint);

            if (partConfig.filesystem == FileSystem::NTFS) {
                emit progressChanged(74, tr("Clearing NTFS dirty bit..."));
                QProcess ntfsfix;
                ntfsfix.setProgram("ntfsfix");
                ntfsfix.setArguments({"-d", dataPartition});
                ntfsfix.start();
                if (!ntfsfix.waitForStarted(5000))
                    qWarning() << "ntfsfix not found (non-fatal)";
                else if (!ntfsfix.waitForFinished(30000) || ntfsfix.exitCode() != 0)
                    qWarning() << "ntfsfix -d failed (non-fatal)";
            }

            // Para dual no-Windows: copiar EFI/ a la partición FAT32
            if (dualPartition) {
                emit progressChanged(76, tr("Setting up EFI/boot partition..."));
                QString efiMountPoint = tmpMount.path() + "/efi";
                QDir().mkpath(efiMountPoint);
                if (mountPartition(partitions.at(0), efiMountPoint)) {
                    QString isoMount2 = tmpMount.path() + "/iso2";
                    QDir().mkpath(isoMount2);
                    if (mountPartition(config.isoPath, isoMount2)) {
                        if (QDir(isoMount2 + "/efi").exists() || QDir(isoMount2 + "/EFI").exists()) {
                            QProcess cpEfi;
                            cpEfi.setProgram("bash");
                            cpEfi.setArguments({"-c",
                                QString("cp -a --no-preserve=ownership "
                                        "\"%1\"/[Ee][Ff][Ii] \"%2\"/ 2>/dev/null || true")
                                    .arg(isoMount2, efiMountPoint)});
                            cpEfi.start();
                            cpEfi.waitForFinished(60000);
                        }
                        unmountPartition(isoMount2);
                    }
                    unmountPartition(efiMountPoint);
                }
            }
        }

        // ── Instalar bootloader ──
        emit progressChanged(85, tr("Installing bootloader..."));

        Bootloader bl;
        connect(&bl, &Bootloader::progressChanged, this, &DiskWriter::progressChanged);
        connect(&bl, &Bootloader::errorOccurred,   this, &DiskWriter::errorOccurred);

        bool blOk = bl.install(
            config.deviceNode,
            partitions.at(0),
            tmpMount.path() + "/bl_mount",
            analysis,
            partConfig.scheme,
            partConfig.target
        );

        if (!blOk)
            qWarning() << "Bootloader installation had warnings";

        // ── Sync final ──
        emit progressChanged(95, tr("Syncing and unmounting..."));
        syncDevice(config.deviceNode);

        emit progressChanged(100, tr("Write complete!"));
        return true;
    }

    // ──────────────────────────────────────────────
    // Helpers
    // ──────────────────────────────────────────────
    bool DiskWriter::mountPartition(const QString &partNode, const QString &mountPoint) {
        if (!partNode.endsWith(".iso", Qt::CaseInsensitive)) {
            // Regular block device: straight mount.
            QProcess proc;
            proc.setProgram("mount");
            proc.setArguments({partNode, mountPoint});
            proc.start();
            if (!proc.waitForFinished(15000) || proc.exitCode() != 0) {
                qWarning() << "mount failed:" << proc.readAllStandardError();
                return false;
            }
            return true;
        }

        // ISO file: Windows 10/11 ISOs use UDF 2.5 as primary filesystem.
        // Mounting without -t picks ISO 9660 (the stub), which only exposes a
        // handful of boot files — the full installer tree lives in UDF only.
        // Try UDF first; fall back to auto-detect (ISO 9660 / Joliet).
        auto tryMount = [&](const QStringList &args) -> bool {
            QProcess p;
            p.setProgram("mount");
            p.setArguments(args);
            p.start();
            bool ok = p.waitForFinished(15000) && p.exitCode() == 0;
            if (!ok)
                qDebug() << "mount attempt failed:" << p.readAllStandardError().trimmed();
            return ok;
        };

        if (tryMount({"-t", "udf", "-o", "loop,ro", partNode, mountPoint}))
            return true;

        // Some older ISOs are Joliet/ISO 9660 only; let the kernel decide.
        if (tryMount({"-o", "loop,ro", partNode, mountPoint}))
            return true;

        qWarning() << "mountPartition: all mount attempts failed for" << partNode;
        return false;
    }

    bool DiskWriter::unmountPartition(const QString &mountPoint) {
        // Aseguramos que el punto de montaje existe antes de intentar umount
        if (!QDir(mountPoint).exists()) return true;
        QProcess proc;
        proc.setProgram("umount");
        proc.setArguments({mountPoint});
        proc.start();
        return proc.waitForFinished(10000) && proc.exitCode() == 0;
    }

    bool DiskWriter::syncDevice(const QString &deviceNode) {
        QProcess flush;
        flush.setProgram("blockdev");
        flush.setArguments({"--flushbufs", deviceNode});
        flush.start();
        flush.waitForFinished(10000);

        QProcess proc;
        proc.setProgram("sync");
        proc.start();
        return proc.waitForFinished(60000);
    }

    // ──────────────────────────────────────────────
    // Copy helpers
    // ──────────────────────────────────────────────
    QString DiskWriter::formatSize(quint64 bytes) {
        if (bytes >= quint64(1) << 30)
            return QString::number(bytes / double(quint64(1) << 30), 'f', 2) + " GB";
        if (bytes >= quint64(1) << 20)
            return QString::number(bytes / double(quint64(1) << 20), 'f', 1) + " MB";
        return QString::number(bytes / double(quint64(1) << 10), 'f', 1) + " KB";
    }

    quint64 DiskWriter::calcDirSize(const QString &dirPath, const QStringList &excludeRelPaths) {
        quint64 total = 0;
        const int baseLen = dirPath.length() + (dirPath.endsWith('/') ? 0 : 1);
        QDirIterator it(dirPath, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString fullPath = it.next();
            const QString relPath = fullPath.mid(baseLen);
            bool excluded = false;
            for (const QString &ex : excludeRelPaths) {
                if (relPath.compare(ex, Qt::CaseInsensitive) == 0 ||
                    relPath.startsWith(ex + "/", Qt::CaseInsensitive)) {
                    excluded = true; break;
                }
            }
            if (!excluded)
                total += static_cast<quint64>(it.fileInfo().size());
        }
        return total;
    }

    bool DiskWriter::copyFileChunked(const QString &src, const QString &dst,
                                      const QString &displayName,
                                      quint64 &bytesWritten, quint64 totalBytes,
                                      int startPct, int endPct) {
        QFile in(src);
        if (!in.open(QIODevice::ReadOnly)) {
            qWarning() << "[copy] cannot open source:" << src << "-" << in.errorString();
            return false;
        }
        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "[copy] cannot open dest:" << dst << "-" << out.errorString();
            return false;
        }

        const quint64 fileSize = static_cast<quint64>(in.size());
        const QString msg = tr("Copying %1 (%2)").arg(displayName, formatSize(fileSize));

        auto calcPct = [&]() -> int {
            return (totalBytes > 0)
                ? qMin(endPct, startPct + static_cast<int>(
                      bytesWritten * static_cast<quint64>(endPct - startPct) / totalBytes))
                : endPct;
        };

        static constexpr qint64 CHUNK = 16 * 1024 * 1024;
        QByteArray buf(static_cast<int>(CHUNK), Qt::Uninitialized);

        // Hint the kernel to prefetch sequentially; avoids page-cache stalls on large files.
        posix_fadvise(in.handle(), 0, 0, POSIX_FADV_SEQUENTIAL);

        // Always emit once at the start of each file so the UI shows which file is being copied.
        int lastPct = calcPct();
        emit progressChanged(lastPct, msg);

        while (!in.atEnd()) {
            if (m_cancelled.load()) return false;
            const qint64 n = in.read(buf.data(), CHUNK);
            if (n < 0) {
                qWarning() << "[copy] read error:" << src << "-" << in.errorString();
                return false;
            }
            if (n == 0) break;
            if (out.write(buf.constData(), n) != n) {
                qWarning() << "[copy] write error:" << dst << "-" << out.errorString();
                return false;
            }
            bytesWritten += static_cast<quint64>(n);
            const int pct = calcPct();
            if (pct != lastPct) {
                emit progressChanged(pct, msg);
                lastPct = pct;
            }
        }
        out.flush();
        return true;
    }

    bool DiskWriter::copyDirRecursive(const QString &rootSrcDir,
                                       const QString &curSrcDir,
                                       const QString &curDstDir,
                                       const QStringList &excludeRelPaths,
                                       quint64 &bytesWritten, quint64 totalBytes,
                                       int startPct, int endPct,
                                       QString *failedAt) {
        if (!QDir(curDstDir).exists())
            QDir().mkpath(curDstDir);

        const int rootLen = rootSrcDir.length() + (rootSrcDir.endsWith('/') ? 0 : 1);
        const auto entries = QDir(curSrcDir).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);

        for (const QFileInfo &entry : entries) {
            if (m_cancelled.load()) return false;

            const QString srcPath = entry.absoluteFilePath();
            const QString relPath = srcPath.mid(rootLen);
            const QString dstPath = curDstDir + "/" + entry.fileName();

            bool excluded = false;
            for (const QString &ex : excludeRelPaths)
                if (relPath.compare(ex, Qt::CaseInsensitive) == 0) { excluded = true; break; }
            if (excluded) continue;

            if (entry.isDir() && !entry.isSymLink()) {
                if (!copyDirRecursive(rootSrcDir, srcPath, dstPath, excludeRelPaths,
                                       bytesWritten, totalBytes, startPct, endPct, failedAt))
                    return false;
            } else {
                // FAT/exFAT/NTFS don't support symlinks — skip broken and dir symlinks.
                if (entry.isSymLink()) {
                    QFileInfo target(srcPath);
                    if (!target.exists()) {
                        qWarning() << "[copy] skipping broken symlink:" << relPath;
                        continue;
                    }
                    if (target.isDir()) {
                        qWarning() << "[copy] skipping dir symlink:" << relPath;
                        continue;
                    }
                    // File symlink with valid target: copy the file content.
                }
                if (!copyFileChunked(srcPath, dstPath, relPath,
                                      bytesWritten, totalBytes, startPct, endPct)) {
                    if (failedAt) *failedAt = relPath;
                    return false;
                }
            }
        }
        return true;
    }

    // ──────────────────────────────────────────────
    // wimlib WIM split
    // ──────────────────────────────────────────────
    bool DiskWriter::splitWimLib(const QString &wimSrc, const QString &outSwm,
                                  int startPct, int endPct) {
        emit progressChanged(startPct, tr("Splitting install.wim for FAT32 compatibility..."));

        QProcess wim;
        wim.setProgram("wimlib-imagex");
        wim.setArguments({"split", wimSrc, outSwm, "3800"});
        wim.setProcessChannelMode(QProcess::MergedChannels);
        wim.start();

        if (!wim.waitForStarted(10000)) {
            emit errorOccurred(tr("wimlib-imagex not found. Install: sudo apt install wimtools"));
            return false;
        }

        // Parse progress. wimlib-imagex split outputs lines like:
        //   "Writing split WIM part 1 of 3 ..."  → "N of M" pattern
        // Some versions also emit percentage values.
        static const QRegularExpression partRx(R"((\d+)\s+of\s+(\d+))");
        static const QRegularExpression pctRx(R"((\d{1,3})\s*%)");
        QString buf;
        int lastPct = startPct;

        while (wim.state() != QProcess::NotRunning) {
            if (m_cancelled.load()) {
                wim.kill();
                wim.waitForFinished(5000);
                return false;
            }
            wim.waitForReadyRead(250);
            buf += QString::fromUtf8(wim.readAll());

            auto mit = partRx.globalMatch(buf);
            while (mit.hasNext()) {
                auto match = mit.next();
                int cur = match.captured(1).toInt();
                int tot = match.captured(2).toInt();
                if (tot > 0) {
                    int pct = startPct + cur * (endPct - startPct) / tot;
                    if (pct != lastPct) {
                        emit progressChanged(pct,
                            tr("Splitting install.wim: part %1 of %2").arg(cur).arg(tot));
                        lastPct = pct;
                    }
                }
            }

            auto pit = pctRx.globalMatch(buf);
            while (pit.hasNext()) {
                auto match = pit.next();
                int wimPct = match.captured(1).toInt();
                int pct = startPct + wimPct * (endPct - startPct) / 100;
                if (pct != lastPct) {
                    emit progressChanged(pct, tr("Splitting install.wim... %1%").arg(wimPct));
                    lastPct = pct;
                }
            }
            buf.clear();
        }
        wim.waitForFinished(1000);

        if (wim.exitCode() != 0) {
            emit errorOccurred(tr("Failed to split install.wim (wimlib exit code %1)")
                .arg(wim.exitCode()));
            return false;
        }

        emit progressChanged(endPct, tr("install.wim split complete."));
        return true;
    }

} // namespace RufusLinux
