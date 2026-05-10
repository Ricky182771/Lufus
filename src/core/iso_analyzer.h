/*
 * iso_analyzer.h — ISO 9660 / El Torito / UDF image analyzer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>

#include <cstdint>
#include <vector>
#include <string>

namespace RufusLinux {

// ──────────────────────────────────────────────
// ISO 9660 on-disk structures (packed)
// ──────────────────────────────────────────────
#pragma pack(push, 1)

struct Iso9660PVD {
    uint8_t  type;
    char     id[5];
    uint8_t  version;
    uint8_t  unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused2[8];
    uint32_t vol_space_size_le;
    uint32_t vol_space_size_be;
    uint8_t  unused3[32];
    uint16_t vol_set_size_le;
    uint16_t vol_set_size_be;
    uint16_t vol_seq_num_le;
    uint16_t vol_seq_num_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t path_table_le;
    uint32_t opt_path_table_le;
    uint32_t path_table_be;
    uint32_t opt_path_table_be;
    uint8_t  root_dir_record[34];
    char     volume_set_id[128];
    char     publisher_id[128];
    char     data_prep_id[128];
    char     app_id[128];
    char     copyright_file[37];
    char     abstract_file[37];
    char     biblio_file[37];
    char     creation_date[17];
    char     modification_date[17];
    char     expiration_date[17];
    char     effective_date[17];
    uint8_t  file_structure_version;
    uint8_t  unused4;
    uint8_t  app_use[512];
    uint8_t  reserved[653];
};

struct ElToritoBootRecord {
    uint8_t  type;
    char     id[5];
    uint8_t  version;
    char     boot_system_id[32];
    uint8_t  unused[32];
    uint32_t boot_catalog_lba;
};

struct ElToritoValidation {
    uint8_t  header_id;
    uint8_t  platform_id;
    uint16_t reserved;
    char     manufacturer[24];
    uint16_t checksum;
    uint16_t key;
};

struct ElToritoDefaultEntry {
    uint8_t  boot_indicator;
    uint8_t  boot_media_type;
    uint16_t load_segment;
    uint8_t  system_type;
    uint8_t  unused;
    uint16_t sector_count;
    uint32_t load_rba;
};

struct ElToritoSectionHeader {
    uint8_t  header_indicator;
    uint8_t  platform_id;
    uint16_t num_entries;
    char     id_string[28];
};

#pragma pack(pop)

// ──────────────────────────────────────────────
// Enums
// ──────────────────────────────────────────────
enum class IsoType {
    Unknown,
    WindowsInstaller,
    WindowsPE,
    LinuxLive,
    LinuxInstaller,
    FreeBSD,
    Generic
};

// FIX 3: BootMode ahora usa Q_DECLARE_FLAGS para que la combinación
// bit a bit sea type-safe y explícita. Antes se hacía OR de enums sin
// ninguna garantía, lo que podía producir valores fuera del enum.
enum class BootModeFlag : uint8_t {
    None        = 0x00,
    Bios        = 0x01,
    Uefi        = 0x02,
};

// Tipo seguro para combinar flags de BootMode
struct BootMode {
    uint8_t flags = 0x00;

    BootMode() = default;
    explicit BootMode(BootModeFlag f) : flags(static_cast<uint8_t>(f)) {}

    void setBios() { flags |= static_cast<uint8_t>(BootModeFlag::Bios); }
    void setUefi() { flags |= static_cast<uint8_t>(BootModeFlag::Uefi); }

    bool hasBios() const { return (flags & static_cast<uint8_t>(BootModeFlag::Bios)) != 0; }
    bool hasUefi() const { return (flags & static_cast<uint8_t>(BootModeFlag::Uefi)) != 0; }
    bool hasBoth() const { return hasBios() && hasUefi(); }
    bool isNone()  const { return flags == 0x00; }

    bool operator==(const BootMode &o) const { return flags == o.flags; }
    bool operator!=(const BootMode &o) const { return flags != o.flags; }
};

// Constantes de conveniencia para compatibilidad con el código existente
namespace BootModes {
    inline BootMode None()        { return BootMode(); }
    inline BootMode BiosOnly()    { BootMode m; m.setBios(); return m; }
    inline BootMode UefiOnly()    { BootMode m; m.setUefi(); return m; }
    inline BootMode BiosAndUefi() { BootMode m; m.setBios(); m.setUefi(); return m; }
}

enum class BootLoader {
    Unknown,
    Grub2,
    Syslinux,
    Isolinux,
    WindowsBootMgr,
    Gummiboot,
    Refind,
};

struct IsoAnalysis {
    QString         filePath;
    QString         volumeLabel;
    uint64_t        totalSizeBytes  = 0;
    uint16_t        blockSize       = 2048;
    uint32_t        blockCount      = 0;
    bool            isValid         = false;

    IsoType         isoType         = IsoType::Unknown;
    BootMode        bootMode;                          // ahora type-safe
    BootLoader      bootLoader      = BootLoader::Unknown;
    bool            isIsoHybrid     = false;
    bool            hasElTorito     = false;
    bool            hasEfiBootFile  = false;

    bool            hasInstallWim   = false;
    bool            hasInstallEsd   = false;
    bool            hasBootWim      = false;
    uint32_t        installWimLba   = 0;
    uint32_t        installWimSize  = 0;
    uint32_t        bootWimLba      = 0;
    uint32_t        bootWimSize     = 0;
    QString         windowsVersion;
    uint64_t        largestFileSize = 0;
    bool            needsSplitWim   = false;

    bool            hasCasper       = false;
    bool            hasLiveDir      = false;
    bool            hasLiveOS       = false;
    bool            hasPersistence  = false;

    QStringList     allFiles;
    QStringList     efiBootFiles;

    QString         description;
};

// ──────────────────────────────────────────────
// Analyzer class
// ──────────────────────────────────────────────
class IsoAnalyzer : public QObject {
    Q_OBJECT

public:
    explicit IsoAnalyzer(QObject *parent = nullptr);
    ~IsoAnalyzer() override;

    IsoAnalysis analyze(const QString &path);
    static bool isIsoFile(const QString &path);

signals:
    void progressChanged(int percent, const QString &message);

private:
    bool readPrimaryVolumeDescriptor(int fd, IsoAnalysis &result);
    bool readElToritoBootRecord(int fd, IsoAnalysis &result);
    void scanBootCatalog(int fd, uint32_t catalogLba, IsoAnalysis &result);
    void scanDirectoryTree(int fd, uint32_t lba, uint32_t size,
                           const QString &parentPath, IsoAnalysis &result);
    void scanViaMount(const QString &isoPath, IsoAnalysis &result);
    bool scanVia7z(const QString &isoPath, IsoAnalysis &result);
    void scanMountedDir(const QString &baseDir, const QString &relPath, IsoAnalysis &result);
    void classifyImage(IsoAnalysis &result);
    bool checkIsoHybrid(int fd, IsoAnalysis &result);
    void detectWindowsVersion(int fd, const IsoAnalysis &result, IsoAnalysis &out);
    void generateDescription(IsoAnalysis &result);
};

} // namespace RufusLinux
