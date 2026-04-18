cmake_minimum_required(VERSION 3.20)

project(TrayKeyboard VERSION 0.1.0 LANGUAGES CXX)

add_executable(TrayKeyboard WIN32
    src/main.cpp
)

target_compile_features(TrayKeyboard PRIVATE cxx_std_20)

target_compile_definitions(TrayKeyboard PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(TrayKeyboard PRIVATE shell32 user32)
