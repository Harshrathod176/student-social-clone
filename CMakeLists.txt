cmake_minimum_required(VERSION 3.20)

project(StudentProfiles LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Crow CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(CURL REQUIRED)

add_executable(student_profiles src/main.cpp)

target_link_libraries(student_profiles PRIVATE
    Crow::Crow
    unofficial::sqlite3::sqlite3
    unofficial-sodium::sodium
    CURL::libcurl
)
