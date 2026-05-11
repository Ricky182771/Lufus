/*
 * main.cpp — Application entry point
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QApplication>
#include <QIcon>

#include "ui/mainwindow.h"

using namespace RufusLinux;

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Lufus");
    app.setApplicationVersion(APP_VERSION);
    app.setOrganizationName("Lufus");
    // Wayland uses the desktop file name to look up the window icon in the icon theme.
    // Without this, Wayland compositors show a generic "W" placeholder.
    app.setDesktopFileName("io.github.lufus.Lufus");
    app.setWindowIcon(QIcon(":/icons/lufus.svg"));

    RufusLinux::MainWindow win;
    win.show();

    return app.exec();
}
