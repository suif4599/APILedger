cmake_minimum_required(VERSION 3.20)
project(APILEDGER VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTING "Build tests" OFF)
option(APILEDGER_WASM "Build the browser module" OFF)
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

set(APILEDGER_SHARE_DIR "${CMAKE_INSTALL_PREFIX}/share/apiledger" CACHE PATH
    "Runtime directory holding presets and report_template.html")

add_library(apiledger_core STATIC
    src/db.cpp
    src/importer.cpp
    src/presets.cpp
    src/expr.cpp
    src/csv.cpp
    src/xlsx.cpp
    src/zip.cpp
    src/paths.cpp
    src/util.cpp
    src/report_data.cpp
    third_party/sqlite/sqlite3.c
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c
)

target_include_directories(apiledger_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(apiledger_core PRIVATE
    APILEDGER_DEFAULT_SHARE_DIR="${APILEDGER_SHARE_DIR}"
    MINIZ_NO_STDIO
)

set_source_files_properties(
    third_party/sqlite/sqlite3.c
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_compile_options(apiledger_core PRIVATE -Wall -Wextra)

if(APILEDGER_WASM)
    target_compile_definitions(apiledger_core PRIVATE
        SQLITE_THREADSAFE=0
        SQLITE_OMIT_LOAD_EXTENSION
    )
    target_compile_options(apiledger_core PRIVATE -fexceptions)

    add_executable(apiledger_web src/wasm.cpp)
    set_target_properties(apiledger_web PROPERTIES OUTPUT_NAME apiledger)
    target_link_libraries(apiledger_web PRIVATE apiledger_core)
    target_compile_options(apiledger_web PRIVATE -fexceptions)
    target_link_options(apiledger_web PRIVATE
        "SHELL:-fexceptions"
        "SHELL:-sDISABLE_EXCEPTION_CATCHING=0"
        "SHELL:-sMODULARIZE=1"
        "SHELL:-sEXPORT_ES6=1"
        "SHELL:-sEXPORT_NAME=createAPILedger"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sENVIRONMENT=web"
        "SHELL:-sDYNAMIC_EXECUTION=0"
        "SHELL:-sFILESYSTEM=1"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sEXPORTED_FUNCTIONS=_malloc,_free,_apiledger_init,_apiledger_load_state,_apiledger_import,_apiledger_report_json,_apiledger_export_dirty,_apiledger_apply_state,_apiledger_delete_range,_apiledger_snapshot"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAP32"
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/presets@/apiledger/presets"
    )
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)

    add_executable(apiledger
        src/main.cpp
        src/report.cpp
    )
    target_link_libraries(apiledger PRIVATE
        apiledger_core
        CURL::libcurl
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    target_compile_options(apiledger PRIVATE -Wall -Wextra)

    install(TARGETS apiledger RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES src/report_template.html web/report.js
            DESTINATION ${CMAKE_INSTALL_DATADIR}/apiledger)
    install(DIRECTORY presets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/apiledger/presets
            FILES_MATCHING PATTERN "*.json")
endif()
