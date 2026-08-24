# Helper script for building Protocol Buffers definitions
# Run this after modifying protocol.proto

cmake_minimum_required(VERSION 3.16)
project(ProtocolBuild)

find_package(Protobuf REQUIRED)

message("Found Protobuf compiler: ${PROTOBUF_EXECUTABLE}")
message("Protobuf include directory: ${PROTOBUF_INCLUDE_DIR}")

# Generate .pb.h and .pb.cc from .proto file
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/common/include/protocol.proto
)

# Print what was generated
message("Generated sources:")
foreach(src ${PROTO_SRCS})
    message("  ${src}")
endforeach()
foreach(hdr ${PROTO_HDRS})
    message("  ${hdr}")
endforeach()

# Add generated sources to common library target (to be used by parent CMakeLists.txt)
set(GENERATED_PROTO_SRCS ${PROTO_SRCS} PARENT_SCOPE)
set(GENERATED_PROTO_HDRS ${PROTO_HDRS} PARENT_SCOPE)
