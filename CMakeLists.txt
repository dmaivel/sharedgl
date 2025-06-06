cmake_minimum_required(VERSION 3.15)
project(sharedgl VERSION 0.9.0 LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/inc)

# set(CMAKE_C_COMPILER "clang")

option(LINUX_LIB32 "Compile 32-bit libGL. Does not affect server." OFF)

IF(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Gz")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "Select MSVC runtime library.")
    set_property(GLOBAL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
ELSE()
    # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
ENDIF(WIN32)

file(GLOB GLOBBED_SERVER_SOURCES CONFIGURE_DEPENDS "src/server/*.c" "src/network/*.c" "src/client/scratch.c")

# client stuff
IF(UNIX)
    file(GLOB GLOBBED_CLIENT_SOURCES CONFIGURE_DEPENDS "src/client/*.c" "src/network/*.c")
    file(GLOB GLOBBED_CLIENT_P_SOURCES CONFIGURE_DEPENDS "src/client/platform/*.c")
ELSEIF(WIN32)
    file(GLOB GLOBBED_CLIENT_SOURCES CONFIGURE_DEPENDS "src/client/winmain.c" "src/client/pb.c" "src/client/spinlock.c" "src/client/glimpl.c" "src/client/scratch.c" "src/network/*.c")
    file(GLOB GLOBBED_CLIENT_P_SOURCES CONFIGURE_DEPENDS "src/client/platform/windrv.c")
ENDIF(UNIX)

# server
IF(UNIX)
    add_executable(sglrenderer ${GLOBBED_SERVER_SOURCES})
    target_link_libraries(sglrenderer SDL2 epoxy)
ENDIF(UNIX)

# client
IF(UNIX)
    add_library(sharedgl-core SHARED ${GLOBBED_CLIENT_SOURCES} ${GLOBBED_CLIENT_P_SOURCES})
    target_link_libraries(sharedgl-core X11)
    set_target_properties(sharedgl-core PROPERTIES OUTPUT_NAME "GL")
    set_target_properties(sharedgl-core PROPERTIES VERSION 1)
    IF(LINUX_LIB32)
        target_compile_options(sharedgl-core PRIVATE "-m32")
        target_link_options(sharedgl-core PRIVATE "-m32")
    ENDIF(LINUX_LIB32)
ELSEIF(WIN32)
    add_library(sharedgl-core SHARED ${GLOBBED_CLIENT_SOURCES} ${GLOBBED_CLIENT_P_SOURCES})
    IF(CMAKE_GENERATOR_PLATFORM MATCHES "Win32")
        set_target_properties(sharedgl-core PROPERTIES OUTPUT_NAME "sharedgl32")
        target_link_options(sharedgl-core PRIVATE /machine:x86)
    ELSE()
        set_target_properties(sharedgl-core PROPERTIES OUTPUT_NAME "sharedgl64")
        target_link_options(sharedgl-core PRIVATE /machine:x64)
    ENDIF()
ENDIF(UNIX)

# windows driver
IF(WIN32 AND WINKERNEL)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/kernel/windows/findwdk/")
    find_package(WDK REQUIRED)

    file(GLOB GLOBBED_KERNEL_SOURCES CONFIGURE_DEPENDS "kernel/windows/*.c")
    wdk_add_driver(ksgldrv KMDF 1.15 ${GLOBBED_KERNEL_SOURCES})
ENDIF(WIN32 AND WINKERNEL)
