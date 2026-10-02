cmake_minimum_required(VERSION 3.16)

project(RfPowerMeterAssistant
    VERSION 1.0.0
    DESCRIPTION "Companion app for the USB RF Power Meter V5"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(RFPM_BUILD_TESTS "Build the protocol unit tests" ON)
option(RFPM_DEPLOY "Windows: run windeployqt after the build so the exe runs standalone" OFF)
set(RFPM_QT_MAJOR "" CACHE STRING "Force the Qt major version (5 or 6). Empty: Qt 6 if it has SerialPort, else Qt 5")

# ── Qt: 6 preferred, 5.15 supported ───────────────────────────────────────────────
if(RFPM_QT_MAJOR)
    set(QT_MAJOR ${RFPM_QT_MAJOR})
else()
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets SerialPort)
    if(Qt6_FOUND)
        set(QT_MAJOR 6)
    else()
        set(QT_MAJOR 5)
    endif()
endif()
if(QT_MAJOR EQUAL 5)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets SerialPort)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort)
endif()
message(STATUS "Building with Qt ${QT_MAJOR}")

if(MSVC)
    add_compile_options(/utf-8 /W3 /permissive-)
    # Qt 5 headers use stdext::checked_array_iterator, deprecated by recent MSVC STLs
    add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ── protocol core (plain C++, no Qt) ─────────────────────────────────────────────
add_library(rfpm_protocol STATIC src/MeterProtocol.cpp src/MeterProtocol.h)
target_include_directories(rfpm_protocol PUBLIC src)

# ── application ──────────────────────────────────────────────────────────────────
set(APP_SOURCES
    src/main.cpp
    src/Compat.h
    src/MainWindow.cpp      src/MainWindow.h
    src/MeterLink.cpp       src/MeterLink.h
    src/MeterSimulator.cpp  src/MeterSimulator.h
    src/SweepScope.cpp      src/SweepScope.h
    src/Theme.cpp           src/Theme.h
    src/TimeSeriesChart.cpp src/TimeSeriesChart.h
    src/Widgets.cpp         src/Widgets.h
)
if(WIN32)
    list(APPEND APP_SOURCES resources/app.rc)
endif()

add_executable(rf-powermeter-assistant WIN32 MACOSX_BUNDLE ${APP_SOURCES})
target_compile_definitions(rf-powermeter-assistant PRIVATE RFPM_VERSION="${PROJECT_VERSION}")
target_link_libraries(rf-powermeter-assistant PRIVATE
    rfpm_protocol
    Qt${QT_MAJOR}::Core Qt${QT_MAJOR}::Gui Qt${QT_MAJOR}::Widgets Qt${QT_MAJOR}::SerialPort)
if(WIN32)
    target_link_libraries(rf-powermeter-assistant PRIVATE dwmapi)
endif()

if(WIN32 AND RFPM_DEPLOY)
    get_target_property(_qmake Qt${QT_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(_qtbin "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT windeployqt HINTS "${_qtbin}")
    if(WINDEPLOYQT)
        add_custom_command(TARGET rf-powermeter-assistant POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime "$<TARGET_FILE:rf-powermeter-assistant>"
            COMMENT "windeployqt: copying the Qt runtime next to the exe")
    endif()
endif()

# ── install ──────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS rf-powermeter-assistant
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/rf-powermeter-assistant.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/rf-powermeter-assistant.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()

# ── tests ────────────────────────────────────────────────────────────────────────
if(RFPM_BUILD_TESTS)
    enable_testing()
    add_executable(rfpm_protocol_test tests/test_protocol.cpp)
    target_link_libraries(rfpm_protocol_test PRIVATE rfpm_protocol)
    add_test(NAME protocol COMMAND rfpm_protocol_test)
endif()
