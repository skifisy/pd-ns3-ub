set(example_as_test_suite)
if(${ENABLE_EXAMPLES})
  list(APPEND example_as_test_suite
       test/mtp-runtime-test-suite.cc
  )
  if(${ENABLE_MPI})
    list(APPEND example_as_test_suite
         test/hybrid-test-suite.cc
    )
  else()
    list(APPEND example_as_test_suite
         test/mtp-test-suite.cc
    )
  endif()
endif()

set(mtp_libs ${libnetwork})
if(${ENABLE_MPI})
  list(APPEND mtp_libs MPI::MPI_CXX)
endif()

build_lib(
  LIBNAME mtp
  SOURCE_FILES
    model/logical-process.cc
    model/mtp-interface.cc
    model/multithreaded-simulator-impl.cc
  HEADER_FILES
    model/logical-process.h
    model/mtp-interface.h
    model/multithreaded-simulator-impl.h
  LIBRARIES_TO_LINK ${mtp_libs}
  TEST_SOURCES ${example_as_test_suite}
)
