# Pinned, shallow dependency fetching.
#
# Every dependency is pinned to an exact commit or tag and fetched with a shallow
# `git fetch` (much faster than FetchContent's full clones of large repos such as
# chiaki-ng). Sources land in DUOPLAY_DEPS_DIR (default: bridge/deps) and are reused
# across builds. Delete that folder to force a re-fetch.

find_package(Git REQUIRED)

set(DUOPLAY_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Where pinned dependency sources are kept")

function(_duo_git dir)
	execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
		WORKING_DIRECTORY "${dir}"
		RESULT_VARIABLE res
		OUTPUT_QUIET ERROR_VARIABLE err)
	if(NOT res EQUAL 0)
		message(FATAL_ERROR "git ${ARGN} failed in ${dir}:\n${err}")
	endif()
endfunction()

# duo_fetch(<name> <repo-url> <ref> [SUBMODULES path...])
# Sets <name>_SOURCE_DIR in the caller's scope.
function(duo_fetch name repo ref)
	cmake_parse_arguments(ARG "" "" "SUBMODULES" ${ARGN})
	set(dir "${DUOPLAY_DEPS_DIR}/${name}")
	set(stamp "${dir}/.duoplay-ref")
	set(have "")
	if(EXISTS "${stamp}")
		file(READ "${stamp}" have)
		string(STRIP "${have}" have)
	endif()
	if(NOT have STREQUAL ref)
		message(STATUS "Fetching ${name} @ ${ref}")
		file(REMOVE_RECURSE "${dir}")
		file(MAKE_DIRECTORY "${dir}")
		_duo_git("${dir}" init -q)
		_duo_git("${dir}" remote add origin "${repo}")
		if(ref MATCHES "^[0-9a-f]+$")
			_duo_git("${dir}" fetch -q --depth 1 origin "${ref}")
		else()
			_duo_git("${dir}" fetch -q --depth 1 origin "refs/tags/${ref}")
		endif()
		_duo_git("${dir}" -c advice.detachedHead=false checkout -q FETCH_HEAD)
		if(ARG_SUBMODULES)
			_duo_git("${dir}" submodule update -q --init --depth 1 --recursive ${ARG_SUBMODULES})
		endif()
		file(WRITE "${stamp}" "${ref}\n")
	endif()
	set(${name}_SOURCE_DIR "${dir}" PARENT_SCOPE)
endfunction()

# --- Pins -------------------------------------------------------------------
# chiaki-ng: the PS4/PS5 Remote Play protocol implementation (AGPL-3.0 w/ OpenSSL exception)
set(DUOPLAY_CHIAKI_REF      "43728af2e639fecb8a7af1cd68c29820a40de973")
# Its submodules, at the exact commits chiaki-ng pins
set(DUOPLAY_JERASURE_REF    "505ccb4bc69eee8d7ea40a6089a056b99671134f")
set(DUOPLAY_GFCOMPLETE_REF  "fa54a4670a5705c84abf6c24b92b0cd479625478")
set(DUOPLAY_NANOPB_REF      "160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f")
# WebRTC (MPL-2.0) and HTTP server (MIT)
set(DUOPLAY_LIBDATACHANNEL_REF "v0.24.8")
set(DUOPLAY_HTTPLIB_REF        "v0.67.0")
