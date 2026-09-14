build_lib_example(
  NAME mtp-manual-partition-validation
  SOURCE_FILES mtp-manual-partition-validation.cc
  LIBRARIES_TO_LINK
    ${libmtp}
)

build_lib_example(
  NAME mtp-past-event-validation
  SOURCE_FILES mtp-past-event-validation.cc
  LIBRARIES_TO_LINK
    ${libmtp}
)

build_lib_example(
  NAME mtp-remote-event-ordering
  SOURCE_FILES mtp-remote-event-ordering.cc
  LIBRARIES_TO_LINK
    ${libmtp}
)

build_lib_example(
  NAME mtp-ordered-global-events
  SOURCE_FILES mtp-ordered-global-events.cc
  LIBRARIES_TO_LINK
    ${libmtp}
)

build_lib_example(
  NAME mtp-zero-delay-partition
  SOURCE_FILES mtp-zero-delay-partition.cc
  LIBRARIES_TO_LINK
    ${libmtp}
    ${libpoint-to-point}
)

build_lib_example(
  NAME simple-mtp
  SOURCE_FILES simple-mtp.cc
  LIBRARIES_TO_LINK
    ${libmtp}
    ${libpoint-to-point}
    ${libinternet}
    ${libnix-vector-routing}
    ${libapplications}
)

build_lib_example(
  NAME fat-tree-mtp
  SOURCE_FILES fat-tree-mtp.cc
  LIBRARIES_TO_LINK
    ${libmtp}
    ${libpoint-to-point}
    ${libinternet}
    ${libnix-vector-routing}
    ${libapplications}
    ${libflow-monitor}
)
