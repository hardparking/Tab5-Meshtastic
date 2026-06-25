# Meshtastic protobufs (nanopb 0.4.9.1 runtime + generated meshtastic/*.pb.c).
# Generated offline with nanopb_generator from github.com/meshtastic/protobufs.
file(GLOB NANOPB_SRCS  "nanopb/*.c")
file(GLOB PROTO_SRCS   "proto/meshtastic/*.pb.c")

idf_component_register(
    SRCS ${NANOPB_SRCS} ${PROTO_SRCS}
    INCLUDE_DIRS "nanopb" "proto"
)

# nanopb default field sizes are fine; silence a couple of pedantic warnings
# in generated/runtime code so it doesn't trip our -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-unused-function)
