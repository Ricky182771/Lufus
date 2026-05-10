#include <QCoreApplication>
#include <QDebug>
#include "core/iso_analyzer.h"
#include <iostream>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <iso_path>" << std::endl;
        return 1;
    }

    RufusLinux::IsoAnalyzer analyzer;
    RufusLinux::IsoAnalysis res = analyzer.analyze(argv[1]);

    std::cout << "=== ISO Analysis Results ===" << std::endl;
    std::cout << "Path: "        << res.filePath.toStdString()    << std::endl;
    std::cout << "Valid: "       << (res.isValid ? "Yes" : "No")  << std::endl;
    std::cout << "Label: "       << res.volumeLabel.toStdString() << std::endl;
    std::cout << "Description: " << res.description.toStdString() << std::endl;
    std::cout << "Files count: " << res.allFiles.size()           << std::endl;

    // FIX: BootMode ya no es un enum con valores estáticos, sino un struct
    // con métodos hasBios() / hasUefi() / hasBoth().
    QString bootModeStr = "None";
    if (res.bootMode.hasBoth())      bootModeStr = "BIOS + UEFI";
    else if (res.bootMode.hasBios()) bootModeStr = "BIOS";
    else if (res.bootMode.hasUefi()) bootModeStr = "UEFI";

    std::cout << "Boot Mode: " << bootModeStr.toStdString() << std::endl;
    std::cout << "ISOHybrid: " << (res.isIsoHybrid ? "Yes" : "No") << std::endl;

    return 0;
}
