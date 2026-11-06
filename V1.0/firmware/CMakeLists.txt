set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRC)
aux_source_directory(${APP_PATH}/src/images IMAGE_SRC)
aux_source_directory(${APP_PATH}/src/fonts FONT_SRC)
list(APPEND APP_SRC ${IMAGE_SRC} ${FONT_SRC})

set(APP_INC
    ${APP_PATH}/src
    ${APP_PATH}/src/images
    ${APP_PATH}/src/fonts
    ${CMAKE_SOURCE_DIR}/src/tal_bluetooth/nimble/host
)

add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB} PRIVATE ${APP_SRC})
target_include_directories(${EXAMPLE_LIB} PRIVATE ${APP_INC})
target_compile_options(${EXAMPLE_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
