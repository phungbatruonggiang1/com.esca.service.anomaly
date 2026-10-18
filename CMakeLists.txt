cmake_minimum_required(VERSION 3.16)
project(esca-anomaly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)
find_package(nlohmann_json REQUIRED)

# The audio framework, and the shared client for the AI server.
find_library(ESCA_AUDCLIENT_LIB NAMES esca-audclient REQUIRED)
find_path(ESCA_AUDIO_INCLUDE_DIR esca/audio/client.hpp REQUIRED)
find_library(ESCA_AICLIENT_LIB NAMES esca-aiclient REQUIRED)
find_path(ESCA_AI_INCLUDE_DIR esca/ai/client.hpp REQUIRED)

# --- esca-anomalyd: the live detector --------------------------------------
add_executable(esca-anomalyd src/daemon/main.cpp)
target_include_directories(esca-anomalyd PRIVATE
    ${ESCA_AUDIO_INCLUDE_DIR} ${ESCA_AI_INCLUDE_DIR})
target_link_libraries(esca-anomalyd PRIVATE
    PkgConfig::SDBUSCPP ${ESCA_AICLIENT_LIB} ${ESCA_AUDCLIENT_LIB} Threads::Threads)

# --- esca-uploader: ships the corpus ---------------------------------------
add_executable(esca-uploader src/tools/uploader.cpp)
target_include_directories(esca-uploader PRIVATE ${ESCA_AI_INCLUDE_DIR})
target_link_libraries(esca-uploader PRIVATE
    ${ESCA_AICLIENT_LIB} nlohmann_json::nlohmann_json)

install(TARGETS esca-anomalyd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS esca-uploader RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/vn.sparc.esca.Anomaly1.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES data/esca-anomalyd.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
