/* AnniAudio: static replacement for CMake-generated libmysofa config.h.
   CMAKE_INSTALL_PREFIX only feeds mysofa's default-file fallback, which we
   never trigger (we always pass an explicit SOFA path). */
#if !defined _CONFIG_H
#define _CONFIG_H
#define CMAKE_INSTALL_PREFIX "."
#define CPACK_PACKAGE_VERSION_MAJOR 1
#define CPACK_PACKAGE_VERSION_MINOR 3
#define CPACK_PACKAGE_VERSION_PATCH 3
#endif
