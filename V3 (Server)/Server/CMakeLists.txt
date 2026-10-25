
cmake_minimum_required(VERSION 3.16)

project(TimeWiseGateway
    VERSION 1.0.0
    LANGUAGES CXX
)

# ---------------------------------------------------------
# C++ Configuration
# ---------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------
# Dependencies
# ---------------------------------------------------------

find_package(Threads REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------
# Source Files
# ---------------------------------------------------------

add_executable(TimeWiseGateway
    src/main.cpp
    src/WeatherStation.cpp
    src/HttpServer.cpp
    src/MusicPlayer.cpp
    src/NavidromeClient.cpp
)

# ---------------------------------------------------------
# Header Files
# ---------------------------------------------------------

target_include_directories(TimeWiseGateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------
# Libraries
# ---------------------------------------------------------

target_link_libraries(TimeWiseGateway
    PRIVATE
        Threads::Threads
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
)

# ---------------------------------------------------------
# Boost Configuration
# ---------------------------------------------------------

if(TARGET Boost::headers)
    target_link_libraries(TimeWiseGateway
        PRIVATE Boost::headers
    )
elseif(TARGET Boost::boost)
    target_link_libraries(TimeWiseGateway
        PRIVATE Boost::boost
    )
else()
    target_include_directories(TimeWiseGateway
        PRIVATE ${Boost_INCLUDE_DIRS}
    )
endif()

# ---------------------------------------------------------
# Windows Configuration
# ---------------------------------------------------------

if(WIN32)
    target_link_libraries(TimeWiseGateway
        PRIVATE
            ws2_32
            mswsock
    )

    # GCC/MinGW configuration
    if(MINGW)
        target_compile_definitions(TimeWiseGateway
            PRIVATE
                _WIN32_WINNT=0x0A00
        )
    endif()
endif()

# ---------------------------------------------------------
# Compiler Warnings
# ---------------------------------------------------------

if(MSVC)
    target_compile_options(TimeWiseGateway
        PRIVATE /W4
    )
else()
    target_compile_options(TimeWiseGateway
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

# ---------------------------------------------------------
# Output Directory
# ---------------------------------------------------------

set_target_properties(TimeWiseGateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/output"
)

# ---------------------------------------------------------
# Build Information
# ---------------------------------------------------------

message(STATUS "--------------------------------------")
message(STATUS "TimeWise Gateway Configuration")
message(STATUS "--------------------------------------")
message(STATUS "Operating System : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler         : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ Standard     : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "Boost Version    : ${Boost_VERSION}")
message(STATUS "CURL Version     : ${CURL_VERSION_STRING}")
message(STATUS "--------------------------------------")
