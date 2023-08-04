cmake_minimum_required(VERSION 3.5.0)
project(sharedgl VERSION 0.1.0 LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/inc)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -rdynamic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")

IF(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
ENDIF(WIN32)

file(GLOB GLOBBED_SERVER_SOURCES CONFIGURE_DEPENDS "src/server/*.c")

# client stuff
IF(UNIX)
    file(GLOB GLOBBED_CLIENT_SOURCES CONFIGURE_DEPENDS "src/client/*.c")
    file(GLOB GLOBBED_CLIENT_P_SOURCES CONFIGURE_DEPENDS "src/client/platform/*.c")
ELSEIF(WIN32)
    file(GLOB GLOBBED_CLIENT_SOURCES CONFIGURE_DEPENDS "src/client/winmain.c" "src/client/pb.c" "src/client/glimpl.c")
    file(GLOB GLOBBED_CLIENT_P_SOURCES CONFIGURE_DEPENDS "src/client/platform/wgl.c")
ENDIF(UNIX)

# server
add_executable(sharedgl ${GLOBBED_SERVER_SOURCES})
target_link_libraries(sharedgl SDL2 epoxy)

# client
IF(UNIX)
    add_library(sharedgl-core SHARED ${GLOBBED_CLIENT_SOURCES} ${GLOBBED_CLIENT_P_SOURCES})
    target_link_libraries(sharedgl-core X11)
    set_target_properties(sharedgl-core PROPERTIES OUTPUT_NAME "GL")
    set_target_properties(sharedgl-core PROPERTIES VERSION 1)
ELSEIF(WIN32)
    add_library(sharedgl-core SHARED ${GLOBBED_CLIENT_SOURCES} ${GLOBBED_CLIENT_P_SOURCES})
    set_target_properties(sharedgl-core PROPERTIES OUTPUT_NAME "opengl32")
    target_link_libraries(sharedgl-core ${CMAKE_SOURCE_DIR}/inc/client/platform/minhook/minhook.lib)
ENDIF(UNIX)