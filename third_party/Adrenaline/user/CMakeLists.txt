cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_user)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(RA_DEV_BUILD "Enable developer-only RA client identity" OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/rcheevos_sources.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/mbedtls_vita_sources.cmake")

add_library(rcheevos STATIC ${RA_RCHEEVOS_SOURCES})
target_compile_options(rcheevos PRIVATE -Os)
target_include_directories(rcheevos PUBLIC ${RA_RCHEEVOS_INCLUDE_DIRS})
target_compile_definitions(rcheevos PUBLIC ${RA_RCHEEVOS_DEFINITIONS})

add_library(mbedcrypto_vita STATIC ${RA_MBEDTLS_VITA_SOURCES})
target_compile_options(mbedcrypto_vita PRIVATE -Os)
target_include_directories(mbedcrypto_vita PUBLIC ${RA_MBEDTLS_VITA_INCLUDE_DIRS})
target_include_directories(mbedcrypto_vita PRIVATE ../../../platform)
target_compile_definitions(mbedcrypto_vita PRIVATE MBEDTLS_NO_PLATFORM_ENTROPY MBEDTLS_PLATFORM_MS_TIME_ALT MBEDTLS_CONFIG_FILE="ra_mbedtls_vita_config.h")

include(ExternalProject)
find_package(Git REQUIRED)

if(DEFINED NIGHTLY AND NIGHTLY EQUAL 1)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin -DNIGHTLY=1")
else()
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

ExternalProject_Add(libvita2dfbo
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    GIT_REPOSITORY https://github.com/frangarcj/vita2dlib.git
    GIT_TAG        fbo
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
      ${GIT_EXECUTABLE} apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/vita2d-fbo-utf8.patch && true
	UPDATE_COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
      ${GIT_EXECUTABLE} reset --hard
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C libvita2d
    INSTALL_COMMAND ""
)

add_library(libvita2dfbo_libA STATIC IMPORTED GLOBAL)
set_property(TARGET libvita2dfbo_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/libvita2d.a
)

ExternalProject_Add(vitashadercollection
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    URL https://github.com/VitaArchive/vita-shader-collection/releases/download/master-0.2-v1767160600/vita-shader-collection.tar.gz
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
)

add_library(vitashadercollection_libA STATIC IMPORTED GLOBAL)
set_property(TARGET vitashadercollection_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/lib/libvitashaders.a
)


# Builds
FUNCTION(ADD_RESOURCES out_var)
  SET(result)
  FOREACH(in_f ${ARGN})
  SET(out_f "${CMAKE_CURRENT_BINARY_DIR}/${in_f}.o")
  GET_FILENAME_COMPONENT(out_dir ${out_f} DIRECTORY)
  ADD_CUSTOM_COMMAND(OUTPUT ${out_f}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${CMAKE_LINKER} -r -b binary -o ${out_f} ${in_f}
    DEPENDS ${in_f}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building resource ${out_f}"
    VERBATIM
    )
  LIST(APPEND result ${out_f})
  ENDFOREACH()
  SET(${out_var} "${result}" PARENT_SCOPE)
ENDFUNCTION()

file(GLOB res_files RELATIVE
  ${CMAKE_CURRENT_SOURCE_DIR}
  flash0/kd/*.prx
  flash0/kd/*.bin
  flash0/vsh/module/*.prx
  flash0/*.bin
)
add_resources(cef_res ${res_files})

add_executable(adrenaline_user
  ${cef_res}
  main.c
  pops.c
  titleinfo.c
  flashfs.c
  msfs.c
  menu.c
  states.c
  usb.c
  utils.c
  ../../../app/menu/ra_menu_lifecycle.c
  ../../../app/menu/ra_menu_entries_cache.c
  ../../../app/menu/ra_menu_status.c
  ../../../app/menu/ra_menu_achievements.c
  ../../../app/menu/ra_menu_rows_build.c
  ../../../app/menu/ra_menu_sets.c
  ../../../app/menu/ra_menu_game.c
  ../../../app/menu/ra_menu_leaderboards.c
  ../../../app/menu/ra_menu_account.c
  ../../../app/menu/ra_menu_settings.c
  ../../../app/menu/ra_menu_nav.c
  ../../../app/menu/ra_menu_entries.c
  ../../../app/menu/ra_menu_tooltips.c
  ../../../app/menu/ra_menu_rows.c
  ../../../app/menu/ra_menu_worker.c
  ../../../app/runtime/ra_runtime_platform.c
  ../../../app/runtime/ra_runtime_cards.c
  ../../../app/runtime/ra_runtime_lb_service.c
  ../../../app/runtime/ra_runtime_challenges.c
  ../../../app/runtime/ra_runtime_hashlib.c
  ../../../app/runtime/ra_runtime_badges.c
  ../../../app/runtime/ra_runtime_diag.c
  ../../../app/runtime/ra_runtime_plugins.c
  ../../../app/runtime/ra_runtime_tracker.c
  ../../../app/runtime/ra_runtime_clock.c
  ../../../app/runtime/ra_runtime_path.c
  ../../../app/runtime/ra_runtime_core.c
  ../../../app/runtime/ra_runtime_memory.c
  ../../../app/runtime/ra_runtime_public.c
  ../../../app/runtime/ra_runtime_events.c
  ../../../app/runtime/ra_runtime_events_lb.c
  ../../../app/runtime/ra_runtime_hold.c
  ../../../app/runtime/ra_runtime_load.c
  ../../../app/runtime/ra_runtime_disc.c
  ../../../app/runtime/ra_runtime_disc_serial.c
  ../../../app/runtime/ra_runtime_tick.c
  ../../../app/runtime/ra_runtime_boot.c
  ../../../app/runtime/ra_runtime_boot_auth.c
  ../../../app/runtime/ra_runtime_start.c
  ../../../app/runtime/ra_runtime_run.c
  ../../../app/runtime/ra_runtime_lifecycle.c
  ../../../core/netqueue/ra_netqueue.c
  ../../../core/log/ra_logqueue.c
  ../../../app/badge/ra_badge_state.c
  ../../../app/badge/ra_badge_fetch.c
  ../../../app/badge/ra_badge_texture.c
  ../../../app/badge/ra_badge_blob.c
  ../../../app/badge/ra_badge_png.c
  ../../../app/badge/ra_badge_worker.c
  ../../../app/text/ra_text.c
  ../../../app/notify/ra_notify_card.c
  ../../../app/notify/ra_notify_indicator.c
  ../../../app/notify/ra_notify_gfx.c
  ../../../app/notify/ra_notify_queue.c
  ../../../app/notify/ra_notify.c
  ../../../app/shot/ra_shot.c
  ../../../app/sound/ra_sound.c
  ../../../app/badge/ra_badge_key.c
  ../../../core/arena/ra_arena.c
  ../../../core/session/ra_client.c
  ../../../core/offline/ra_offline.c
  ../../../core/server/ra_server.c
  ../../../core/server/ra_server_json.c
  ../../../core/server/ra_server_util.c
  ../../../core/server/ra_server_payload.c
  ../../../core/server/ra_server_sets.c
  ../../../core/server/ra_server_job.c
  ../../../core/server/ra_server_lookup.c
  ../../../core/server/ra_http_diag.c
  ../../../core/server/ra_http_request.c
  ../../../core/server/ra_http_template.c
  ../../../core/server/ra_http_state.c
  ../../../core/settings/ra_vault.c
  ../../../core/session/ra_session.c
  ../../../core/session/ra_setup.c
  ../../../core/settings/ra_settings_model.c
  ../../../core/settings/ra_settings.c
  ../../../core/settings/ra_settings_policy.c
  ../../../core/settings/ra_credentials.c
  ../../../platform/ra_ime.c
  ../../../app/sound/ra_wav.c
  math_utils.c
  lz4/lz4.c
  lodepng/lodepng.c
)

add_dependencies(adrenaline_user libvita2dfbo)
add_dependencies(adrenaline_user vitashadercollection)

target_compile_options(adrenaline_user PRIVATE -Os)

# core/server/ra_gate.h defines static helpers with internal linkage; a unit that includes
# it without calling them cannot avoid the unused-function warning, and the header is
# shared with the PSP-side modules. Project sources keep their own unused statics out.
target_compile_options(adrenaline_user PRIVATE -Wno-unused-function)

if(RA_DEV_BUILD)
  target_compile_definitions(adrenaline_user PRIVATE RA_DEV_BUILD=1)
endif()

# M4 PSP hashing: rcheevos rhash plus the Vita file/CISO reader.
target_sources(adrenaline_user PRIVATE
  ../../../core/hash/ra_hash_io.c
  ../../../core/hash/ra_hash_cd.c
  ../../../core/hash/ra_hash_cd_pbp.c
  ../../../core/hash/ra_hash_ps1.c
  ../../../core/hash/ra_hash.c
  ../../../third_party/puff/puff.c
)

target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/include)
target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/includes)
target_include_directories(adrenaline_user PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../app/runtime
  ../../../app/menu
  ../../../app/notify
  ../../../app/badge
  ../../../app/shot
  ../../../app/sound
  ../../../app/text
  ../../../core/session
  ../../../core/server
  ../../../core/hash
  ../../../core/settings
  ../../../core/offline
  ../../../core/netqueue
  ../../../core/log
  ../../../core/arena
  ../../../platform
  ../../../third_party/puff
  ${RA_RCHEEVOS_INCLUDE_DIRS})
target_include_directories(adrenaline_user PRIVATE ../../../third_party/mbedtls/include)
target_link_directories(adrenaline_user PRIVATE ${CMAKE_BINARY_DIR}/kernel)
target_link_libraries(adrenaline_user
  -Wl,--wrap=malloc
  -Wl,--wrap=calloc
  -Wl,--wrap=realloc
  -Wl,--wrap=free
  AdrenalineKernel_stub
  taihen_stub
  rcheevos
  m
  mbedcrypto_vita
  SceHttp_stub_weak
  SceSsl_stub_weak
  SceNet_stub_weak
  SceNetCtl_stub_weak
  libvita2dfbo_libA
  vitashadercollection_libA
  SceRtabi_stub
  SceLibc_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceCompat_stub
  SceCtrl_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceIofilemgr_stub
  SceIme_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceMtpIfDriver_stub
  SceNet_stub_weak
  ScePgf_stub_weak
  ScePower_stub
  SceProcessmgr_stub
  SceRtc_stub
  SceScreenShot_stub_weak
  SceShellSvc_stub
  SceSysmem_stub
  SceSysmodule_stub
  SceUdcd_stub
  SceUsbstorVStorDriver_stub
  SceVshBridge_stub
  SceLiveArea_stub_weak
  SceAVConfig_stub
)

vita_create_self(adrenaline_user.suprx adrenaline_user CONFIG exports.yml UNSAFE)

#vita_create_stubs(ustubs adrenaline_user ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml)

#add_custom_target(copy
#  COMMAND cp adrenaline_user.suprx F:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(pkg
#  COMMAND cp adrenaline_user.suprx ../../bubble/pkg/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(updater
#  COMMAND bin2c adrenaline_user.suprx ../../cef/updater/adrenaline_user.h adrenaline_user
#  DEPENDS adrenaline_user.suprx
#)

add_custom_target(user_all
  ALL
  DEPENDS adrenaline_user.suprx-self
)


















































































































