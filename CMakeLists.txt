cmake_minimum_required(VERSION 3.25)
project(uon-soem VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(main example/main.cpp)

# add_subdirectory(SOEM)
# target_include_directories(main PRIVATE 
#     ${CMAKE_CURRENT_SOURCE_DIR}/SOEM/src
#     ${CMAKE_CURRENT_SOURCE_DIR}/SOEM/osal
#     ${CMAKE_CURRENT_SOURCE_DIR}/SOEM/osal/linux
#     ${CMAKE_CURRENT_SOURCE_DIR}/SOEM/oshw/linux
# )
# target_link_libraries(main PRIVATE soem pthread)

target_include_directories(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/SOEM/include")
target_link_libraries(main PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/SOEM/lib/libsoem.a"
    pthread
    # rt
)
