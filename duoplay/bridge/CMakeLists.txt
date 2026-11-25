cmake_minimum_required(VERSION 3.20)
project(duoplay_bridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(cmake/Deps.cmake)
include(cmake/Chiaki.cmake)

# --- libdatachannel (WebRTC) --------------------------------------------------
duo_fetch(libdatachannel https://github.com/paullouisageneau/libdatachannel.git ${DUOPLAY_LIBDATACHANNEL_REF}
	SUBMODULES deps/plog deps/usrsctp deps/libjuice deps/libsrtp deps/json)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
add_subdirectory("${libdatachannel_SOURCE_DIR}" libdatachannel EXCLUDE_FROM_ALL)

# --- cpp-httplib (HTTP/HTTPS server + PSN HTTPS client) -----------------------
duo_fetch(httplib https://github.com/yhirose/cpp-httplib.git ${DUOPLAY_HTTPLIB_REF})

add_executable(duoplay-bridge
	src/main.cpp
	src/util.cpp
	src/store.cpp
	src/console.cpp
	src/streamer.cpp
	src/media.cpp
	src/peer.cpp
	src/wifilink.cpp
	src/fmp4.cpp
	src/api.cpp
	src/tls.cpp
	src/psn.cpp
	src/fakesource.cpp)
target_include_directories(duoplay-bridge PRIVATE
	"${httplib_SOURCE_DIR}"
	"${libdatachannel_SOURCE_DIR}/deps/json/single_include")
target_compile_definitions(duoplay-bridge PRIVATE
	CPPHTTPLIB_OPENSSL_SUPPORT
	DUOPLAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(duoplay-bridge PRIVATE duo_chiaki LibDataChannel::LibDataChannelStatic OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
	target_link_libraries(duoplay-bridge PRIVATE ws2_32 crypt32 iphlpapi)
	target_compile_definitions(duoplay-bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(duoplay-bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
