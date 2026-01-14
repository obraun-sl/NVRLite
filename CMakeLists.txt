cmake_minimum_required(VERSION 3.10)
project(NVRLite CXX)

set(CMAKE_CXX_STANDARD 17)
SET(CMAKE_AUTOMOC ON)
SET(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(OpenCV REQUIRED)
find_package(Qt5 5 REQUIRED COMPONENTS Core)

IF(NOT WIN32)
find_package(PkgConfig REQUIRED)
# FFmpeg (libav*) via pkg-config
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
)
ELSE()
SET(THIRD_PARTY_LIBS ${CMAKE_CURRENT_SOURCE_DIR}/ext)
include_directories("${THIRD_PARTY_LIBS}/ffmpeg_win/include")
link_directories("${THIRD_PARTY_LIBS}/ffmpeg_win/lib")
set(FFMPEG_LIBRARIES avcodec avdevice avformat avutil swscale)
ENDIF()

include_directories(${Qt5Core_INCLUDE_DIRS})
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/)

add_definitions(-std=c++17 -O3 -fPIC)

## DEBUG/ SANITIZER options
IF(NOT WIN32)
    find_package(OpenSSL REQUIRED)
    add_definitions(-Werror=return-type
        -Werror=uninitialized
        -Werror=switch
        -Werror=delete-incomplete
        -Werror=strict-overflow=1
        -Werror=maybe-uninitialized
        -Werror=sign-compare
        )


    IF (BUILD_WITH_SANITIZER)
        message("!! Building with address sanitizer and -g !!")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address -Wall -Wextra -g")
        set (CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fno-omit-frame-pointer -fsanitize=address -Wall -Wextra -g")
        add_definitions(-g -fsanitize=address -fno-omit-frame-pointer -static-libasan -Wall -Wextra)
    ENDIF()

    IF (BUILD_WITH_DEBUGINFOS)
        message("!! Building with -g !!")
        add_definitions(-g)
    ENDIF()
ENDIF()


## Add sources for executable
FILE(GLOB_RECURSE SRC_FILES src/*.c*)
FILE(GLOB_RECURSE HDR_FILES include/*.h*)

add_executable(${PROJECT_NAME} ${SRC_FILES} ${HDR_FILES})
add_custom_target(external ALL SOURCES ./ReleaseNotes.md ./README.md)

IF(NOT WIN32)
target_link_libraries(${PROJECT_NAME}
    PkgConfig::FFMPEG
    ${OpenCV_LIBS}
    Qt5::Core
    )
ELSE()
target_link_libraries(${PROJECT_NAME}
    ${FFMPEG_LIBRARIES}
    ${OpenCV_LIBS}
    Qt5::Core
    )
ENDIF()
