/*
 * disk_writer.h — ISO-to-USB writer engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

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

    bool    mountPartition(const QString &partNode, const QString &mountPoint);
    bool    unmountPartition(const QString &mountPoint);
    bool    syncDevice(const QString &deviceNode);

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

    std::atomic<bool> m_cancelled{false};
    uint64_t          m_totalBytesWritten = 0;
};

} // namespace RufusLinux
