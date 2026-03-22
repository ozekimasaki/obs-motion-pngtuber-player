cmake_minimum_required(VERSION 3.28)

project(MotionPngTuberPlayer VERSION 0.1.7 LANGUAGES C CXX)

add_library(${PROJECT_NAME} MODULE)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" MPT_PROJECT_ROOT_C)

find_package(libobs QUIET)
find_package(PkgConfig QUIET)

set(MPT_WINDOWS_FALLBACK_OBS OFF)
set(MPT_PKGCONFIG_LIBOBS OFF)
set(MPT_MACOS_FALLBACK_OBS OFF)
set(MPT_LOCAL_OBS_FALLBACK OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(MPT_LIBOBS QUIET libobs)
  if(NOT WIN32)
    pkg_check_modules(MPT_FFMPEG QUIET libavcodec libavformat libavutil libswscale)
    pkg_check_modules(MPT_PORTAUDIO QUIET portaudio-2.0)
  endif()
endif()
if(NOT WIN32)
  find_package(PNG REQUIRED)
endif()

if(TARGET OBS::libobs)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
elseif(WIN32)
  set(MPT_WINDOWS_FALLBACK_OBS ON)
  set(MPT_LOCAL_OBS_FALLBACK ON)
  set(MPT_OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "OBS Studio install root for fallback builds")
  set(MPT_OBS_DLL "${MPT_OBS_ROOT}/bin/64bit/obs.dll")
  if(NOT EXISTS "${MPT_OBS_DLL}")
    message(FATAL_ERROR "Fallback build requires obs.dll at ${MPT_OBS_DLL}")
  endif()

  set(MPT_MSVC_BIN_CANDIDATES)
  foreach(MPT_TOOL_PATH IN ITEMS "${CMAKE_AR}" "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}")
    if(MPT_TOOL_PATH)
      get_filename_component(MPT_TOOL_DIR "${MPT_TOOL_PATH}" DIRECTORY)
      list(APPEND MPT_MSVC_BIN_CANDIDATES "${MPT_TOOL_DIR}")
    endif()
  endforeach()

  if(DEFINED ENV{VCToolsInstallDir} AND NOT "$ENV{VCToolsInstallDir}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" MPT_VCTOOLS_INSTALL_DIR)
    list(APPEND MPT_MSVC_BIN_CANDIDATES "${MPT_VCTOOLS_INSTALL_DIR}/bin/Hostx64/x64")
  endif()

  if(DEFINED ENV{VCINSTALLDIR} AND NOT "$ENV{VCINSTALLDIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCINSTALLDIR}" MPT_VCINSTALLDIR_C)
    file(GLOB MPT_VCINSTALL_BIN_CANDIDATES "${MPT_VCINSTALLDIR_C}/Tools/MSVC/*/bin/Hostx64/x64")
    list(APPEND MPT_MSVC_BIN_CANDIDATES ${MPT_VCINSTALL_BIN_CANDIDATES})
  endif()

  find_program(MPT_VSWHERE_EXE vswhere.exe
    HINTS
      "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
      "C:/Program Files/Microsoft Visual Studio/Installer"
  )
  if(MPT_VSWHERE_EXE)
    execute_process(
      COMMAND "${MPT_VSWHERE_EXE}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      OUTPUT_VARIABLE MPT_VS_INSTALL_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(MPT_VS_INSTALL_PATH)
      file(TO_CMAKE_PATH "${MPT_VS_INSTALL_PATH}" MPT_VS_INSTALL_PATH_C)
      file(GLOB MPT_VSWHERE_BIN_CANDIDATES "${MPT_VS_INSTALL_PATH_C}/VC/Tools/MSVC/*/bin/Hostx64/x64")
      list(APPEND MPT_MSVC_BIN_CANDIDATES ${MPT_VSWHERE_BIN_CANDIDATES})
    endif()
  endif()

  file(GLOB MPT_LAYOUT_BIN_CANDIDATES
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/*/bin/Hostx64/x64"
    "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/*/bin/Hostx64/x64"
  )
  list(APPEND MPT_MSVC_BIN_CANDIDATES ${MPT_LAYOUT_BIN_CANDIDATES})
  list(REMOVE_DUPLICATES MPT_MSVC_BIN_CANDIDATES)
  list(SORT MPT_MSVC_BIN_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
  list(LENGTH MPT_MSVC_BIN_CANDIDATES MPT_MSVC_BIN_COUNT)
  if(MPT_MSVC_BIN_COUNT EQUAL 0)
    message(FATAL_ERROR "Fallback build requires Visual Studio C++ tools with dumpbin.exe and lib.exe")
  endif()

  if(CMAKE_AR MATCHES "([/\\\\]|^)lib\\.exe$")
    set(MPT_LIB_EXE "${CMAKE_AR}")
  else()
    find_program(MPT_LIB_EXE lib.exe HINTS ${MPT_MSVC_BIN_CANDIDATES} REQUIRED)
  endif()
  find_program(MPT_DUMPBIN_EXE dumpbin.exe HINTS ${MPT_MSVC_BIN_CANDIDATES} REQUIRED)

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

  target_link_libraries(${PROJECT_NAME} PRIVATE "${MPT_GENERATED_LIB}")
elseif(MPT_LIBOBS_FOUND)
  set(MPT_PKGCONFIG_LIBOBS ON)
elseif(APPLE)
  set(MPT_MACOS_FALLBACK_OBS ON)
  set(MPT_LOCAL_OBS_FALLBACK ON)
  set(MPT_MACOS_OBS_APP_ROOT "/Applications/OBS.app" CACHE PATH "OBS.app root for macOS fallback builds")
  set(MPT_MACOS_OBS_LIBRARY "" CACHE FILEPATH "Path to the libobs library for macOS fallback builds")

  if("${MPT_MACOS_OBS_LIBRARY}" STREQUAL "")
    foreach(MPT_MACOS_LIBRARY_CANDIDATE
      "${MPT_MACOS_OBS_APP_ROOT}/Contents/Frameworks/libobs.framework/libobs"
      "${MPT_MACOS_OBS_APP_ROOT}/Contents/Frameworks/libobs.framework/Versions/A/libobs"
      "${MPT_MACOS_OBS_APP_ROOT}/Contents/Frameworks/libobs.0.dylib")
      if(EXISTS "${MPT_MACOS_LIBRARY_CANDIDATE}")
        set(MPT_MACOS_OBS_LIBRARY "${MPT_MACOS_LIBRARY_CANDIDATE}")
        break()
      endif()
    endforeach()
  endif()

  if("${MPT_MACOS_OBS_LIBRARY}" STREQUAL "" OR NOT EXISTS "${MPT_MACOS_OBS_LIBRARY}")
    message(FATAL_ERROR "macOS fallback builds require MPT_MACOS_OBS_LIBRARY (or an OBS.app install with a libobs framework binary).")
  endif()

  target_link_libraries(${PROJECT_NAME} PRIVATE "${MPT_MACOS_OBS_LIBRARY}")
else()
  message(FATAL_ERROR "Non-Windows builds require libobs via CMake package or pkg-config (for example Ubuntu libobs-dev or an OBS/libobs install exposed via PKG_CONFIG_PATH or libobs_DIR). macOS fallback builds can also use MPT_MACOS_OBS_LIBRARY with an installed OBS.app.")
endif()
if(MPT_LOCAL_OBS_FALLBACK)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MPT_FALLBACK_OBS=1)
  target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-obs-util.c)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(MPT_PKGCONFIG_LIBOBS)
  target_include_directories(${PROJECT_NAME} PRIVATE ${MPT_LIBOBS_INCLUDE_DIRS})
  target_compile_options(${PROJECT_NAME} PRIVATE ${MPT_LIBOBS_CFLAGS_OTHER})
  target_link_directories(${PROJECT_NAME} PRIVATE ${MPT_LIBOBS_LIBRARY_DIRS})
endif()
if(MPT_FFMPEG_FOUND)
  target_include_directories(${PROJECT_NAME} PRIVATE ${MPT_FFMPEG_INCLUDE_DIRS})
  target_compile_options(${PROJECT_NAME} PRIVATE ${MPT_FFMPEG_CFLAGS_OTHER})
  target_link_directories(${PROJECT_NAME} PRIVATE ${MPT_FFMPEG_LIBRARY_DIRS})
elseif(NOT WIN32)
  message(FATAL_ERROR "Non-Windows builds require FFmpeg development libraries via pkg-config (libavcodec, libavformat, libavutil, libswscale).")
endif()
if(MPT_PORTAUDIO_FOUND)
  target_include_directories(${PROJECT_NAME} PRIVATE ${MPT_PORTAUDIO_INCLUDE_DIRS})
  target_compile_options(${PROJECT_NAME} PRIVATE ${MPT_PORTAUDIO_CFLAGS_OTHER})
  target_link_directories(${PROJECT_NAME} PRIVATE ${MPT_PORTAUDIO_LIBRARY_DIRS})
elseif(NOT WIN32)
  message(FATAL_ERROR "Non-Windows builds require PortAudio development libraries via pkg-config (portaudio-2.0).")
endif()
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c @ONLY)

