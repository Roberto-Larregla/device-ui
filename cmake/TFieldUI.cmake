# T-Field UI (https://github.com/Roberto-Larregla/t-field-ui): the interface TFieldView shows.
# -DTFIELD_UI_DIR=<path> builds a local checkout instead of the pinned version.
if(TFIELD_UI_DIR)
    set(tfieldui_SOURCE_DIR ${TFIELD_UI_DIR})
else()
    message(STATUS "Fetching T-Field UI ...")
    FetchContent_Declare(
        TFieldUI
        GIT_REPOSITORY "https://github.com/Roberto-Larregla/t-field-ui"
        GIT_TAG  b9a5c7d7e88a31b30ee11436d41aaf1974ddd4e0 # same as library.json
        SOURCE_SUBDIR none # only the sources; its own CMake project is the simulator
    )
    FetchContent_MakeAvailable(TFieldUI)
endif()
file(GLOB TFIELD_SOURCES ${tfieldui_SOURCE_DIR}/src/*.cpp ${tfieldui_SOURCE_DIR}/fonts/*.c)
include_directories(${tfieldui_SOURCE_DIR}/include)
