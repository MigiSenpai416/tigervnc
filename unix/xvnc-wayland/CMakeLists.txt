if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "cmake must be invoked with the top level directory")
endif()

# Xvnc-wayland: the Wayland mode of Xvnc. Users run it as
# "Xvnc --wayland"; the X.Org built Xvnc execs this program, so it is
# installed in libexecdir rather than on PATH. wlroots is only linked
# here, never into the X server.

# Some wlroots headers include generated server protocol headers
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir
                wayland-protocols
                OUTPUT_VARIABLE WAYLAND_PROTOCOLS_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT WAYLAND_PROTOCOLS_DIR)
  message(FATAL_ERROR "wayland-protocols NOT found, needed by Xvnc-wayland")
endif()

set(WVNC_PROTOCOLS
  "xdg-shell|${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
  "wlr-layer-shell-unstable-v1|${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")
set(WVNC_PROTOCOL_HEADERS)
foreach(proto ${WVNC_PROTOCOLS})
  string(REPLACE "|" ";" proto "${proto}")
  list(GET proto 0 name)
  list(GET proto 1 xml)
  set(header ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.h)
  add_custom_command(OUTPUT ${header}
    COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${header}
    DEPENDS ${xml})
  list(APPEND WVNC_PROTOCOL_HEADERS ${header})
endforeach()

add_executable(Xvnc-wayland
  ${WVNC_PROTOCOL_HEADERS}
  compositor.c
  WaylandServerDesktop.cxx
  parameters.cxx
  xvnc-wayland.cxx
  ../w0vncserver/qnum_to_xorgevdev.c
)

target_include_directories(Xvnc-wayland PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(Xvnc-wayland PRIVATE
  WLR_USE_UNSTABLE
  WLROOTS_VERSION_STR="${WLROOTS_VERSION}")

if(WLROOTS_FOUND AND WVNC_XCB_FOUND)
  # wlroots exposes this in its pkg-config "have_xwayland" variable
  execute_process(COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=have_xwayland
                  ${WLROOTS_PKG}
                  OUTPUT_VARIABLE WLROOTS_HAVE_XWAYLAND
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(WLROOTS_HAVE_XWAYLAND STREQUAL "true")
    target_compile_definitions(Xvnc-wayland PRIVATE HAVE_XWAYLAND)
    target_link_libraries(Xvnc-wayland PRIVATE PkgConfig::WVNC_XCB)
    message(STATUS "Xvnc-wayland: Xwayland support enabled")
  else()
    message(WARNING "${WLROOTS_PKG} was built without Xwayland, X11 applications will not run in Wayland mode")
  endif()
endif()

target_link_libraries(Xvnc-wayland PRIVATE rfbserver rfb network rdr core)
target_link_libraries(Xvnc-wayland PRIVATE
  PkgConfig::WLROOTS
  PkgConfig::WAYLANDSERVER
  PkgConfig::WVNC_XKBCOMMON
  PkgConfig::WVNC_PIXMAN
  PkgConfig::WVNC_LIBDRM)

install(TARGETS Xvnc-wayland DESTINATION ${CMAKE_INSTALL_FULL_LIBEXECDIR})
