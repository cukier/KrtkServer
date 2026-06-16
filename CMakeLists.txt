cmake_minimum_required(VERSION 3.16)

project(KrtkServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

qt_standard_project_setup()

qt_add_executable(KrtkServer
  main.cpp
  httpserver.h
  httpserver.cpp
)

target_link_libraries(KrtkServer PRIVATE Qt6::Core Qt6::Network)

install(TARGETS KrtkServer
    LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
)
