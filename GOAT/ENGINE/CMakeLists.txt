cmake_minimum_required(VERSION 3.22)
project(engine)

add_compile_options(-mbranch-protection=none)

add_library(engine SHARED
    src/main/cpp/native-lib.cpp
    src/main/cpp/ghost_sim.cpp
    src/main/cpp/crypto.cpp
    src/main/cpp/trajectory.cpp
    src/main/cpp/overlay.cpp
    src/main/cpp/solitaire_auto.cpp
    src/main/cpp/uno_auto.cpp
    src/main/cpp/license.cpp
    src/main/cpp/license_security.cpp
)

add_library(dobby SHARED IMPORTED)
set_target_properties(dobby PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libdobby.so)

target_include_directories(engine PRIVATE
    src/main/cpp/include
    libs/include
)

target_link_libraries(engine
    dobby
    log
    android
)

target_compile_options(engine PRIVATE
    -O2
    -DNDEBUG
    -DGOAT_PROTECTED_BUILD=1
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-exceptions
    -fno-rtti
    -fno-ident
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
    -std=c++17
    -ffunction-sections
    -fdata-sections
)

target_link_options(engine PRIVATE
    -Wl,--gc-sections
    -Wl,--icf=safe
    -Wl,--exclude-libs,ALL
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/main/cpp/engine.map
    -Wl,--strip-all
)

if(NOT ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()
