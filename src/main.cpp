/*
 * main.cpp — Application entry point
 *
 * Normal invocation: launches the Qt GUI (MainWindow).
 * Backend invocation (via pkexec as root):
 *   rufus-linux --write-backend <json-config-path>
 *   Reads the JSON config, runs DiskWriter, and writes PROGRESS:/ERROR: lines
 *   to stdout so the GUI process can parse them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

#include "ui/mainwindow.h"
#include "core/disk_writer.h"

using namespace RufusLinux;

// ──────────────────────────────────────────────
// Backend mode (root process invoked by pkexec)
// ──────────────────────────────────────────────
static int runBackend(int argc, char *argv[], const QString &jsonPath) {
    QCoreApplication app(argc, argv);

    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        std::cout << "ERROR:Cannot open config: " << jsonPath.toStdString() << std::endl;
        return 1;
    }
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    WriteConfig config;
    config.isoPath        = obj["isoPath"].toString();
    config.deviceNode     = obj["deviceNode"].toString();
    config.mode           = static_cast<WriteMode>(obj["mode"].toInt());
    config.dualPartition  = obj["dualPartition"].toBool();
    config.needsSplitWim  = obj["needsSplitWim"].toBool();
    config.isoType        = static_cast<IsoType>(obj["isoType"].toInt());

    config.partConfig.scheme      = static_cast<PartitionScheme>(obj["scheme"].toInt());
    config.partConfig.filesystem  = static_cast<FileSystem>(obj["filesystem"].toInt());
    config.partConfig.volumeLabel = obj["volumeLabel"].toString();
    config.partConfig.target      = static_cast<TargetSystem>(obj["target"].toInt());
    config.partConfig.clusterSize = 0;

    // Reconstruct the IsoAnalysis fields that DiskWriter actually reads at runtime
    IsoAnalysis analysis;
    analysis.filePath      = config.isoPath;
    analysis.isoType       = config.isoType;
    analysis.needsSplitWim = config.needsSplitWim;
    analysis.isValid       = true;

    DiskWriter writer;

    // Direct connections: signals are delivered synchronously in the same thread,
    // so stdout lines appear as the write proceeds without an event loop.
    QObject::connect(&writer, &DiskWriter::progressChanged,
                     [](int pct, const QString &msg) {
        std::cout << "PROGRESS:" << pct << ":" << msg.toStdString() << std::endl;
        std::cout.flush();
    });
    QObject::connect(&writer, &DiskWriter::errorOccurred,
                     [](const QString &msg) {
        std::cout << "ERROR:" << msg.toStdString() << std::endl;
        std::cout.flush();
    });

    const bool ok = writer.write(config, analysis);
    return ok ? 0 : 1;
}

// ──────────────────────────────────────────────
// Entry point
// ──────────────────────────────────────────────
int main(int argc, char *argv[]) {
    // Check for --write-backend before constructing QApplication (no display needed)
    for (int i = 1; i < argc - 1; ++i) {
        if (QString(argv[i]) == "--write-backend")
            return runBackend(argc, argv, QString(argv[i + 1]));
    }

    QApplication app(argc, argv);
    app.setApplicationName("Lufus");
    app.setApplicationVersion(APP_VERSION);
    app.setOrganizationName("Lufus");

    RufusLinux::MainWindow win;
    win.show();

    return app.exec();
}
