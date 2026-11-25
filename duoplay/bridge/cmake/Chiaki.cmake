# Builds the protocol core of chiaki-ng as a static library, headless.
#
# Only what a LAN bridge needs is compiled: discovery, wakeup, registration and the
# streaming session (ctrl / takion / FEC / congestion control / feedback). The
# internet "holepunch" code (which drags in curl, json-c, libevent and miniupnpc)
# is left out and replaced with stubs; the bridge only ever connects on the LAN.
# Opus decoding is not compiled either: audio is passed through to the browser
# still Opus-encoded.

duo_fetch(chiaki     https://github.com/streetpea/chiaki-ng.git   ${DUOPLAY_CHIAKI_REF})
duo_fetch(jerasure   https://github.com/streetpea/jerasure.git    ${DUOPLAY_JERASURE_REF})
duo_fetch(gfcomplete https://github.com/streetpea/gf-complete.git ${DUOPLAY_GFCOMPLETE_REF})
duo_fetch(nanopb     https://github.com/nanopb/nanopb.git         ${DUOPLAY_NANOPB_REF})

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PROTOC protoc REQUIRED)

# --- gf-complete + jerasure (Reed-Solomon FEC used by the video stream) -----
add_library(duo_gfcomplete STATIC
	${gfcomplete_SOURCE_DIR}/src/gf.c
	${gfcomplete_SOURCE_DIR}/src/gf_wgen.c
	${gfcomplete_SOURCE_DIR}/src/gf_w4.c
	${gfcomplete_SOURCE_DIR}/src/gf_w8.c
	${gfcomplete_SOURCE_DIR}/src/gf_w16.c
	${gfcomplete_SOURCE_DIR}/src/gf_w32.c
	${gfcomplete_SOURCE_DIR}/src/gf_w64.c
	${gfcomplete_SOURCE_DIR}/src/gf_w128.c
	${gfcomplete_SOURCE_DIR}/src/gf_rand.c
	${gfcomplete_SOURCE_DIR}/src/gf_general.c
	${gfcomplete_SOURCE_DIR}/src/gf_cpu.c)
target_include_directories(duo_gfcomplete PUBLIC ${gfcomplete_SOURCE_DIR}/include)
set_target_properties(duo_gfcomplete PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(duo_jerasure STATIC
	${jerasure_SOURCE_DIR}/src/galois.c
	${jerasure_SOURCE_DIR}/src/jerasure.c
	${jerasure_SOURCE_DIR}/src/reed_sol.c
	${jerasure_SOURCE_DIR}/src/cauchy.c
	${jerasure_SOURCE_DIR}/src/liberation.c)
target_include_directories(duo_jerasure PUBLIC ${jerasure_SOURCE_DIR}/include)
target_link_libraries(duo_jerasure PUBLIC duo_gfcomplete)

# --- nanopb + generated takion protobuf -------------------------------------
set(DUO_PB_DIR "${CMAKE_CURRENT_BINARY_DIR}/chiaki-pb")
file(MAKE_DIRECTORY "${DUO_PB_DIR}")
set(DUO_TAKION_PROTO "${chiaki_SOURCE_DIR}/lib/protobuf/takion.proto")
add_custom_command(
	OUTPUT "${DUO_PB_DIR}/takion.pb.c" "${DUO_PB_DIR}/takion.pb.h"
	COMMAND "${PROTOC}" "-o${DUO_PB_DIR}/takion.pb" "-I${chiaki_SOURCE_DIR}/lib/protobuf" "${DUO_TAKION_PROTO}"
	COMMAND "${Python3_EXECUTABLE}" "${nanopb_SOURCE_DIR}/generator/nanopb_generator.py" -q "takion.pb"
	WORKING_DIRECTORY "${DUO_PB_DIR}"
	DEPENDS "${DUO_TAKION_PROTO}"
	COMMENT "Generating nanopb code for takion.proto")

add_library(duo_nanopb STATIC
	${nanopb_SOURCE_DIR}/pb_common.c
	${nanopb_SOURCE_DIR}/pb_encode.c
	${nanopb_SOURCE_DIR}/pb_decode.c)
target_include_directories(duo_nanopb PUBLIC ${nanopb_SOURCE_DIR})
target_compile_definitions(duo_nanopb PUBLIC PB_C99_STATIC_ASSERT)

# --- chiaki core ------------------------------------------------------------
set(CHIAKI_LIB_ENABLE_OPUS 0)
set(CHIAKI_LIB_ENABLE_PI_DECODER 0)
set(CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0)
configure_file("${chiaki_SOURCE_DIR}/lib/config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/chiaki-config/chiaki/config.h")

set(_C "${chiaki_SOURCE_DIR}/lib/src")
add_library(duo_chiaki STATIC
	${_C}/common.c ${_C}/sock.c ${_C}/session.c ${_C}/thread.c ${_C}/base64.c ${_C}/http.c
	${_C}/log.c ${_C}/ctrl.c ${_C}/rpcrypt.c ${_C}/takion.c ${_C}/senkusha.c
	${_C}/streamconnection.c ${_C}/ecdh.c ${_C}/launchspec.c ${_C}/random.c ${_C}/gkcrypt.c
	${_C}/audio.c ${_C}/audioreceiver.c ${_C}/audiosender.c ${_C}/videoreceiver.c
	${_C}/frameprocessor.c ${_C}/packetstats.c ${_C}/discovery.c ${_C}/congestioncontrol.c
	${_C}/stoppipe.c ${_C}/reorderqueue.c ${_C}/discoveryservice.c ${_C}/feedback.c
	${_C}/feedbacksender.c ${_C}/controller.c ${_C}/takionsendbuffer.c ${_C}/time.c
	${_C}/fec.c ${_C}/regist.c ${_C}/orientation.c ${_C}/bitstream.c
	${_C}/remote/rudp.c ${_C}/remote/rudpsendbuffer.c
	"${CMAKE_CURRENT_SOURCE_DIR}/src/chiaki_lan_only.c"
	"${DUO_PB_DIR}/takion.pb.c")
# musl (used by the static Raspberry Pi build) has no pthread_clockjoin_np. chiaki-ng
# already carries a portable timed join for Android's libc, which lacks it too; use it.
if(DUOPLAY_MUSL)
	set_source_files_properties("${_C}/thread.c" PROPERTIES COMPILE_DEFINITIONS "__ANDROID__=1")
endif()

target_include_directories(duo_chiaki
	PUBLIC "${chiaki_SOURCE_DIR}/lib/include" "${CMAKE_CURRENT_BINARY_DIR}/chiaki-config"
	PRIVATE "${DUO_PB_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/shim")
target_compile_definitions(duo_chiaki PRIVATE
	CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=10 CHIAKI_VERSION_PATCH=1 CHIAKI_VERSION="1.10.1")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(duo_chiaki PRIVATE -w)
endif()
target_link_libraries(duo_chiaki PUBLIC duo_jerasure duo_nanopb OpenSSL::Crypto Threads::Threads)
if(WIN32)
	target_link_libraries(duo_chiaki PUBLIC ws2_32 wsock32 bcrypt iphlpapi)
endif()
if(UNIX AND NOT APPLE)
	target_link_libraries(duo_chiaki PUBLIC m)
endif()
