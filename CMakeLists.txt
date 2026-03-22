cmake_minimum_required(VERSION 3.28)

project(MotionPngTuberPlayer VERSION 0.1.0 LANGUAGES C CXX)

add_library(${PROJECT_NAME} MODULE)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" MPT_PROJECT_ROOT_C)

find_package(libobs QUIET)

set(MPT_WINDOWS_FALLBACK_OBS OFF)
if(TARGET OBS::libobs)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
elseif(WIN32)
  set(MPT_WINDOWS_FALLBACK_OBS ON)
  set(MPT_OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "OBS Studio install root for fallback builds")
  set(MPT_OBS_DLL "${MPT_OBS_ROOT}/bin/64bit/obs.dll")
  if(NOT EXISTS "${MPT_OBS_DLL}")
    message(FATAL_ERROR "Fallback build requires obs.dll at ${MPT_OBS_DLL}")
  endif()

  file(GLOB MPT_MSVC_BIN_CANDIDATES
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64"
  )
  list(SORT MPT_MSVC_BIN_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
  list(LENGTH MPT_MSVC_BIN_CANDIDATES MPT_MSVC_BIN_COUNT)
  if(MPT_MSVC_BIN_COUNT EQUAL 0)
    message(FATAL_ERROR "Fallback build requires Visual Studio Build Tools with dumpbin.exe and lib.exe")
  endif()
  list(GET MPT_MSVC_BIN_CANDIDATES 0 MPT_MSVC_BIN_DIR)

  find_program(MPT_DUMPBIN_EXE dumpbin.exe HINTS "${MPT_MSVC_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
  find_program(MPT_LIB_EXE lib.exe HINTS "${MPT_MSVC_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)

  set(MPT_GENERATED_DEF "${CMAKE_CURRENT_BINARY_DIR}/obs.def")
  set(MPT_GENERATED_LIB "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_obs_import_lib.ps1"
            -ObsDll "${MPT_OBS_DLL}"
            -Dumpbin "${MPT_DUMPBIN_EXE}"
            -LibExe "${MPT_LIB_EXE}"
            -OutDef "${MPT_GENERATED_DEF}"
            -OutLib "${MPT_GENERATED_LIB}"
    RESULT_VARIABLE MPT_IMPORT_RESULT
  )
  if(NOT MPT_IMPORT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate obs import library from installed OBS runtime")
  endif()

  target_compile_definitions(${PROJECT_NAME} PRIVATE MPT_WINDOWS_FALLBACK_OBS=1)
  target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-obs-util.c)
  target_link_libraries(${PROJECT_NAME} PRIVATE "${MPT_GENERATED_LIB}")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c @ONLY)

target_sources(
  ${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-source.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-native.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c
)

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE windowscodecs mfplat mfreadwrite mfuuid mf winmm ole32 windowsapp)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

if(WIN32)
  set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins/64bit")
else()
  set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins")
endif()

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION} RUNTIME DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/ DESTINATION data/obs-plugins/${PROJECT_NAME})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python/runtime DESTINATION data/obs-plugins/${PROJECT_NAME}/python)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/python/lipsync_core.py ${CMAKE_CURRENT_SOURCE_DIR}/python/realtime_emotion_audio.py
        DESTINATION data/obs-plugins/${PROJECT_NAME}/python)
