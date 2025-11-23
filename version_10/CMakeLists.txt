cmake_minimum_required(VERSION 3.10)
project(RockPaperScissorsTournament)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O2 -D_DEFAULT_SOURCE")

find_library(PTHREAD_LIBRARY pthread)
find_library(RT_LIBRARY rt)

add_executable(tournament tournament.c)
add_executable(tournament_control tournament_control.c)
add_executable(fighter fighter.c)
add_executable(single_observer single_observer.c)
add_executable(multi_observer version_10/multi_observer.c)

foreach(target
  tournament
  tournament_control
  fighter
  single_observer
  multi_observer
)
  target_link_libraries(${target} ${PTHREAD_LIBRARY} ${RT_LIBRARY})
endforeach()
