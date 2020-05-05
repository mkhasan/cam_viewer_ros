cmake_minimum_required(VERSION 2.8.3)
project(cam_viewer)


add_compile_options(-std=c++14)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ggdb -Wall -D_GNU_SOURCE=1 -D_REENTRANT")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -Wall -D_GNU_SOURCE=1 -D_REENTRANT")

find_package(catkin REQUIRED COMPONENTS
  roscpp
  rospy
  std_msgs
  client_interface
  
)


include_directories(
  include
  /usr/local/include
  ${catkin_INCLUDE_DIRS}
)


 

find_library(JPEG_LIB jpeg)
#find_library(ACE ACE)


find_library(AVUTIL avutil HINTS $ENV{FFMPEG_ROOT}/libavutil)
find_library(SWRESAMPLE swresample HINTS $ENV{FFMPEG_ROOT}/libswresample)
find_library(AVCODEC avcodec HINTS $ENV{FFMPEG_ROOT}/libavcodec)
find_library(AVFORMAT avformat HINTS $ENV{FFMPEG_ROOT}/libavformat)
find_library(SWSCALE swscale HINTS $ENV{FFMPEG_ROOT}/libswscale)
find_library(AVFILTER avfilter HINTS $ENV{FFMPEG_ROOT}/libavfilter)
find_library(AVDEVICE avdevice HINTS $ENV{FFMPEG_ROOT}/libavdevice)

find_library(YUV_LIB yuv HINTS "/usr/local")
message ("yuv foudn ${YUV_LIB}")


add_library(${PROJECT_NAME} src/${PROJECT_NAME}.c src/convert.cpp )

catkin_package(
   INCLUDE_DIRS include 
   LIBRARIES ${PROJECT_NAME} 
   CATKIN_DEPENDS roscpp rospy std_msgs client_interface
)

add_dependencies(${PROJECT_NAME} ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})

target_link_libraries(${PROJECT_NAME} ${catkin_LIBRARIES} ${AVUTIL} ${SWRESAMPLE} ${AVCODEC} ${AVFORMAT} ${SWSCALE} ${AVFILTER} ${AVDEVICE} ${YUV_LIB} ${JPEG_LIB})

add_executable(${PROJECT_NAME}_node src/${PROJECT_NAME}_node.cpp )
 
add_dependencies(${PROJECT_NAME}_node ${${PROJECT_NAME}_node_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})

target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME} ${catkin_LIBRARIES})