target_sources(
  ${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-audio-backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-image-backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-video-backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-source.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-native.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c
)

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE windowscodecs mfplat mfreadwrite mfuuid mf winmm ole32 windowsapp)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE PNG::PNG ${MPT_FFMPEG_LINK_LIBRARIES} ${MPT_PORTAUDIO_LINK_LIBRARIES})
  target_link_options(${PROJECT_NAME} PRIVATE ${MPT_FFMPEG_LDFLAGS_OTHER} ${MPT_PORTAUDIO_LDFLAGS_OTHER})
endif()
if(MPT_PKGCONFIG_LIBOBS)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${MPT_LIBOBS_LINK_LIBRARIES})
  target_link_options(${PROJECT_NAME} PRIVATE ${MPT_LIBOBS_LDFLAGS_OTHER})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

if(WIN32)
  set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins/64bit")
else()
  set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins")
endif()

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION} RUNTIME DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/ DESTINATION data/obs-plugins/${PROJECT_NAME})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python/runtime DESTINATION data/obs-plugins/${PROJECT_NAME}/python
        PATTERN "__pycache__" EXCLUDE
        PATTERN "*.pyc" EXCLUDE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/python/lipsync_core.py ${CMAKE_CURRENT_SOURCE_DIR}/python/realtime_emotion_audio.py
        DESTINATION data/obs-plugins/${PROJECT_NAME}/python)
