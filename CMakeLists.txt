cmake_minimum_required(VERSION 3.22)

project(MotionPngTuberPlayer VERSION 0.1.52 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins")
set(MPT_PLUGIN_DATA_DESTINATION "data/obs-plugins/${PROJECT_NAME}")
if(WIN32)
  set(MPT_PLUGIN_RUNTIME_DESTINATION "obs-plugins/64bit")
elseif(UNIX AND NOT APPLE)
  option(MPT_INSTALL_UBUNTU_PER_USER_PLUGIN "Install using OBS's per-user plugin layout under the chosen prefix" ON)
  if(MPT_INSTALL_UBUNTU_PER_USER_PLUGIN)
    set(MPT_PLUGIN_RUNTIME_DESTINATION "${PROJECT_NAME}/bin/64bit")
    set(MPT_PLUGIN_DATA_DESTINATION "${PROJECT_NAME}/data")
  else()
    set(MPT_PLUGIN_RUNTIME_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    set(MPT_PLUGIN_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PROJECT_NAME}")
  endif()
endif()

set(MPT_PLUGIN_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-audio-backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-image-backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-text.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-video-backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-source.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-native.cpp
)

set(MPT_VIDEO_BACKEND_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-video-backend.cpp
)

set(MPT_VIDEO_BACKEND_LIBS)
set(MPT_PLUGIN_PLATFORM_LIBS)

if(WIN32)
  list(APPEND MPT_VIDEO_BACKEND_LIBS windowscodecs mfplat mfreadwrite mfuuid mf ole32 windowsapp)
  list(APPEND MPT_PLUGIN_PLATFORM_LIBS windowscodecs mfplat mfreadwrite mfuuid mf winmm ole32 windowsapp)
else()
  find_package(PkgConfig REQUIRED)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  pkg_check_modules(MPT_FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
  find_package(ZLIB REQUIRED)
  list(APPEND MPT_VIDEO_BACKEND_LIBS PkgConfig::MPT_FFMPEG)
  list(APPEND MPT_PLUGIN_PLATFORM_LIBS PkgConfig::MPT_FFMPEG Threads::Threads)
endif()

function(mpt_configure_video_backend_target target_name)
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(${target_name} PRIVATE cxx_std_17)
  target_link_libraries(${target_name} PRIVATE ${MPT_VIDEO_BACKEND_LIBS})
endfunction()

find_package(libobs QUIET)
if(TARGET libobs::libobs AND NOT TARGET OBS::libobs)
  add_library(OBS::libobs INTERFACE IMPORTED)
  target_link_libraries(OBS::libobs INTERFACE libobs::libobs)
endif()
if(UNIX AND NOT APPLE AND NOT TARGET OBS::libobs)
  pkg_check_modules(MPT_LIBOBS QUIET IMPORTED_TARGET libobs)
  if(TARGET PkgConfig::MPT_LIBOBS)
    add_library(OBS::libobs INTERFACE IMPORTED)
    target_link_libraries(OBS::libobs INTERFACE PkgConfig::MPT_LIBOBS)
  endif()
endif()

if(WIN32)
  add_library(${PROJECT_NAME} MODULE)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" MPT_PROJECT_ROOT_C)

  set(MPT_WINDOWS_FALLBACK_OBS OFF)
  set(MPT_LOCAL_OBS_FALLBACK OFF)

  if(TARGET OBS::libobs)
    target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
  else()
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
  endif()
else()
  if(NOT TARGET OBS::libobs)
    option(MPT_BUILD_PLUGIN "Build the OBS plugin target" OFF)
    if(MPT_BUILD_PLUGIN)
      message(FATAL_ERROR "libobs was not found. Install OBS Studio development files on Ubuntu or configure with -DMPT_BUILD_PLUGIN=OFF to build backend tests only.")
    endif()
  else()
    option(MPT_BUILD_PLUGIN "Build the OBS plugin target" ON)
  endif()

  if(MPT_BUILD_PLUGIN)
    add_library(${PROJECT_NAME} MODULE)
    target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)
  endif()
endif()

if(TARGET ${PROJECT_NAME})
  target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-support.c.in ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c @ONLY)

  target_sources(
    ${PROJECT_NAME}
    PRIVATE
      ${MPT_PLUGIN_SOURCES}
      ${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c
  )

  if(MPT_LOCAL_OBS_FALLBACK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MPT_FALLBACK_OBS=1)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-obs-util.c)
  endif()

  target_link_libraries(${PROJECT_NAME} PRIVATE ${MPT_PLUGIN_PLATFORM_LIBS})
  if(NOT WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
  endif()
  set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

  install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION} RUNTIME DESTINATION ${MPT_PLUGIN_RUNTIME_DESTINATION})
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/ DESTINATION ${MPT_PLUGIN_DATA_DESTINATION})
endif()

