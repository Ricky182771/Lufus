/*
 * bootloader.h — Bootloader installation for USB drives
 *
 * Handles the post-copy bootloader setup step:
 *   - UEFI: verifies EFI/BOOT/BOOTx64.EFI is in place (already copied from ISO)
 *   - BIOS/MBR: attempts grub-install for Linux ISOs (non-fatal on failure)
 *   - Windows: bootloader files are supplied by the ISO itself; no external tool needed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>

#include "core/iso_analyzer.h"
#include "core/partition_manager.h"

namespace RufusLinux {

class Bootloader : public QObject {
    Q_OBJECT

public:
    explicit Bootloader(QObject *parent = nullptr);
    ~Bootloader() override;

    /**
     * Install / verify the bootloader on the target USB drive.
     *
     * @param deviceNode  Whole-disk device, e.g. /dev/sdc
     * @param partNode    First (boot) partition, e.g. /dev/sdc1
     * @param mountDir    Temporary directory used to mount partNode
     * @param analysis    Metadata of the source ISO
     * @param scheme      MBR or GPT
     * @param target      BiosOnly / UefiOnly / BiosAndUefi
     * @return true on success (or non-critical warning), false on hard failure
     */
    bool install(const QString      &deviceNode,
                 const QString      &partNode,
                 const QString      &mountDir,
                 const IsoAnalysis  &analysis,
                 PartitionScheme     scheme,
                 TargetSystem        target);

signals:
    void progressChanged(int percent, const QString &message);
    void errorOccurred(const QString &message);

private:
    void installGrubBios(const QString &deviceNode, const QString &mountDir);
};

} // namespace RufusLinux
