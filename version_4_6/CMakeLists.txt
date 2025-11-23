cmake_minimum_required(VERSION 3.10)
project(Tournament_4_6)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O2 -D_DEFAULT_SOURCE")

find_library(PTHREAD_LIBRARY pthread)
find_library(RT_LIBRARY rt)

add_executable(tournament tournament.c)
target_link_libraries(tournament ${PTHREAD_LIBRARY} ${RT_LIBRARY})