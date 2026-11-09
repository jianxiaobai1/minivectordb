cmake_minimum_required(VERSION 3.22)
project(MiniVectorDB CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3 -fsanitize=address,undefined")

# The runtime image installs nothing, so the two compiler runtime libraries
# are linked in rather than shipped as apt packages. Only glibc stays dynamic,
# which keeps getrandom()/clock_gettime on the platform's own libc.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -static-libstdc++ -static-libgcc")
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

file(GLOB_RECURSE MVD_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(minivectordb ${MVD_SOURCES})
target_include_directories(minivectordb PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(minivectordb PRIVATE Threads::Threads)

install(TARGETS minivectordb RUNTIME DESTINATION bin)
