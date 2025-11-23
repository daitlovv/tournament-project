cmake_minimum_required(VERSION 3.10)
project(Tournament_7_8)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O2 -D_DEFAULT_SOURCE")

find_library(PTHREAD_LIBRARY pthread)
find_library(RT_LIBRARY rt)

add_executable(tournament tournament.c)
add_executable(fighter fighter.c)

target_link_libraries(tournament ${PTHREAD_LIBRARY} ${RT_LIBRARY})
target_link_libraries(fighter ${PTHREAD_LIBRARY} ${RT_LIBRARY})