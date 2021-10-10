cmake_minimum_required(VERSION 3.15)
project(jsonblur)

set(CMAKE_CXX_STANDARD 17)
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
target_link_libraries(${TARGET} ${Boost_LIBRARIES})

add_definitions( -DMAGICKCORE_QUANTUM_DEPTH=16 )
add_definitions( -DMAGICKCORE_HDRI_ENABLE=0 )
find_package(ImageMagick COMPONENTS Magick++)
include_directories(${ImageMagick_INCLUDE_DIRS})
target_link_libraries(${TARGET} ${ImageMagick_LIBRARIES})

set_target_properties (${TARGET} PROPERTIES PREFIX "")
