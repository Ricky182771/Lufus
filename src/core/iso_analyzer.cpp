/*
 * iso_analyzer.cpp — ISO 9660 / El Torito / UDF image analyzer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/iso_analyzer.h"

#include <QFileInfo>
#include <QDebug>
#include <QProcess>
#include <QTemporaryDir>
#include <QDir>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <endian.h>

#ifdef HAS_WIMLIB
#include <wimlib.h>
#endif

namespace RufusLinux {

static constexpr int SECTOR_SIZE = 2048;

static QString trimIsoString(const char *buf, size_t len) {
    QString s = QString::fromLatin1(buf, static_cast<int>(len));
    return s.trimmed();
}

static bool readSector(int fd, uint32_t lba, void *buf, size_t count = 1) {
    off_t offset = static_cast<off_t>(lba) * SECTOR_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
        return false;
    ssize_t n = ::read(fd, buf, count * SECTOR_SIZE);
    return n == static_cast<ssize_t>(count * SECTOR_SIZE);
}

IsoAnalyzer::IsoAnalyzer(QObject *parent)
    : QObject(parent)
{
}

IsoAnalyzer::~IsoAnalyzer() = default;

bool IsoAnalyzer::isIsoFile(const QString &path) {
    int fd = ::open(path.toUtf8().constData(), O_RDONLY);
    if (fd < 0) return false;

    char buf[SECTOR_SIZE];
    if (!readSector(fd, 16, buf)) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return (std::memcmp(buf + 1, "CD001", 5) == 0);
}

IsoAnalysis IsoAnalyzer::analyze(const QString &path) {
    IsoAnalysis result;
    result.filePath = path;

    QFileInfo fi(path);
    if (!fi.exists() || !fi.isReadable()) {
        result.description = tr("File does not exist or is not readable.");
        return result;
    }
    result.totalSizeBytes = static_cast<uint64_t>(fi.size());

    int fd = ::open(path.toUtf8().constData(), O_RDONLY);
    if (fd < 0) {
        result.description = tr("Cannot open file.");
        return result;
    }

    emit progressChanged(5, tr("Reading volume descriptor..."));

    if (!readPrimaryVolumeDescriptor(fd, result)) {
        ::close(fd);
        result.description = tr("Not a valid ISO 9660 image.");
        return result;
    }
    result.isValid = true;

    emit progressChanged(15, tr("Checking El Torito boot record..."));
    readElToritoBootRecord(fd, result);

    emit progressChanged(25, tr("Checking ISOHybrid MBR..."));
    checkIsoHybrid(fd, result);

    emit progressChanged(35, tr("Scanning directory tree..."));
    {
        Iso9660PVD pvd;
        readSector(fd, 16, &pvd);
        uint32_t rootLba  = *reinterpret_cast<uint32_t *>(pvd.root_dir_record + 2);
        uint32_t rootSize = *reinterpret_cast<uint32_t *>(pvd.root_dir_record + 10);
        scanDirectoryTree(fd, rootLba, rootSize, "", result);
    }

    // Modern Windows 10/11 ISOs use UDF 2.5 as primary filesystem. The ISO 9660
    // stub typically has 10-30 boot files (bootmgr, BCD, boot.sdi…) but NOT
    // sources/install.wim, which lives only in the UDF partition. If the key
    // installer file is absent after the raw scan, mount via the kernel's
    // UDF/Joliet driver so the full directory tree becomes visible.
    {
        bool hasKeyInstaller = false;
        for (const auto &f : result.allFiles) {
            const QString up = f.toUpper();
            if (up == "SOURCES/INSTALL.WIM" || up == "SOURCES/INSTALL.ESD" ||
                up == "SOURCES/BOOT.WIM") {
                hasKeyInstaller = true;
                break;
            }
        }
        if (!hasKeyInstaller) {
            emit progressChanged(50, tr("Mounting ISO for extended scan (UDF/Joliet)..."));
            scanViaMount(path, result);
        }
    }

    emit progressChanged(75, tr("Classifying image..."));
    classifyImage(result);

    if (result.isoType == IsoType::WindowsInstaller || result.isoType == IsoType::WindowsPE) {
        detectWindowsVersion(fd, result, result);
    }

    emit progressChanged(90, tr("Generating summary..."));
    generateDescription(result);

    ::close(fd);

    emit progressChanged(100, tr("Analysis complete."));
    return result;
}

bool IsoAnalyzer::readPrimaryVolumeDescriptor(int fd, IsoAnalysis &result) {
    uint8_t sector[SECTOR_SIZE];
    if (!readSector(fd, 16, sector))
        return false;

    auto *pvd = reinterpret_cast<Iso9660PVD *>(sector);
    if (pvd->type != 1 || std::memcmp(pvd->id, "CD001", 5) != 0)
        return false;

    result.volumeLabel = trimIsoString(pvd->volume_id, sizeof(pvd->volume_id));
    result.blockSize   = le16toh(pvd->logical_block_size_le);
    result.blockCount  = le32toh(pvd->vol_space_size_le);

    if (result.blockSize == 0)
        result.blockSize = SECTOR_SIZE;

    return true;
}

bool IsoAnalyzer::readElToritoBootRecord(int fd, IsoAnalysis &result) {
    uint8_t sector[SECTOR_SIZE];
    if (!readSector(fd, 17, sector))
        return false;

    auto *br = reinterpret_cast<ElToritoBootRecord *>(sector);
    if (br->type != 0 || std::memcmp(br->id, "CD001", 5) != 0)
        return false;

    if (std::memcmp(br->boot_system_id, "EL TORITO SPECIFICATION", 23) != 0)
        return false;

    result.hasElTorito = true;
    uint32_t catalogLba = le32toh(br->boot_catalog_lba);
    scanBootCatalog(fd, catalogLba, result);

    return true;
}

// FIX 3: scanBootCatalog usa los métodos type-safe de BootMode
// en lugar de OR de enums con cast explícito, que era frágil.
void IsoAnalyzer::scanBootCatalog(int fd, uint32_t catalogLba, IsoAnalysis &result) {
    uint8_t sector[SECTOR_SIZE];
    if (!readSector(fd, catalogLba, sector))
        return;

    auto *validation = reinterpret_cast<ElToritoValidation *>(sector);
    if (le16toh(validation->key) != 0xAA55)
        return;

    auto *defaultEntry = reinterpret_cast<ElToritoDefaultEntry *>(sector + 32);
    if (defaultEntry->boot_indicator == 0x88) {
        if (validation->platform_id == 0x00) {       // x86 → BIOS
            result.bootMode.setBios();
        } else if (validation->platform_id == 0xEF) { // EFI → UEFI
            result.bootMode.setUefi();
        }
    }

    int offset = 64;
    while (offset + 32 <= SECTOR_SIZE) {
        auto *sectionHdr = reinterpret_cast<ElToritoSectionHeader *>(sector + offset);
        if (sectionHdr->header_indicator != 0x90 && sectionHdr->header_indicator != 0x91)
            break;

        uint8_t  platformId  = sectionHdr->platform_id;
        uint16_t numEntries  = le16toh(sectionHdr->num_entries);
        offset += 32;

        for (uint16_t i = 0; i < numEntries && (offset + 32 <= SECTOR_SIZE); i++) {
            auto *entry = reinterpret_cast<ElToritoDefaultEntry *>(sector + offset);
            if (entry->boot_indicator == 0x88) {
                if (platformId == 0xEF) {
                    result.bootMode.setUefi();
                } else if (platformId == 0x00) {
                    result.bootMode.setBios();
                }
            }
            offset += 32;
        }

        if (sectionHdr->header_indicator == 0x91)
            break;
    }
}

bool IsoAnalyzer::checkIsoHybrid(int fd, IsoAnalysis &result) {
    uint8_t mbr[512];
    if (lseek(fd, 0, SEEK_SET) != 0)
        return false;
    if (::read(fd, mbr, 512) != 512)
        return false;

    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        bool hasCode = false;
        for (int i = 0; i < 440; i++) {
            if (mbr[i] != 0x00) {
                hasCode = true;
                break;
            }
        }
        result.isIsoHybrid = hasCode;
    }

    return result.isIsoHybrid;
}

void IsoAnalyzer::scanDirectoryTree(int fd, uint32_t lba, uint32_t size,
                                     const QString &parentPath, IsoAnalysis &result)
{
    if (size == 0 || lba == 0)
        return;

    if (result.allFiles.size() > 50000)
        return;

    std::vector<uint8_t> dirData(size);
    off_t offset = static_cast<off_t>(lba) * SECTOR_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
        return;
    if (::read(fd, dirData.data(), size) != static_cast<ssize_t>(size))
        return;

    uint32_t pos = 0;
    while (pos + 33 < size) {
        uint8_t recordLen = dirData[pos];
        if (recordLen == 0) {
            uint32_t nextSector = ((pos / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            if (nextSector >= size) break;
            pos = nextSector;
            continue;
        }
        if (pos + recordLen > size)
            break;

        uint8_t  nameLen   = dirData[pos + 32];
        uint32_t entryLba  = *reinterpret_cast<uint32_t *>(&dirData[pos + 2]);
        uint32_t entrySize = *reinterpret_cast<uint32_t *>(&dirData[pos + 10]);
        uint8_t  flags     = dirData[pos + 25];
        bool     isDir     = (flags & 0x02) != 0;

        if (nameLen == 1 && (dirData[pos + 33] == 0x00 || dirData[pos + 33] == 0x01)) {
            pos += recordLen;
            continue;
        }

        QString name = QString::fromLatin1(
            reinterpret_cast<char *>(&dirData[pos + 33]), nameLen);
        int semicolon = name.indexOf(';');
        if (semicolon >= 0)
            name.truncate(semicolon);
        if (name.endsWith('.'))
            name.chop(1);

        QString fullPath = parentPath.isEmpty() ? name : (parentPath + "/" + name);

        if (isDir) {
            scanDirectoryTree(fd, entryLba, entrySize, fullPath, result);
        } else {
            result.allFiles.append(fullPath);
            if (entrySize > result.largestFileSize)
                result.largestFileSize = entrySize;

            QString upper = fullPath.toUpper();
            if (upper == "SOURCES/INSTALL.WIM" || upper == "SOURCES/INSTALL.ESD") {
                result.installWimLba  = entryLba;
                result.installWimSize = entrySize;
            } else if (upper == "SOURCES/BOOT.WIM") {
                result.bootWimLba  = entryLba;
                result.bootWimSize = entrySize;
            }
        }

        pos += recordLen;
    }
}

void IsoAnalyzer::scanViaMount(const QString &isoPath, IsoAnalysis &result) {
    // Primary: 7z can read UDF/Joliet without root or loop devices.
    if (scanVia7z(isoPath, result))
        return;

    // Fallback: mount via loop device.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;

    // Ensure loop module is loaded (no-op if already loaded).
    { QProcess m; m.setProgram("modprobe"); m.setArguments({"loop"}); m.start(); m.waitForFinished(5000); }

    // Explicit losetup is more reliable than "mount -o loop" because the
    // kernel allocates the device name before we try to mount it.
    QString loopDev;
    {
        QProcess lo;
        lo.setProgram("losetup");
        lo.setArguments({"--find", "--show", "--read-only", isoPath});
        lo.start();
        if (lo.waitForFinished(10000) && lo.exitCode() == 0)
            loopDev = QString::fromUtf8(lo.readAllStandardOutput()).trimmed();
    }

    bool mounted = false;
    if (!loopDev.isEmpty()) {
        QProcess mnt;
        mnt.setProgram("mount");
        mnt.setArguments({"-o", "ro", loopDev, tmp.path()});
        mnt.start();
        mounted = mnt.waitForFinished(15000) && mnt.exitCode() == 0;
        if (!mounted) {
            qWarning() << "scanViaMount: mount" << loopDev << "failed:" << mnt.readAllStandardError();
            QProcess ld; ld.setProgram("losetup"); ld.setArguments({"-d", loopDev}); ld.start(); ld.waitForFinished(5000);
            loopDev.clear();
        }
    }

    if (!mounted) {
        // Last resort: legacy "mount -o loop" (works on many distros).
        QProcess mnt;
        mnt.setProgram("mount");
        mnt.setArguments({"-o", "loop,ro", isoPath, tmp.path()});
        mnt.start();
        if (!mnt.waitForFinished(15000) || mnt.exitCode() != 0) {
            qWarning() << "scanViaMount: all mount approaches failed:"
                       << QString::fromUtf8(mnt.readAllStandardError()).trimmed();
            return;
        }
        mounted = true;
    }

    const int prevCount = result.allFiles.size();
    result.allFiles.clear();
    result.largestFileSize = 0;
    scanMountedDir(tmp.path(), "", result);
    qDebug() << "scanViaMount: found" << result.allFiles.size()
             << "files via mount (was" << prevCount << ")";

    { QProcess u; u.setProgram("umount"); u.setArguments({tmp.path()}); u.start(); u.waitForFinished(10000); }
    if (!loopDev.isEmpty()) {
        QProcess ld; ld.setProgram("losetup"); ld.setArguments({"-d", loopDev}); ld.start(); ld.waitForFinished(5000);
    }
}

bool IsoAnalyzer::scanVia7z(const QString &isoPath, IsoAnalysis &result) {
    // Locate a 7-zip binary. p7zip packages ship it under different names.
    QString binary;
    for (const QString &candidate : {"7z", "7za", "7zz", "7zr"}) {
        QProcess which;
        which.setProgram("which");
        which.setArguments({candidate});
        which.start();
        if (which.waitForFinished(3000) && which.exitCode() == 0) {
            binary = candidate;
            break;
        }
    }
    if (binary.isEmpty()) {
        qDebug() << "scanVia7z: no 7z binary found";
        return false;
    }

    // "7z l -ba <iso>" outputs one line per entry, no headers:
    //   YYYY-MM-DD HH:MM:SS ATTR    SIZE COMPRESSED  Name
    // Columns split by whitespace (Qt::SkipEmptyParts handles variable spacing).
    QProcess p;
    p.setProgram(binary);
    p.setArguments({"l", "-ba", isoPath});
    p.start();
    if (!p.waitForStarted(5000)) return false;
    if (!p.waitForFinished(60000)) { p.kill(); return false; }
    // 7z exits 0 (success) or 1 (warning, still usable).
    if (p.exitCode() > 1) return false;

    QStringList newFiles;
    uint64_t newLargest = 0;

    const QString output = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString &line : output.split('\n')) {
        // Minimum: "date time attr size comp name" = 6 tokens
        QStringList tok = line.split(' ', Qt::SkipEmptyParts);
        if (tok.size() < 6) continue;

        // tok[2] = attribute string; first char 'D' means directory.
        if (tok[2].startsWith('D')) continue;

        bool ok = false;
        uint64_t size = tok[3].toULongLong(&ok);

        // Name starts at token 5; rejoin in case filename contains spaces.
        QString name = QStringList(tok.mid(5)).join(' ');
        if (name.isEmpty()) continue;

        // Normalise path separator (7z may use backslash on some ISOs).
        name.replace('\\', '/');

        newFiles.append(name);
        if (ok && size > newLargest)
            newLargest = size;
    }

    if (newFiles.isEmpty()) return false;

    qDebug() << "scanVia7z: found" << newFiles.size() << "files via 7z";
    result.allFiles    = newFiles;
    result.largestFileSize = newLargest;
    return true;
}

void IsoAnalyzer::scanMountedDir(const QString &baseDir, const QString &relPath,
                                  IsoAnalysis &result)
{
    if (result.allFiles.size() > 50000) return;

    QString absPath = relPath.isEmpty() ? baseDir : (baseDir + "/" + relPath);
    QDir dir(absPath);
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString entryRel = relPath.isEmpty() ? fi.fileName() : (relPath + "/" + fi.fileName());
        if (fi.isDir()) {
            scanMountedDir(baseDir, entryRel, result);
        } else {
            result.allFiles.append(entryRel);
            auto fsize = static_cast<uint64_t>(fi.size());
            if (fsize > result.largestFileSize)
                result.largestFileSize = fsize;
        }
    }
}

// FIX 3: classifyImage usa los métodos type-safe (setUefi/setBios)
// en lugar de OR con cast que podía producir valores fuera del enum.
void IsoAnalyzer::classifyImage(IsoAnalysis &result) {
    QStringList upperFiles;
    upperFiles.reserve(result.allFiles.size());
    for (const auto &f : result.allFiles)
        upperFiles.append(f.toUpper());

    for (const auto &f : result.allFiles) {
        QString upper = f.toUpper();
        if (upper.startsWith("EFI/BOOT/BOOT") && upper.endsWith(".EFI")) {
            result.hasEfiBootFile = true;
            result.efiBootFiles.append(f);
        }
    }
    if (result.hasEfiBootFile) {
        result.bootMode.setUefi();
    }

    for (const auto &upper : upperFiles) {
        if (upper == "SOURCES/INSTALL.WIM") result.hasInstallWim = true;
        if (upper == "SOURCES/INSTALL.ESD") result.hasInstallEsd = true;
        if (upper == "SOURCES/BOOT.WIM")    result.hasBootWim    = true;
    }

    if (result.hasInstallWim || result.hasInstallEsd) {
        result.isoType    = IsoType::WindowsInstaller;
        result.bootLoader = BootLoader::WindowsBootMgr;
        if (result.largestFileSize > 4187593113ULL)
            result.needsSplitWim = true;
    } else if (result.hasBootWim) {
        result.isoType    = IsoType::WindowsPE;
        result.bootLoader = BootLoader::WindowsBootMgr;
    }

    if (result.isoType == IsoType::Unknown) {
        for (const auto &upper : upperFiles) {
            if (upper.startsWith("CASPER/")) result.hasCasper  = true;
            if (upper.startsWith("LIVE/"))   result.hasLiveDir = true;
            if (upper.startsWith("LIVEOS/")) result.hasLiveOS  = true;
        }

        if (result.hasCasper || result.hasLiveDir || result.hasLiveOS) {
            result.isoType = IsoType::LinuxLive;
        }

        for (const auto &upper : upperFiles) {
            if (upper.contains("INSTALL") || upper.startsWith(".DISK/")) {
                if (result.isoType == IsoType::Unknown)
                    result.isoType = IsoType::LinuxInstaller;
            }
        }
    }

    if (result.bootLoader == BootLoader::Unknown) {
        for (const auto &upper : upperFiles) {
            if (upper.startsWith("ISOLINUX/") || upper == "ISOLINUX.BIN") {
                result.bootLoader = BootLoader::Isolinux;
                break;
            }
            if (upper.startsWith("SYSLINUX/") || upper == "SYSLINUX.CFG") {
                result.bootLoader = BootLoader::Syslinux;
                break;
            }
            if (upper.startsWith("BOOT/GRUB/") || upper == "GRUB/GRUB.CFG") {
                result.bootLoader = BootLoader::Grub2;
                break;
            }
        }
    }

    if (result.isoType == IsoType::Unknown)
        result.isoType = IsoType::Generic;
}

void IsoAnalyzer::detectWindowsVersion(int fd, const IsoAnalysis &result,
                                        IsoAnalysis &out)
{
#ifdef HAS_WIMLIB
    static bool wimlib_init_done = false;
    if (!wimlib_init_done) {
        if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) == 0)
            wimlib_init_done = true;
    }
    out.windowsVersion = "";
#else
    Q_UNUSED(fd);
    Q_UNUSED(result);
    out.windowsVersion = "";
#endif
}

// FIX 3: generateDescription usa los métodos hasBios/hasUefi/hasBoth
// en lugar de comparaciones directas con valores de enum como antes.
void IsoAnalyzer::generateDescription(IsoAnalysis &result) {
    QStringList parts;

    switch (result.isoType) {
    case IsoType::WindowsInstaller: parts << tr("Windows Installer ISO"); break;
    case IsoType::WindowsPE:        parts << tr("Windows PE ISO");        break;
    case IsoType::LinuxLive:        parts << tr("Linux Live ISO");        break;
    case IsoType::LinuxInstaller:   parts << tr("Linux Installer ISO");   break;
    case IsoType::FreeBSD:          parts << tr("FreeBSD ISO");           break;
    case IsoType::Generic:          parts << tr("Generic bootable ISO");  break;
    default:                        parts << tr("Unknown ISO");           break;
    }

    if (!result.volumeLabel.isEmpty())
        parts << QString("(%1)").arg(result.volumeLabel);

    if (result.bootMode.hasBoth())
        parts << "| BIOS + UEFI";
    else if (result.bootMode.hasBios())
        parts << "| BIOS only";
    else if (result.bootMode.hasUefi())
        parts << "| UEFI only";

    if (result.isIsoHybrid)
        parts << "| ISOHybrid";

    if (result.needsSplitWim)
        parts << "| install.wim > 3.9 GB (FAT32 disabled)";

    parts << QString("| %1 files").arg(result.allFiles.size());

    result.description = parts.join(' ');
}

} // namespace RufusLinux
