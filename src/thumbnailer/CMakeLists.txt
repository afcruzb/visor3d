# Dolphin/KIO thumbnail plugin. Uses the same core as the viewer: embedded
# 3MF plate images when present, otherwise the CPU rasterizer (no GL context
# inside the KIO worker, no GPU wake-up).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui)
find_package(KF6KIO 6.0 REQUIRED)
find_package(KF6CoreAddons 6.0 REQUIRED)

# Qt's own plugin directory (lib/qt6/plugins), without pulling in KDEInstallDirs.
set(KDE_INSTALL_PLUGINDIR "${QT6_INSTALL_PLUGINS}")
set(CMAKE_AUTOMOC ON)
set(BUILD_SHARED_LIBS ON)  # kcoreaddons_add_plugin builds a static plugin otherwise
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
kcoreaddons_add_plugin(visor3dthumbnailer SOURCES visor3dthumbnailer.cpp INSTALL_NAMESPACE "kf6/thumbcreator")
set_target_properties(visor3dthumbnailer PROPERTIES PREFIX "")
target_link_libraries(visor3dthumbnailer PRIVATE visor3d_core KF6::KIOGui Qt6::Gui)
target_link_options(visor3dthumbnailer PRIVATE -Wl,--as-needed -Wl,-O1)
