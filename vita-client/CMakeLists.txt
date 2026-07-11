cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK env var to your VitaSDK install path")
  endif()
endif()

project(DawnCord C)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# "DawnCord": renamed (with a fresh TITLEID) so it can't collide with the
# historical VitaCord homebrew someone may still have installed.
set(VITA_APP_NAME "DawnCord")
set(VITA_TITLEID  "DWNC00001")
# Bumped per release (CI derives it from the git tag): VitaShell refuses the
# install-over and the LiveArea keeps stale cached assets when it says the
# same number as the installed build.
if(NOT VITA_VERSION)
  set(VITA_VERSION "00.30")
endif()

# Compile-time defaults; at runtime ux0:data/dawncord/config.txt overrides
# them, so prebuilt VPKs work without touching these.
set(COMPANION_HOST "192.168.1.100" CACHE STRING "Companion PC LAN IP")
set(COMPANION_PORT "9100" CACHE STRING "Companion TCP port")
set(DAWNCORD_PAIR_CODE "" CACHE STRING "Pairing code (must match companion)")

add_executable(${PROJECT_NAME}
  src/main.c
  src/protocol.c
  src/network.c
  src/state.c
  src/config.c
  src/b64.c
  src/imgcache.c
  src/ui.c
  src/ime.c
  src/voice.c
  src/cjson/cJSON.c
)

target_include_directories(${PROJECT_NAME} PRIVATE include src/cjson)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  COMPANION_HOST="${COMPANION_HOST}"
  COMPANION_PORT=${COMPANION_PORT}
  DAWNCORD_PAIR_CODE="${DAWNCORD_PAIR_CODE}"
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  ScePower_stub
  ScePgf_stub
  SceCommonDialog_stub
  SceNet_stub
  SceNetCtl_stub
  ScePower_stub
  SceAudio_stub
  SceAppMgr_stub
  freetype
  bz2
  png
  jpeg
  z
  m
  c
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  # Inter (OFL, subset): crisp FreeType text instead of scaled bitmap PGF.
  FILE assets/font.ttf font.ttf
  FILE assets/font-OFL.txt font-OFL.txt
)
