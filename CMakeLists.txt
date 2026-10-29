cmake_minimum_required(VERSION 3.21)

project(qcnote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 QuickDialogs2 Test)

qt_standard_project_setup()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
