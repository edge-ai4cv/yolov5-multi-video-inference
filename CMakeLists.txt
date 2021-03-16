cmake_minimum_required(VERSION 2.6)

project(yolov5-multi-video)

add_definitions(-std=c++11)

option(CUDA_USE_STATIC_CUDA_RUNTIME OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

find_package(CUDA REQUIRED)

include_directories(${PROJECT_SOURCE_DIR}/include)

# include and link dirs of cuda and tensorrt, you need adapt them if yours are different
# cuda
include_directories(/usr/local/cuda/include)
link_directories(/usr/local/cuda/lib64)

if (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "aarch64")
    # tensorrt
    include_directories(/usr/include/aarch64-linux-gnu/)
    link_directories(/usr/lib/aarch64-linux-gnu/)
elseif (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "x86_64")
    # tensorrt
    include_directories(/usr/include/x86_64-linux-gnu/)
    link_directories(/usr/lib/x86_64-linux-gnu/)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Ofast -Wfatal-errors -D_MWAITXINTRIN_H_INCLUDED -pthread")

cuda_add_library(myplugins SHARED ${PROJECT_SOURCE_DIR}/yololayer.cu)
target_link_libraries(myplugins nvinfer cudart)

find_package(OpenCV)
include_directories(${OpenCV_INCLUDE_DIRS})

add_executable(yolov5-multi-video ${PROJECT_SOURCE_DIR}/calibrator.cpp ${PROJECT_SOURCE_DIR}/yolov5.cpp)
target_link_libraries(yolov5-multi-video nvinfer)
target_link_libraries(yolov5-multi-video cudart)
target_link_libraries(yolov5-multi-video myplugins)
target_link_libraries(yolov5-multi-video ${OpenCV_LIBS})

add_definitions(-O2 -pthread)

