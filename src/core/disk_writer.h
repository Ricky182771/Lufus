/*
 * disk_writer.h — ISO-to-USB writer engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>

#include <atomic>
#include <cstdint>

#include "core/iso_analyzer.h"
#include "core/partition_manager.h"

namespace RufusLinux {

enum class WriteMode {
    ISO,   // Extract ISO filesystem to USB (file copy)
    DD,    // Raw disk image write
};

struct WriteConfig {
    QString         isoPath;
    QString         deviceNode;
    WriteMode       mode          = WriteMode::ISO;
    bool            dualPartition = false;
    bool            needsSplitWim = false;
    IsoType         isoType       = IsoType::Unknown;
    PartitionConfig partConfig;
};

class DiskWriter : public QObject {
    Q_OBJECT

public:
    explicit DiskWriter(QObject *parent = nullptr);
    ~DiskWriter() override;

    /** Run the write operation synchronously. Emits writeCompleted when done. */
    bool write(const WriteConfig &config, const IsoAnalysis &analysis);

    /** Request cancellation. Thread-safe. */
    void cancel();

signals:
    void progressChanged(int percent, const QString &message);
    void errorOccurred(const QString &message);
    void writeCompleted(bool success);

private:
    bool writeDdMode(const WriteConfig &config);
    bool writeIsoMode(const WriteConfig &config, const IsoAnalysis &analysis);

    // UDisks2 helpers — all privileged I/O goes through these.
    // Returns the UDisks2 object path for a device node (/dev/sdX → /org/.../sdX).
    static QString udisksPath(const QString &devNode);
    // Mounts via UDisks2; for ISO paths creates a loop device first.
    // Returns the mount path assigned by UDisks2, or "" on failure.
    QString mountPartition(const QString &partNodeOrIso);
    // Unmounts via UDisks2; tears down the loop device if one was created.
    bool unmountPartition(const QString &mountPath);
    // Opens a block device for raw writing via UDisks2 OpenForRestore (Polkit auth).
    // Returns a dup()'d fd (caller owns it), or -1 on failure.
    int udisksOpenForRestore(const QString &objectPath);

    void    syncDevice(const QString &deviceNode);

    static QString formatSize(quint64 bytes);
    quint64 calcDirSize(const QString &dirPath, const QStringList &excludeRelPaths);

    bool copyFileChunked(const QString &src, const QString &dst,
                         const QString &displayName,
                         quint64 &bytesWritten, quint64 totalBytes,
                         int startPct, int endPct);

    bool copyDirRecursive(const QString &rootSrcDir,
                          const QString &curSrcDir,
                          const QString &curDstDir,
                          const QStringList &excludeRelPaths,
                          quint64 &bytesWritten, quint64 totalBytes,
                          int startPct, int endPct,
                          QString *failedAt);

    bool splitWimLib(const QString &wimSrc, const QString &outSwm,
                     int startPct, int endPct);

    std::atomic<bool>      m_cancelled{false};
    uint64_t               m_totalBytesWritten = 0;
    // Maps UDisks2-assigned mount path → UDisks2 object path (for unmount lookup).
    QHash<QString, QString> m_mountToObject;
    // Tracks loop device object paths so they can be deleted after unmount.
    QSet<QString>           m_activeLoops;
};

} // namespace RufusLinux