if(BUILD_TESTING)
  if(WIN32)
    add_executable(mpt-video-backend-reader-test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mpt-video-backend-reader-test.cpp
      ${MPT_VIDEO_BACKEND_SOURCES}
    )
    mpt_configure_video_backend_target(mpt-video-backend-reader-test)

    add_test(
      NAME mpt-video-backend-reader
      COMMAND $<TARGET_FILE:mpt-video-backend-reader-test>
    )
  endif()

  find_program(MPT_FFMPEG_EXE ffmpeg)
  if(MPT_FFMPEG_EXE)
    add_executable(mpt-video-backend-loop-test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mpt-video-backend-loop-test.cpp
      ${MPT_VIDEO_BACKEND_SOURCES}
    )
    mpt_configure_video_backend_target(mpt-video-backend-loop-test)

    add_test(
      NAME mpt-video-backend-loop
      COMMAND $<TARGET_FILE:mpt-video-backend-loop-test>
        --ffmpeg ${MPT_FFMPEG_EXE}
        --work-dir ${CMAKE_CURRENT_BINARY_DIR}/test-assets/mpt-video-backend-loop
        --frames 20000
        --max-same-hash-run 4
    )
  else()
    message(STATUS "ffmpeg not found; skipping mpt-video-backend-loop test target")
  endif()

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME obs-websocket-smoke-unit
      COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p obs_websocket_smoke_test.py
    )
  else()
    message(STATUS "Python3 not found; skipping obs-websocket-smoke-unit test")
  endif()

  if(UNIX AND NOT APPLE AND TARGET ${PROJECT_NAME} AND Python3_Interpreter_FOUND AND MPT_FFMPEG_EXE)
    set(MPT_LINUX_VALIDATION_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-assets/mpt-linux-validation")

    add_test(
      NAME mpt-linux-validation-assets
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generate-linux-validation-assets.py
        --output-dir ${MPT_LINUX_VALIDATION_ASSET_DIR}
        --ffmpeg ${MPT_FFMPEG_EXE}
    )
    set_tests_properties(mpt-linux-validation-assets PROPERTIES FIXTURES_SETUP mpt-linux-validation-assets)

    add_executable(mpt-linux-runtime-test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mpt-linux-runtime-test.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-audio-backend.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-image-backend.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mpt-video-backend.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/motionpngtuber-native.cpp
    )
    target_include_directories(mpt-linux-runtime-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(mpt-linux-runtime-test PRIVATE cxx_std_17)
    target_link_libraries(mpt-linux-runtime-test PRIVATE OBS::libobs ${MPT_PLUGIN_PLATFORM_LIBS} ZLIB::ZLIB)

    add_test(
      NAME mpt-linux-runtime
      COMMAND $<TARGET_FILE:mpt-linux-runtime-test>
        --asset-dir ${MPT_LINUX_VALIDATION_ASSET_DIR}
    )
    set_tests_properties(mpt-linux-runtime PROPERTIES FIXTURES_REQUIRED mpt-linux-validation-assets)

    add_executable(mpt-obs-plugin-load-test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mpt-obs-plugin-load-test.cpp
    )
    target_compile_features(mpt-obs-plugin-load-test PRIVATE cxx_std_17)
    target_link_libraries(mpt-obs-plugin-load-test PRIVATE OBS::libobs)

    add_test(
      NAME mpt-obs-plugin-load
      COMMAND $<TARGET_FILE:mpt-obs-plugin-load-test>
        --plugin-path $<TARGET_FILE:${PROJECT_NAME}>
        --plugin-data-dir ${CMAKE_CURRENT_SOURCE_DIR}/data
        --asset-dir ${MPT_LINUX_VALIDATION_ASSET_DIR}
    )
    set_tests_properties(mpt-obs-plugin-load PROPERTIES FIXTURES_REQUIRED mpt-linux-validation-assets)
  endif()
endif()
