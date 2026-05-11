cmake_minimum_required(VERSION 3.20)

project(Lufus
    VERSION 1.0.0
    DESCRIPTION "Lufus — Bootable USB creator for Linux"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ──────────────────────────────────────────────
# Dependencies
# ──────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets Core DBus Concurrent SvgWidgets)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUDEV REQUIRED libudev)
pkg_check_modules(LIBBLKID REQUIRED blkid)
pkg_check_modules(LIBFDISK REQUIRED fdisk)
pkg_check_modules(LIBCURL REQUIRED libcurl)

# Optional: wimlib for Windows image manipulation
pkg_check_modules(WIMLIB wimlib)

# ──────────────────────────────────────────────
# Sources
# ──────────────────────────────────────────────
set(CORE_SOURCES
    src/core/iso_analyzer.cpp
    src/core/device_manager.cpp
    src/core/partition_manager.cpp
    src/core/disk_writer.cpp
    src/core/bootloader.cpp
)

set(CORE_HEADERS
    src/core/iso_analyzer.h
    src/core/device_manager.h
    src/core/partition_manager.h
    src/core/disk_writer.h
    src/core/bootloader.h
)

set(UI_SOURCES
    src/ui/mainwindow.cpp
)

set(UI_HEADERS
    src/ui/mainwindow.h
)

set(ALL_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${CORE_HEADERS}
    ${UI_SOURCES}
    ${UI_HEADERS}
    resources/resources.qrc
)

# ──────────────────────────────────────────────
# Executable
# ──────────────────────────────────────────────
add_executable(Lufus ${ALL_SOURCES})

target_include_directories(Lufus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBUDEV_INCLUDE_DIRS}
    ${LIBBLKID_INCLUDE_DIRS}
    ${LIBFDISK_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
)

target_link_libraries(Lufus PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::DBus
    Qt6::Concurrent
    Qt6::SvgWidgets
    ${LIBUDEV_LIBRARIES}
    ${LIBBLKID_LIBRARIES}
    ${LIBFDISK_LIBRARIES}
    ${LIBCURL_LIBRARIES}
)

if(WIMLIB_FOUND)
    target_compile_definitions(Lufus PRIVATE HAS_WIMLIB)
    target_include_directories(Lufus PRIVATE ${WIMLIB_INCLUDE_DIRS})
    target_link_libraries(Lufus PRIVATE ${WIMLIB_LIBRARIES})
endif()

target_compile_definitions(Lufus PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_NAME="${PROJECT_NAME}"
)

# ──────────────────────────────────────────────
# Test Executable
# ──────────────────────────────────────────────
add_executable(test_iso src/test_iso.cpp ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(test_iso PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBUDEV_INCLUDE_DIRS}
    ${LIBBLKID_INCLUDE_DIRS}
    ${LIBFDISK_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
)

target_link_libraries(test_iso PRIVATE
    Qt6::Core
    Qt6::DBus
    ${LIBUDEV_LIBRARIES}
    ${LIBBLKID_LIBRARIES}
    ${LIBFDISK_LIBRARIES}
    ${LIBCURL_LIBRARIES}
)

if(WIMLIB_FOUND)
    target_compile_definitions(test_iso PRIVATE HAS_WIMLIB)
    target_include_directories(test_iso PRIVATE ${WIMLIB_INCLUDE_DIRS})
    target_link_libraries(test_iso PRIVATE ${WIMLIB_LIBRARIES})
endif()

# ──────────────────────────────────────────────
# Install
# ──────────────────────────────────────────────
install(TARGETS Lufus DESTINATION bin)
install(FILES resources/io.github.lufus.Lufus.desktop DESTINATION share/applications)
install(FILES resources/icons/lufus.svg
        DESTINATION share/icons/hicolor/scalable/apps
        RENAME io.github.lufus.Lufus.svg)
install(FILES polkit/io.github.lufus.Lufus.policy DESTINATION share/polkit-1/actions)
