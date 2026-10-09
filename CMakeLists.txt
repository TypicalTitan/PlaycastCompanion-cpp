cmake_minimum_required(VERSION 3.21)
project(PlaycastCompanion VERSION 3.2.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR "Playcast Companion targets MSVC (Windows only).")
endif()

file(GLOB PC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(PlaycastCompanion WIN32 ${PC_SOURCES} ${CMAKE_SOURCE_DIR}/src/app.rc)
target_sources(PlaycastCompanion PRIVATE ${CMAKE_SOURCE_DIR}/src/LogAnalysisSample.rc)

target_include_directories(PlaycastCompanion PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(PlaycastCompanion PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  PC_VERSION_STRING="${PROJECT_VERSION}"
  PC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  PC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  PC_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Hardened build: warnings are errors, SDL checks, stack cookies, CFG, CET,
# ASLR/DEP/high-entropy, UTF-8 sources, strict conformance.
target_compile_options(PlaycastCompanion PRIVATE
  /W4 /WX /permissive- /sdl /GS /guard:cf /utf-8 /EHsc /Zc:__cplusplus /bigobj
  /await:strict
  $<$<CONFIG:Release>:/O2 /Oi /Gy>)
target_link_options(PlaycastCompanion PRIVATE
  /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /CETCOMPAT /SUBSYSTEM:WINDOWS
  /MANIFEST:NO
  $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)

# Static CRT so the single exe has no VC++ runtime dependency.
set_property(TARGET PlaycastCompanion PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(PlaycastCompanion PRIVATE
  user32 gdi32 gdiplus shell32 advapi32 ole32 comctl32 uxtheme dwmapi
  wtsapi32 winhttp ws2_32 version windowsapp runtimeobject)

set_target_properties(PlaycastCompanion PROPERTIES OUTPUT_NAME "PlaycastCompanion")

include(CTest)
if(BUILD_TESTING)
  add_executable(NativeSessionLoggingContracts
    tests/SessionLoggingContracts.cpp
    src/Config.cpp src/ConfigSerialization.cpp src/Json.cpp src/Log.cpp
    src/SessionLoggingInventory.cpp src/SessionLoggingVdf.cpp
    src/SessionLoggingSecurity.cpp src/SessionLoggingRedactor.cpp src/SessionLoggingTextRedactor.cpp
    src/SessionLogging.cpp src/SessionLoggingSnapshot.cpp src/SessionLoggingProcesses.cpp
    src/SessionLoggingWriter.cpp src/SessionLoggingPipe.cpp src/SessionLoggingClassifier.cpp)
  target_include_directories(NativeSessionLoggingContracts PRIVATE src)
  target_compile_definitions(NativeSessionLoggingContracts PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    PC_VERSION_STRING="${PROJECT_VERSION}" PC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PC_VERSION_MINOR=${PROJECT_VERSION_MINOR} PC_VERSION_PATCH=${PROJECT_VERSION_PATCH})
  target_compile_options(NativeSessionLoggingContracts PRIVATE
    /W4 /WX /permissive- /sdl /GS /guard:cf /utf-8 /EHsc /Zc:__cplusplus /bigobj /await:strict)
  set_property(TARGET NativeSessionLoggingContracts PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(NativeSessionLoggingContracts PRIVATE
    user32 gdi32 gdiplus shell32 advapi32 ole32 comctl32 uxtheme dwmapi
    wtsapi32 winhttp ws2_32 version windowsapp runtimeobject)
  add_test(NAME NativeSessionLoggingContracts COMMAND NativeSessionLoggingContracts)
  add_executable(NativeLogAnalysisContracts tests/LogAnalysisContracts.cpp
    src/Json.cpp src/LogAnalysisJson.cpp src/LogAnalysisImporter.cpp src/LogAnalysisProjection.cpp
    src/LogAnalysisSample.cpp src/LogAnalysisSample.rc)
  target_include_directories(NativeLogAnalysisContracts PRIVATE src)
  target_compile_definitions(NativeLogAnalysisContracts PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(NativeLogAnalysisContracts PRIVATE
    /W4 /WX /permissive- /sdl /GS /guard:cf /utf-8 /EHsc /Zc:__cplusplus /bigobj /await:strict)
  set_property(TARGET NativeLogAnalysisContracts PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(NativeLogAnalysisContracts PRIVATE windowsapp runtimeobject ole32)
  add_test(NAME NativeLogAnalysisContracts COMMAND NativeLogAnalysisContracts)
  add_test(NAME NativeUiScrollContracts COMMAND PlaycastCompanion --test-ui-scroll)
endif()
