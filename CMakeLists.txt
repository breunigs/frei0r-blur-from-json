cmake_minimum_required(VERSION 3.15)
project(jsonblur)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set (SOURCES jsonblur.cpp)
set (TARGET jsonblur)

if (MSVC)
  set (SOURCES ${SOURCES} ${FREI0R_DEF})
endif (MSVC)

add_library (${TARGET}  MODULE ${SOURCES})

find_package(Boost COMPONENTS iostreams)
target_include_directories(${TARGET} PRIVATE ${Boost_INCLUDE_DIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(vips REQUIRED IMPORTED_TARGET vips-cpp)

target_link_libraries(${TARGET} PRIVATE ${Boost_LIBRARIES} PkgConfig::vips)
set_target_properties (${TARGET} PROPERTIES PREFIX "")
