#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <nghttp2/nghttp2.h>

// Response content and headers
#define RESPONSE_DATA "Hello from HTTP/2 Server"
const nghttp2_nv headers[] = {
    {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
    {(uint8_t*)"content-type", (uint8_t*)"text/plain", 12, 10, NGHTTP2_NV_FLAG_NONE}
};

// Response data source structure
typedef struct {
    const char *data;   // Response data pointer
    size_t offset;      // Current read offset
} response_data_source;

// Per-connection data structure
typedef struct {
    int client_fd;                      // Client file descriptor
    response_data_source src;           // Response data source for this connection
} connection_data;

/* Send callback: write HTTP/2 frame data to TCP socket */
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data) {
    int fd = *(int *)user_data;
    ssize_t sent = send(fd, data, length, 0);
    return (sent >= 0) ? sent : NGHTTP2_ERR_CALLBACK_FAILURE;
}

/* Data read callback for response body */
static ssize_t data_read_callback(nghttp2_session *session, 
                                  int32_t stream_id,
                                  uint8_t *buf, 
                                  size_t length,
                                  uint32_t *data_flags,
                                  nghttp2_data_source *source,
                                  void *user_data) {
    // Get data source
    response_data_source *src = (response_data_source*)source->ptr;
    
    // Calculate remaining data to send
    size_t remaining = strlen(src->data) - src->offset;
    if(remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF; // End of data flag
        return 0;
    }

    // Calculate how much data to send this time
    size_t send_len = (remaining > length) ? length : remaining;
    
    // Copy data to buffer
    memcpy(buf, src->data + src->offset, send_len);
    src->offset += send_len;

    return send_len; 
}

/* Frame receive callback: process received HTTP/2 frames */
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        // Get connection-specific data
        connection_data *conn_data = (connection_data *)user_data;
        
        // Reset response offset for this connection
        conn_data->src.offset = 0;
        
        // Construct response data provider
        nghttp2_data_provider data_prd;
        data_prd.source.ptr = &conn_data->src;
        data_prd.read_callback = data_read_callback;

        // Submit response
        nghttp2_submit_response(session, frame->hd.stream_id, headers, 2, &data_prd);
    }
    return 0;
}

/* Header callback: parse request headers */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data) {
    printf("Received header: %.*s -> %.*s\n", (int)namelen, name, (int)valuelen, value);
    return 0;
}

/* Thread function to handle a single connection */
void *handle_connection(void *arg) {
    connection_data *conn_data = (connection_data *)arg;
    int client_fd = conn_data->client_fd;
    
    // Initialize nghttp2 callbacks
    nghttp2_session_callbacks *callbacks; 
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    
    // Create HTTP/2 session for this connection
    nghttp2_session *session;
    nghttp2_session_server_new(&session, callbacks, conn_data);
    
    // Send initial SETTINGS frame
    nghttp2_settings_entry iv = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}; 
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &iv, 1);
    
    uint8_t recv_buf[4096];
    size_t buffer_len = 0;
    
    while (1) {
        ssize_t recv_len = recv(client_fd, recv_buf + buffer_len, sizeof(recv_buf) - buffer_len, 0);
        if (recv_len <= 0) break;
        
        buffer_len += recv_len;
        ssize_t processed_len = nghttp2_session_mem_recv(session, recv_buf, buffer_len);
        
        if (processed_len < 0) {
            break;
        }
        
        if ((size_t)processed_len < buffer_len) {
            memmove(recv_buf, recv_buf + processed_len, buffer_len - processed_len);
        }
        buffer_len -= processed_len;
        
        // Send response
        nghttp2_session_send(session);
    }

    // Cleanup
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    close(client_fd);
    free(conn_data);
    
    return NULL;
}

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        AF_INET,
        htons(8443),
        INADDR_ANY
    };
    bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 10);

    while(1) {
        int client_fd = accept(sockfd, NULL, NULL);
        
        // Create per-connection data
        connection_data *conn_data = (connection_data *)malloc(sizeof(connection_data));
        conn_data->client_fd = client_fd;
        conn_data->src.data = RESPONSE_DATA;
        conn_data->src.offset = 0;
        
        // Create thread for this connection
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_connection, conn_data);
        pthread_detach(thread_id); // Detach thread to avoid resource leak
    }

    close(sockfd);
    return 0;
}
