cmake_minimum_required(VERSION 3.10)
project(fake-qcrilmsgtunnel C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBINDER REQUIRED libgbinder)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GLIBUTIL REQUIRED libglibutil)

include_directories(
  ${GBINDER_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
  ${GLIBUTIL_INCLUDE_DIRS}
)

link_directories(
  ${GBINDER_LIBRARY_DIRS}
  ${GLIB_LIBRARY_DIRS}
  ${GLIBUTIL_LIBRARY_DIRS}
)

add_definitions(
  ${GBINDER_CFLAGS_OTHER}
  ${GLIB_CFLAGS_OTHER}
  ${GLIBUTIL_CFLAGS_OTHER}
)

add_executable(fake-qcrilmsgtunnel
  src/main.c
  )

target_link_libraries(
  fake-qcrilmsgtunnel
  ${GBINDER_LIBRARIES}
  ${GLIB_LIBRARIES}
  ${GLIBUTIL_LIBRARIES}
)

install(TARGETS fake-qcrilmsgtunnel DESTINATION sbin)
