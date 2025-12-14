cmake_minimum_required(VERSION 3.10)
project(FabrykaCzekolady)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(magazyn
    src/magazyn.cpp
)

add_executable(dyrektor
    src/dyrektor.cpp
)


add_executable(dostawca
    src/dostawca.cpp
)


add_executable(stanowisko
    src/stanowisko.cpp
)