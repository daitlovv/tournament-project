cmake_minimum_required(VERSION 3.10)
project(RockPaperScissorsTournament)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O2 -D_DEFAULT_SOURCE")

find_library(PTHREAD_LIBRARY pthread)
find_library(RT_LIBRARY rt)

set(LIBS ${PTHREAD_LIBRARY} ${RT_LIBRARY})

add_executable(tournament tournament.c)
add_executable(tournament_control tournament_control.c)
add_executable(fighter fighter.c)
add_executable(single_observer single_observer.c)
add_executable(multi_observer multi_observer.c)

foreach(target
  tournament
  tournament_control
  fighter
  single_observer
  multi_observer
)
  target_link_libraries(${target} ${LIBS})
endforeach()

message(STATUS "Собраны программы:")
message(STATUS "  tournament         - Программа 1 (4-6 баллов) - неименованные семафоры")
message(STATUS "  tournament_control - Программа 2 (7-8 баллов) - именованные семафоры")
message(STATUS "  fighter            - Программа 2 (7-8 баллов) - именованные семафоры")
message(STATUS "  single_observer    - Программа 3 (9 баллов) - один наблюдатель")
message(STATUS "  multi_observer     - Программа 4 (10 баллов) - несколько наблюдателей")