set(protobuf_BUILD_TESTS           OFF CACHE INTERNAL "")
set(protobuf_BUILD_EXAMPLES        OFF CACHE INTERNAL "")
set(protobuf_BUILD_PROTOC_BINARIES ON  CACHE INTERNAL "")
set(protobuf_INSTALL               OFF CACHE INTERNAL "")
set(protobuf_WITH_ZLIB             OFF CACHE INTERNAL "")
set(protobuf_BUILD_SHARED_LIBS     OFF CACHE INTERNAL "")

add_subdirectory(protobuf EXCLUDE_FROM_ALL)
