#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <muduo/net/TcpConnection.h>

#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <nghttp2/nghttp2.h>


// http2 相关处理
typedef struct RequestHandler RequestHandler;

typedef struct {
    char *headers;         // Collected request headers
    size_t headers_len;
    char *body;            // Collected request body
    size_t body_len;
    
    char *response_body;   // Response body to send
    size_t response_len;
    size_t response_offset;
    
    RequestHandler *handler;
} stream_data;


// Per-connection data structure
typedef struct {
    muduo::net::TcpConnectionPtr client_fd;                      // Client file descriptor
    RequestHandler *default_handler;    // Default request handler
} connection_data;

// Request handler interface
struct RequestHandler {
    // Handle request and prepare response
    void (*handle_request)(RequestHandler *self, 
                           nghttp2_session *session, 
                           int32_t stream_id, 
                           stream_data *sdata);
    
    // Optional: additional data for handler
    void *data;
};

void default_request_handler(RequestHandler *self, nghttp2_session *session, int32_t stream_id, stream_data *sdata);

void api_request_handler(RequestHandler *self, nghttp2_session *session, int32_t stream_id, stream_data *sdata);

void root_request_handler(RequestHandler *self, nghttp2_session *session, int32_t stream_id, stream_data *sdata);

ssize_t send_callback(nghttp2_session *session, const uint8_t *data,size_t length, int flags, void *user_data);

ssize_t data_read_callback(nghttp2_session *session, int32_t stream_id,uint8_t *buf, size_t length,
                            uint32_t *data_flags,nghttp2_data_source *source,void *user_data);

int on_header_callback(nghttp2_session *session,const nghttp2_frame *frame, const uint8_t *name,
                        size_t namelen, const uint8_t *value,size_t valuelen, uint8_t flags, void *user_data);

int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,int32_t stream_id, 
                                const uint8_t *data,size_t len, void *user_data);

int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);

int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,uint32_t error_code, void *user_data);

extern RequestHandler default_handler_impl;
extern RequestHandler api_handler_impl;
extern RequestHandler root_handler_impl;


