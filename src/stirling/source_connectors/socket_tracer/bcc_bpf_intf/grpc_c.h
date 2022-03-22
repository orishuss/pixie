#pragma once

// The amount of bytes in a single slice of data.
// This value was not chosen according to some constant in the grpc-c library.
// Largest I saw was 1293.
#define GRPC_C_SLICE_SIZE (16380)

// This needs to not be lower than 8 (which is the maximum amount of inlined
// slices in a grpc_slice_buffer). The real maximum size isn't known - it can
// probably be larger than 8. Until now I have not seen a size larger than 2 used,
// so 8 is more than enough.
#define SIZE_OF_DATA_SLICE_ARRAY (8)

#define GRPC_C_DEFAULT_MAP_SIZE (10240)

#define MAXIMUM_AMOUNT_OF_ITEMS_IN_METADATA (30)
#define MAXIMUM_LENGTH_OF_KEY_IN_METADATA (44)
#define MAXIMUM_LENGTH_OF_VALUE_IN_METADATA (100)

#define GRPC_C_EVENT_DIRECTION_UNKNOWN (0)
#define GRPC_C_EVENT_DIRECTION_OUTGOING (1)
#define GRPC_C_EVENT_DIRECTION_INCOMING (2)

enum grpc_c_version_e
{
    GRPC_C_VERSION_UNSUPPORTED = 0,
    GRPC_C_V1_19_0,
    GRPC_C_V1_24_1,
    GRPC_C_V1_33_2,
    GRPC_C_V1_41_1,
    GRPC_C_VERSION_LAST
};

struct grpc_c_data_slice_t
{
    uint32_t slice_len;
    char bytes[GRPC_C_SLICE_SIZE];
};
// This must be aligned to 8-bytes.
// Because of this, the length of the bytes array
// must be (length % 8) == 4 to accommodate for the uint32_t.
// UPDATE: assert has been commented out, doesn't work on kernels 4*
// static_assert( (sizeof(struct grpc_c_data_slice_t) % 8) == 0, "gRPC-C data slice is not aligned to 8-bytes." );

struct grpc_c_metadata_item_t
{
    char key[MAXIMUM_LENGTH_OF_KEY_IN_METADATA];
    char value[MAXIMUM_LENGTH_OF_VALUE_IN_METADATA];
};

struct grpc_c_metadata_t
{
    uint64_t count;
    struct grpc_c_metadata_item_t items[MAXIMUM_AMOUNT_OF_ITEMS_IN_METADATA];
};


#define GRPC_C_EVENT_COMMON \
    struct conn_id_t conn_id; \
    uint32_t stream_id; \
    uint64_t timestamp; \
    int32_t stack_id; \
    uint32_t direction;

#define GRPC_C_EVENT_DATA_FOR_SEND_AND_RECEIVE \
    GRPC_C_EVENT_COMMON; \
    uint64_t position_in_stream;

struct grpc_c_header_event_data_t
{
    GRPC_C_EVENT_COMMON;
    struct grpc_c_metadata_item_t header;
};

struct grpc_c_event_data_t
{
    GRPC_C_EVENT_DATA_FOR_SEND_AND_RECEIVE;
    struct grpc_c_data_slice_t slice;
};

struct grpc_c_stream_closed_data
{
    GRPC_C_EVENT_COMMON;
    uint32_t read_closed;
    uint32_t write_closed;
};

#undef GRPC_C_EVENT_DATA_FOR_SEND_AND_RECEIVE
#undef GRPC_C_EVENT_COMMON