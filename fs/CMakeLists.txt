cmake_minimum_required(VERSION 3.17)
project(droidmount_fs C CXX)
set(MTPFS_NAME simple-mtpfs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0)
# libfuse3 from macFUSE 5 (needed for the FSKit backend), vanilla API without Darwin extensions.
add_compile_definitions(_FILE_OFFSET_BITS=64 FUSE_USE_VERSION=31 FUSE_DARWIN_ENABLE_EXTENSIONS=0)

# libmtp is built from source and linked statically: the batched listing needs its
# internal ptp_* functions. scripts/build.sh downloads and builds it.
set(LIBMTP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../build/libmtp" CACHE PATH "built libmtp source tree")
# libusb is linked statically too, so the app needs nothing from Homebrew at run time.
# scripts/build.sh builds it for macOS 26 and passes -DLIBUSB_ROOT.
set(LIBUSB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../build/libusb-install" CACHE PATH "libusb install prefix")
find_path(LIBUSB_INCLUDE libusb.h PATHS ${LIBUSB_ROOT}/include PATH_SUFFIXES libusb-1.0
          NO_DEFAULT_PATH REQUIRED)
find_library(LIBUSB_STATIC NAMES libusb-1.0.a PATHS ${LIBUSB_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
find_library(LIBFUSE3 fuse3 PATHS /usr/local/lib REQUIRED NO_DEFAULT_PATH)

file(GLOB my_sources "./src/*.cpp" "./src/*.c")
add_executable(${MTPFS_NAME} ${my_sources})
target_include_directories(${MTPFS_NAME} PUBLIC ./ /usr/local/include/fuse3
                           ${LIBMTP_SRC}/src ${LIBUSB_INCLUDE})
target_link_libraries(${MTPFS_NAME}
    ${LIBMTP_SRC}/src/.libs/libmtp.a ${LIBUSB_STATIC} ${LIBFUSE3} iconv
    "-framework IOKit" "-framework CoreFoundation" "-framework Security")
