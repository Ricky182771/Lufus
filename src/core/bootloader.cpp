/*
 * bootloader.cpp — Bootloader installation for USB drives
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/bootloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

#include <unistd.h>

namespace RufusLinux {

Bootloader::Bootloader(QObject *parent) : QObject(parent) {}
Bootloader::~Bootloader() = default;

// ──────────────────────────────────────────────
// install()
// ──────────────────────────────────────────────
bool Bootloader::install(const QString     &deviceNode,
                         const QString     &partNode,
                         const QString     &mountDir,
                         const IsoAnalysis &analysis,
                         PartitionScheme    scheme,
                         TargetSystem       target)
{
    Q_UNUSED(scheme)

    emit progressChanged(87, tr("Configuring bootloader..."));

    const bool isWindows = (analysis.isoType == IsoType::WindowsInstaller ||
                            analysis.isoType == IsoType::WindowsPE);

    // Mount the boot partition via UDisks2 (works inside Flatpak sandbox).
    QString mountPath;
    {
        const QString blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/")
                                  + QFileInfo(partNode).fileName();
        QDBusInterface fs(QStringLiteral("org.freedesktop.UDisks2"), blockPath,
                          QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
                          QDBusConnection::systemBus());
        fs.setTimeout(30000);
        QDBusReply<QString> reply = fs.call(QStringLiteral("Mount"), QVariantMap());
        if (reply.isValid()) {
            mountPath = reply.value();
        } else {
            qWarning() << "Bootloader: UDisks2 mount failed for" << partNode
                       << ":" << reply.error().message();
        }
    }

    // ── UEFI ──────────────────────────────────────────────────────────────
    // For both Windows and Linux ISOs, EFI/BOOT/BOOTx64.EFI is copied
    // directly from the ISO content by DiskWriter. UEFI firmware discovers
    // it via the standard fallback path regardless of partition type.
    // Secure Boot works because the signed bootloaders come from the ISO.
    if (target == TargetSystem::UefiOnly || target == TargetSystem::BiosAndUefi) {
        if (!mountPath.isEmpty()) {
            const QStringList efiCandidates = {
                mountPath + "/EFI/BOOT/BOOTx64.EFI",
                mountPath + "/EFI/BOOT/bootx64.efi",
                mountPath + "/efi/boot/bootx64.efi",
                mountPath + "/EFI/Microsoft/Boot/bootmgfw.efi",
            };
            bool efiFound = false;
            for (const QString &p : efiCandidates) {
                if (QFile::exists(p)) { efiFound = true; break; }
            }
            if (efiFound)
                emit progressChanged(88, tr("UEFI bootloader detected."));
            else
                qWarning() << "Bootloader: EFI bootloader not found on" << partNode
                           << "(non-fatal — may be in a subdirectory)";
        }
    }

    // ── BIOS / legacy ─────────────────────────────────────────────────────
    // Windows: bootmgr + BCD are already on the partition (copied from ISO).
    // The Windows installer does not require an external MBR writer.
    //
    // Linux: attempt grub-install so the drive boots on legacy BIOS systems.
    // This is non-fatal — if grub-install is unavailable or fails, UEFI boot
    // still works, and many Linux ISOs carry syslinux/isolinux MBR stubs in
    // the copied files.
    if ((target == TargetSystem::BiosOnly || target == TargetSystem::BiosAndUefi) &&
        !isWindows && !mountPath.isEmpty()) {
        installGrubBios(deviceNode, mountPath);
    }

    if (!mountPath.isEmpty()) {
        ::sync();
        const QString blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/")
                                  + QFileInfo(partNode).fileName();
        QDBusInterface fs(QStringLiteral("org.freedesktop.UDisks2"), blockPath,
                          QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
                          QDBusConnection::systemBus());
        QVariantMap opts;
        opts[QStringLiteral("force")] = true;
        fs.call(QStringLiteral("Unmount"), opts);
    }

    emit progressChanged(90, tr("Bootloader setup complete."));
    return true; // failures above are warnings; DiskWriter treats blOk=false as non-fatal
}

// ──────────────────────────────────────────────
// GRUB BIOS installation (Linux ISOs only)
// ──────────────────────────────────────────────
void Bootloader::installGrubBios(const QString &deviceNode, const QString &mountDir) {
    // Try grub2-install (Fedora/RHEL) then grub-install (Debian/Arch/openSUSE)
    for (const QString &cmd : QStringList{"grub2-install", "grub-install"}) {
        QProcess proc;
        proc.setProgram(cmd);
        proc.setArguments({
            "--target=i386-pc",
            "--boot-directory=" + mountDir + "/boot",
            "--no-floppy",
            deviceNode
        });
        proc.start();
        if (!proc.waitForStarted(5000)) continue; // command not found

        if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
            emit progressChanged(89, tr("GRUB BIOS bootloader installed."));
            return;
        }
        qWarning() << cmd << "failed (exit" << proc.exitCode() << "):"
                   << proc.readAllStandardError().trimmed();
    }
    qWarning() << "Bootloader: GRUB BIOS installation failed (non-fatal)";
}

} // namespace RufusLinux
