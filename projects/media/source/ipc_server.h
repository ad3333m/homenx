#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITUBE_IPC_RESPONSE_MAX_DATA_SIZE \
  (0x100u - sizeof(HipcHeader) - 0x10u - sizeof(CmifOutHeader))

typedef struct SwitubeIpcServer {
  SmServiceName service_name;
  Handle handles[MAX_WAIT_OBJECTS];
  uint32_t max_handles;
  uint32_t handle_count;
} SwitubeIpcServer;

typedef struct SwitubeIpcServerRequest {
  HipcParsedRequest hipc;
  uint32_t command_id;
  const void* data;
  size_t data_size;
} SwitubeIpcServerRequest;

typedef Result (*SwitubeIpcServerHandler)(
    void* context, const SwitubeIpcServerRequest* request, uint8_t* output,
    size_t* output_size);

Result switubeIpcServerInitialize(SwitubeIpcServer* server, const char* name,
                                  uint32_t max_sessions);
Result switubeIpcServerExit(SwitubeIpcServer* server);
Result switubeIpcServerProcess(SwitubeIpcServer* server,
                               SwitubeIpcServerHandler handler,
                               void* context, uint64_t timeout);

#ifdef __cplusplus
}
#endif
