FILE(GLOB ALL_HEADERS "*.h")

set(SERVER_SPECIFIC_SRCS job.cc bgtask.cc cacheman.cc conn.cc conserver.cc expiration.cc maintenance.cc acregistry.cc mirror.cc pkgimport.cc showinfo.cc)

set(SHAREDSRCS astrop.cc sockio.cc acbuf.cc acfg.cc acfg_defaults.cc aclogger.cc caddrinfo.cc dirwalk.cc dlcon.cc fileio.cc
    fileitem.cc filereader.cc header.cc meta.cc tcpconnect.cc cleaner.cc lockable.cc evabase.cc ebrunner.cc httpdate.cc
    csmapping.cc acerrno.cc aconnect.cc ac3rdparty.cc remotedb.cc tpool.cc ahttpurl.cc
    ${SERVER_SPECIFIC_SRCS}
    ${ALL_HEADERS})

# XXX: SERVER_SPECIFIC_SRCS should be only part of the target below, not shared set. To be changed when symbol export is fixed.
set(ACNG_SRCS apt-cacher.cc )

if(SUP_STATIC)
        message("-- Configuring shared code as STATIC lib")
ADD_LIBRARY(supacng STATIC ${SHAREDSRCS})
else()
        message("-- Configuring shared code as DYNAMIC lib")
ADD_LIBRARY(supacng SHARED ${SHAREDSRCS})
add_definitions(-DACNG_CORE_IN_SO)
endif()

SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")

option(ENABLE_PCH "Enable use of precompiled headers for faster builds" off)
if(ENABLE_PCH)
	# this brings about 15% faster builds
	# OTOH it ruins ccache functionality with default setting - needs ccache config change, see ccache(1) about PRECOMPILED HEADERS
	if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16")
		target_precompile_headers(supacng PUBLIC meta.h debug.h filereader.h acfg.h)
		if(CMAKE_COMPILER_IS_CCACHE OR CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
			message(WARNING "Precompiled headers are enabled and ccache is in use. Make sure to configure it, see ccache(1) about PRECOMPILED HEADERS, or set ENABLE_PCH to off.")
		endif()
	endif()
endif()

TARGET_LINK_LIBRARIES(supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
INSTALL(TARGETS supacng LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

ADD_EXECUTABLE(apt-cacher-ng ${ACNG_SRCS} apt-cacher.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${CMAKE_INSTALL_SBINDIR})

ADD_EXECUTABLE(acngtool acngtool.cc)
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(acngtool supacng ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

option(ENABLE_EXPERIMENTAL "Enables incomplete of unsafe features" off)
if (ENABLE_EXPERIMENTAL)
    ADD_LIBRARY(supacng_experimental STATIC atcpstream.cc)
    ADD_EXECUTABLE(acngtoolX acngtool.cc)
    SET_TARGET_PROPERTIES(acngtoolX PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_PTHREAD} -DENABLE_EXPERIMENTAL")
    TARGET_LINK_LIBRARIES(acngtoolX supacng ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNGTOOL} supacng_experimental)
endif()


# build a special version for test with full symbol visibility
if(ENABLE_TESTS)
	ADD_LIBRARY(supacngTEST STATIC ${SHAREDSRCS})
  SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD} -fvisibility=default -DUNDER_TEST")
endif()

