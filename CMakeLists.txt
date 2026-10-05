cmake_minimum_required(VERSION 3.20)
project(visor3d VERSION 0.1.0 DESCRIPTION "Visor rápido de STL, 3MF y STEP" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VISOR3D_NATIVE "Optimizar para esta CPU (-march=native)" ON)
option(VISOR3D_STEP "Plugin STEP con OpenCASCADE" ON)
option(VISOR3D_THUMBNAILER "Miniaturas para Dolphin (KF6 KIO)" ON)
option(VISOR3D_TOOLS "Herramientas de prueba (generador STEP)" OFF)

include(GNUInstallDirs)
set(VISOR3D_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/visor3d")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3>=3.4)
pkg_check_modules(DEFLATE REQUIRED IMPORTED_TARGET libdeflate)

add_compile_options(-Wall -Wextra)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-plt)
    if(VISOR3D_NATIVE)
        add_compile_options(-march=native)
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT VISOR3D_IPO OUTPUT _ipo_msg LANGUAGES CXX)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${VISOR3D_IPO})
endif()

# Parsers, loaders and the CPU rasterizer, shared by the viewer and the thumbnailer.
add_library(visor3d_core STATIC
    src/core/bambu_paint.cpp
    src/core/camera.cpp
    src/core/loader.cpp
    src/core/mapped_file.cpp
    src/core/mesh.cpp
    src/core/mesh_cache.cpp
    src/core/parallel.cpp
    src/core/png_write.cpp
    src/core/step_plugin.cpp
    src/core/stl.cpp
    src/core/thumbnail.cpp
    src/core/threemf.cpp
    src/core/zip.cpp
)
set_target_properties(visor3d_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(visor3d_core PUBLIC src/core)
target_compile_definitions(visor3d_core PRIVATE V3D_PLUGIN_DIR="${VISOR3D_PLUGIN_DIR}")
target_link_libraries(visor3d_core PUBLIC PkgConfig::DEFLATE Threads::Threads ${CMAKE_DL_LIBS})

add_executable(visor3d
    src/viewer/async_loader.cpp
    src/viewer/gl_loader.cpp
    src/viewer/main.cpp
    src/viewer/renderer.cpp
)
target_link_libraries(visor3d PRIVATE visor3d_core PkgConfig::GLFW)
target_link_options(visor3d PRIVATE -Wl,--as-needed -Wl,-O1)
install(TARGETS visor3d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(VISOR3D_STEP)
    find_package(OpenCASCADE REQUIRED)
    set(VISOR3D_OCCT_LIBS TKDESTEP TKXCAF TKRWMesh TKMesh TKTopAlgo TKBRep TKLCAF TKCAF TKXSBase TKG3d TKMath TKernel)
    # MODULE: only ever dlopen()ed, so STL/3MF never load OpenCASCADE.
    add_library(visor3d_step MODULE src/step/step_occt.cpp)
    target_include_directories(visor3d_step SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(visor3d_step PRIVATE ${VISOR3D_OCCT_LIBS})
    target_link_options(visor3d_step PRIVATE -Wl,--as-needed -Wl,-O1)
    install(TARGETS visor3d_step LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/visor3d)

    if(VISOR3D_TOOLS)
        add_executable(gen_test_step tools/gen_test_step.cpp)
        target_include_directories(gen_test_step SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
        target_link_libraries(gen_test_step PRIVATE TKDESTEP TKXCAF TKFillet TKPrim TKTopAlgo TKBRep TKLCAF TKCAF TKXSBase TKG3d TKMath TKernel)
    endif()
endif()

if(VISOR3D_THUMBNAILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnailer/CMakeLists.txt")
    add_subdirectory(src/thumbnailer)
endif()

install(FILES data/visor3d.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/visor3d.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
