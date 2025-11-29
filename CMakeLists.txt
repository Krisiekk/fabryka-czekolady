cmake_minimum_required(VERSION 3.10)
project(FabrykaCzekolady)

set(CMAKE_CXX_STANDARD 17)

include_directories(include)

# Każdy proces osobny plik wykonywalny
add_executable(main        src/main.cpp)
add_executable(dyrektor    src/dyrektor.cpp)
add_executable(dostawca    src/dostawca.cpp)
add_executable(pracownik   src/pracownik.cpp)
add_executable(raport      src/raport.cpp)