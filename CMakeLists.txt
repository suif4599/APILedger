cmake_minimum_required(VERSION 3.20)
project(APILedger VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTING "Build tests" OFF)
if(${CMAKE_EXPORT_NO_PACKAGE_REGISTRY})
    # Passed by the nixpkgs CMake hook; no package registry is exported.
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(APILEDGER_SHARE_DIR "${CMAKE_INSTALL_PREFIX}/share/apiledger" CACHE PATH
    "Runtime directory holding presets and report_template.html")

add_executable(apiledger
    src/main.cpp
    src/db.cpp
    src/importer.cpp
    src/presets.cpp
    src/expr.cpp
    src/csv.cpp
    src/xlsx.cpp
    src/zip.cpp
    src/report.cpp
    src/paths.cpp
    src/util.cpp
    third_party/sqlite/sqlite3.c
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c
)

target_include_directories(apiledger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(apiledger PRIVATE
    APILEDGER_DEFAULT_SHARE_DIR="${APILEDGER_SHARE_DIR}"
    MINIZ_NO_STDIO
)

set_source_files_properties(
    third_party/sqlite/sqlite3.c
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_link_libraries(apiledger PRIVATE
    CURL::libcurl
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

target_compile_options(apiledger PRIVATE -Wall -Wextra)

install(TARGETS apiledger RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/report_template.html DESTINATION ${CMAKE_INSTALL_DATADIR}/apiledger)
install(DIRECTORY presets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/apiledger/presets
        FILES_MATCHING PATTERN "*.json")
