cmake_minimum_required(VERSION 3.28)
project(kkuk C)

set(CMAKE_C_STANDARD 11)

add_executable(kkuk main.c
        stella/gc.c
        stella/runtime.c
        example.c)
