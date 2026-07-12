include(CheckCSourceCompiles)
include(CheckCCompilerFlag)
include(TestBigEndian)
include(CheckSymbolExists)
include(CheckPrototypeDefinition)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")

# Use ZLIB_ROOT when searching for zlib with find_package
# This is used when making Win32 binary releases.
cmake_policy(SET CMP0074 NEW)

# Check if LD supports linker scripts.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/conftest.ldscript" "VERS_1 {
  global: sym;
};

VERS_2 {
  global: sym;
} VERS_1;
")
set(CMAKE_REQUIRED_FLAGS_SAVE ${CMAKE_REQUIRED_FLAGS})
set(CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS} "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/conftest.ldscript")
check_c_source_compiles("int main(void){return 0;}" HAVE_LD_VERSION_SCRIPT)
set(CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS_SAVE})
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/conftest.ldscript")

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# strdup, strtok_r
add_definitions(-D_XOPEN_SOURCE=600)

# strnlen
add_definitions(-D_POSIX_C_SOURCE=200809L)

# strsep
# Also silence warnings about obsolete _BSD_SOURCE macro in ASN.1 code
add_definitions(-D_DEFAULT_SOURCE)

# strsep on MacOS
if(APPLE)
	add_definitions(-D_DARWIN_C_SOURCE)
endif()

if (HAVE_LD_VERSION_SCRIPT)
	set(acars_ldscript "${CMAKE_CURRENT_SOURCE_DIR}/symversions.ldscript")
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--version-script,${acars_ldscript}")
endif()

CHECK_C_COMPILER_FLAG(-fno-common CC_HAS_FNO_COMMON)
if(CC_HAS_FNO_COMMON)
        add_definitions(-fno-common)
endif()

TEST_BIG_ENDIAN(IS_BIG_ENDIAN)

set(CMAKE_REQUIRED_FLAGS_SAVE ${CMAKE_REQUIRED_FLAGS})
CHECK_SYMBOL_EXISTS(strsep string.h HAVE_STRSEP)
set(CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS_SAVE})

set(CMAKE_REQUIRED_DEFINITIONS_SAVE ${CMAKE_REQUIRED_DEFINITIONS})
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} "-D_GNU_SOURCE")
CHECK_SYMBOL_EXISTS(memmem string.h HAVE_MEMMEM)
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS_SAVE})

# lfind - Unix prototype
check_prototype_definition(
	lfind
	"void *lfind(const void *key, const void *base, size_t *nmemb,
	size_t size, int(*compar)(const void *, const void *))"
	"NULL"
	"stddef.h;search.h"
	LFIND_NMEMB_SIZE_SIZE_T)

# lfind - Windows prototype
check_prototype_definition(
	lfind
	"void *lfind(const void *key, const void *base, unsigned int *nmemb,
	unsigned int size, int(*compar)(const void *, const void *))"
	"NULL"
	"stddef.h;search.h"
	LFIND_NMEMB_SIZE_UINT)

if(NOT ${LFIND_NMEMB_SIZE_SIZE_T} AND NOT ${LFIND_NMEMB_SIZE_UINT})
	message(FATAL_ERROR "Unable to determine lfind function prototype")
endif()

CHECK_INCLUDE_FILE("sys/time.h" HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILE("unistd.h" HAVE_UNISTD_H)

if(NOT MSVC)
	find_library(LIBM m REQUIRED)
	list(APPEND acars_extra_libs m)
endif()

option(ZLIB "Enable ZLIB support for MIAM" ON)
set(WITH_ZLIB FALSE)

if(ZLIB)
	if(TARGET zlibstatic)
		set(WITH_ZLIB TRUE)
		list(APPEND acars_extra_libs zlibstatic)
		list(APPEND acars_include_dirs ${ZLIB_VENDORED_INCLUDE_DIR})
	else()
		find_package(ZLIB)
		if(ZLIB_FOUND)
			set(WITH_ZLIB TRUE)
			list(APPEND acars_extra_libs ${ZLIB_LIBRARIES})
			list(APPEND acars_include_dirs ${ZLIB_INCLUDE_DIRS})
		endif()
	endif()
endif()

option(LIBXML2 "Enable libxml2 support for pretty-printing XML messages" ON)
set(WITH_LIBXML2 FALSE)

if(LIBXML2)
	find_package(LibXml2)
	if(LibXml2_FOUND)
		set(WITH_LIBXML2 TRUE)
		list(APPEND acars_extra_libs ${LIBXML2_LIBRARIES})
		list(APPEND acars_include_dirs ${LIBXML2_INCLUDE_DIR})
	endif()
endif()

option(JANSSON "Enable jansson library support for parsing JSON-encoded messages (eg. OHMA)" ON)
set(WITH_JANSSON FALSE)

if(JANSSON)
	if(TARGET jansson)
		set(WITH_JANSSON TRUE)
		list(APPEND acars_extra_libs jansson)
		list(APPEND acars_include_dirs ${JANSSON_VENDORED_INCLUDE_DIR})
	else()
		find_package(Jansson)
		if(JANSSON_FOUND)
			set(WITH_JANSSON TRUE)
			list(APPEND acars_extra_libs ${JANSSON_LIBRARIES})
			list(APPEND acars_include_dirs ${JANSSON_INCLUDE_DIR})
		endif()
	endif()
endif()

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.c
		${CMAKE_CURRENT_BINARY_DIR}/_version.c
	COMMAND ${CMAKE_COMMAND} -DLA_VERSION=${LA_VERSION} -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
)

message(STATUS "libacars configuration summary:")
message(STATUS "- zlib:\t\trequested: ${ZLIB}, enabled: ${WITH_ZLIB}")
message(STATUS "- libxml2:\t\trequested: ${LIBXML2}, enabled: ${WITH_LIBXML2}")
message(STATUS "- jansson:\t\trequested: ${JANSSON}, enabled: ${WITH_JANSSON}")

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/config.h"
	@ONLY
)

add_subdirectory (asn1)
add_library (acars_core OBJECT
	acars.c
	adsc.c
	arinc.c
	asn1-format-common.c
	asn1-format-cpdlc-text.c
	asn1-format-cpdlc-json.c
	asn1-util.c
	bitstream.c
	configuration.c
	cpdlc.c
	crc.c
	dict.c
	hash.c
	json.c
	libacars.c
	list.c
	media-adv.c
	miam.c
	miam-core.c
	ohma.c
	reassembly.c
	util.c
	vstring.c
	${CMAKE_CURRENT_BINARY_DIR}/version.c
)
set_property(TARGET acars_core PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(acars_core PUBLIC ${acars_include_dirs} ${CMAKE_CURRENT_BINARY_DIR})

set(acars_obj_files $<TARGET_OBJECTS:asn1> $<TARGET_OBJECTS:acars_core>)

add_library(acars_static STATIC ${acars_obj_files})
target_link_libraries(acars_static ${acars_extra_libs})
target_include_directories(acars_static PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_BINARY_DIR}
)
