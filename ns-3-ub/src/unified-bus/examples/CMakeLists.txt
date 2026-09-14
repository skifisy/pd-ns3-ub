set(ub_quick_example_libraries
    ${libcore}
    ${libconfig-store}
    ${libpoint-to-point}
)

if(${ENABLE_MTP})
  list(APPEND ub_quick_example_libraries ${libmtp})
endif()

if(${ENABLE_MPI})
  list(APPEND ub_quick_example_libraries ${libmpi})
endif()

build_lib_example(
  NAME ub-quick-example
  SOURCE_FILES
    ub-quick-example.cc
    ub-case-runner.cc
  LIBRARIES_TO_LINK ${ub_quick_example_libraries}
)

if(${ENABLE_MPI} AND ${ENABLE_MTP})
  build_lib_example(
    NAME ub-mtp-remote-tp-regression
    SOURCE_FILES ub-mtp-remote-tp-regression.cc
    LIBRARIES_TO_LINK
      ${libmpi}
      ${libmtp}
      ${libnetwork}
  )
endif()
