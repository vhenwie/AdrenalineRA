cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Please define VITASDK to point to your vita SDK path!")
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(adrenaline)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# The production release is built and tested against one Adrenaline base.
# Do not silently build against an older or newer Adrenaline tree.
set(RA_ADRENALINE_BASE_MAJOR 8)
set(RA_ADRENALINE_BASE_MINOR 0)
set(RA_ADRENALINE_BASE_MICRO 2)
set(RA_ADRENALINE_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/adrenaline_version.h")
if(NOT EXISTS "${RA_ADRENALINE_VERSION_HEADER}")
  message(FATAL_ERROR "AdrenalineRA requires the Adrenaline version header")
endif()
file(READ "${RA_ADRENALINE_VERSION_HEADER}" RA_ADRENALINE_VERSION_TEXT)
foreach(_adr_part MAJOR MINOR MICRO)
  string(REGEX MATCH "#define[ ]+ADRENALINE_VERSION_${_adr_part}[ ]+([0-9]+)"
         _adr_match "${RA_ADRENALINE_VERSION_TEXT}")
  if(CMAKE_MATCH_1 STREQUAL "")
    message(FATAL_ERROR "Cannot read ADRENALINE_VERSION_${_adr_part}")
  endif()
  set(RA_ADRENALINE_VERSION_${_adr_part} "${CMAKE_MATCH_1}")
endforeach()
if(NOT RA_ADRENALINE_VERSION_MAJOR EQUAL RA_ADRENALINE_BASE_MAJOR OR
   NOT RA_ADRENALINE_VERSION_MINOR EQUAL RA_ADRENALINE_BASE_MINOR OR
   NOT RA_ADRENALINE_VERSION_MICRO EQUAL RA_ADRENALINE_BASE_MICRO)
  message(FATAL_ERROR
    "AdrenalineRA requires Adrenaline ${RA_ADRENALINE_BASE_MAJOR}.${RA_ADRENALINE_BASE_MINOR}.${RA_ADRENALINE_BASE_MICRO}; found ${RA_ADRENALINE_VERSION_MAJOR}.${RA_ADRENALINE_VERSION_MINOR}.${RA_ADRENALINE_VERSION_MICRO}")
endif()
message(STATUS "AdrenalineRA base: Adrenaline ${RA_ADRENALINE_BASE_MAJOR}.${RA_ADRENALINE_BASE_MINOR}.${RA_ADRENALINE_BASE_MICRO}")

# The RA gate record and crypto helpers live in the repo-level core/server tree.
# All Vita targets that include adrenaline_vita.h need this include path.
include_directories(${CMAKE_SOURCE_DIR}/../../core/server)

set(NIGHTLY_CMAKE_PARAM "")
set(NIGHTLY_MAKE_PARAM "")
if(DEFINED NIGHTLY AND NIGHTLY EQUAL 1)
	set(NIGHTLY 1)
	set(NIGHTLY_CMAKE_PARAM "-DNIGHTLY=${NIGHTLY}")
	set(NIGHTLY_MAKE_PARAM "NIGHTLY=${NIGHTLY}")
endif()

if(DEFINED CEF_CMAKE AND CEF_CMAKE EQUAL 1)
  set(CEF_BUILD_CMD1 cmake -S ${CMAKE_SOURCE_DIR}/cef -B ${CMAKE_SOURCE_DIR}/cef/build -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${NIGHTLY_CMAKE_PARAM})
  set(CEF_BUILD_CMD2 cmake --build ${CMAKE_SOURCE_DIR}/cef/build -j )
  set(CEF_UPDATER_BUILD_CMD cmake --build ${CMAKE_SOURCE_DIR}/cef/build --target updater)
  set(CEF_UPDATER_PATH ${CMAKE_SOURCE_DIR}/cef/build/extra/updater/EBOOT.PBP)
else()
  set(CEF_BUILD_CMD1 make -C ${CMAKE_SOURCE_DIR}/cef ${NIGHTLY_CMAKE_PARAM} -j1 )
  set(CEF_BUILD_CMD2 "")
  set(CEF_UPDATER_BUILD_CMD make -C ${CMAKE_SOURCE_DIR}/cef/extra/updater)
  set(CEF_UPDATER_PATH ${CMAKE_SOURCE_DIR}/cef/extra/updater/EBOOT.PBP)
endif()

if(DEFINED WHOLE_PROJECT AND WHOLE_PROJECT EQUAL 1)
  set(CEF_DEP "cef")
else()
  set(CEF_DEP)
endif()

add_subdirectory(kernel)
add_subdirectory(user)
add_subdirectory(vsh)
add_subdirectory(usbdevice)
add_subdirectory(bubble)


link_directories(${CMAKE_CURRENT_BINARY_DIR}/kernel)
link_directories(${CMAKE_BINARY_DIR}/kernel)

add_custom_target(cef
	COMMAND ${CEF_BUILD_CMD1}
	COMMAND ${CEF_BUILD_CMD2}
)

add_dependencies(${PROJECT_NAME} "kernel_all")
add_dependencies(${PROJECT_NAME} "user_all")
add_dependencies(${PROJECT_NAME} "vsh_all")
add_dependencies(${PROJECT_NAME} "usbdevice_all")
add_dependencies(adrenaline_user "kernel_all" ${CEF_DEP})
add_dependencies(adrenaline_vsh "kernel_all")

add_custom_target(updater
  COMMAND bin2c ${CMAKE_CURRENT_BINARY_DIR}/user/adrenaline_user.suprx ${CMAKE_SOURCE_DIR}/cef/extra/updater/adrenaline_user.h adrenaline_user
  COMMAND bin2c ${CMAKE_CURRENT_BINARY_DIR}/kernel/adrenaline_kernel.skprx ${CMAKE_SOURCE_DIR}/cef/extra/updater/adrenaline_kernel.h adrenaline_kernel
  COMMAND bin2c ${CMAKE_CURRENT_BINARY_DIR}/vsh/adrenaline_vsh.suprx ${CMAKE_SOURCE_DIR}/cef/extra/updater/adrenaline_vsh.h adrenaline_vsh
  COMMAND bin2c ${CMAKE_SOURCE_DIR}/bubble/pkg/frame.xml ${CMAKE_SOURCE_DIR}/cef/extra/updater/frame_xml.h frame_xml
  COMMAND bin2c ${CMAKE_SOURCE_DIR}/bubble/pkg/sce_sys/livearea/contents/template.xml ${CMAKE_SOURCE_DIR}/cef/extra/updater/template_xml.h template_xml
  COMMAND ${CEF_UPDATER_BUILD_CMD}
  COMMAND mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/updater
  COMMAND cp ${CEF_UPDATER_PATH} ${CMAKE_CURRENT_BINARY_DIR}/updater
  COMMAND cp ${CMAKE_SOURCE_DIR}/cef/extra/updater/psp-updatelist.txt ${CMAKE_CURRENT_BINARY_DIR}/updater
  DEPENDS user_all kernel_all vsh_all
)

#add_dependencies(updater "kernel_all")
#add_dependencies(updater "user_all")
#add_dependencies(updater "vsh_all")
