#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <nghttp2/nghttp2.h>

// Per-stream data structure
typedef struct {
    char *headers;      // Collected request headers
    size_t headers_len; // Length of headers string
    char *body;         // Collected request body
    size_t body_len;    // Length of body data
    size_t body_offset; // Current read offset for body in response
} stream_data;

// Per-connection data structure
typedef struct {
    int client_fd;                      // Client file descriptor
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
    stream_data *sdata = (stream_data *)source->ptr;
    
    // Calculate remaining data to send
    size_t remaining = sdata->body_len - sdata->body_offset;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF; // End of data flag
        return 0;
    }

    // Calculate how much data to send this time
    size_t send_len = (remaining > length) ? length : remaining;
    
    // Copy data to buffer
    memcpy(buf, sdata->body + sdata->body_offset, send_len);
    sdata->body_offset += send_len;

    return send_len; 
}

/* Header callback: collect request headers */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && 
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        
        // Get stream user data
        stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (!sdata) {
            // Allocate stream data if not exists
            sdata = (stream_data *)calloc(1, sizeof(stream_data));
            nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sdata);
        }
        
        // Format header: "name: value\n" (without null terminator for intermediate strings)
        size_t content_len = namelen + valuelen + 3; // name + ": " + value + "\n" (no null terminator)
        char *header_str = (char *)malloc(content_len + 1); // +1 for null terminator
        snprintf(header_str, content_len + 1, "%.*s: %.*s\n", (int)namelen, name, (int)valuelen, value);
        
        // Append to headers string
        if (sdata->headers) {
            // Calculate new length: current content length + new content length + null terminator
            size_t new_content_len = sdata->headers_len - 1; // exclude existing null terminator
            size_t new_total_len = new_content_len + content_len + 1; // +1 for new null terminator
            sdata->headers = (char *)realloc(sdata->headers, new_total_len);
            
            // Copy new content (overwriting the old null terminator)
            memcpy(sdata->headers + new_content_len, header_str, content_len);
            sdata->headers[new_total_len - 1] = '\0'; // add new null terminator
            sdata->headers_len = new_total_len;
        } else {
            sdata->headers = header_str;
            sdata->headers_len = content_len + 1; // include null terminator
        }
    }
    return 0;
}

/* Data receive callback: collect request body */
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                       int32_t stream_id, const uint8_t *data,
                                       size_t len, void *user_data) {
    stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (!sdata) {
        sdata = (stream_data *)calloc(1, sizeof(stream_data));
        nghttp2_session_set_stream_user_data(session, stream_id, sdata);
    }
    
    // Append data to body
    if (sdata->body) {
        size_t new_len = sdata->body_len + len;
        sdata->body = (char *)realloc(sdata->body, new_len);
        memcpy(sdata->body + sdata->body_len, data, len);
        sdata->body_len = new_len;
    } else {
        sdata->body = (char *)malloc(len);
        memcpy(sdata->body, data, len);
        sdata->body_len = len;
    }
    
    return 0;
}

/* Frame receive callback: process received HTTP/2 frames */
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && 
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        
        int32_t stream_id = frame->hd.stream_id;
        stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, stream_id);
        
        if (!sdata) {
            sdata = (stream_data *)calloc(1, sizeof(stream_data));
            nghttp2_session_set_stream_user_data(session, stream_id, sdata);
        }
        
        // Prepare response headers
        const nghttp2_nv headers[] = {
            {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)"content-type", (uint8_t*)"text/plain", 12, 10, NGHTTP2_NV_FLAG_NONE}
        };
        
        // Prepare response body: headers + body
        const char *header_prefix = "Headers:\n";
        const char *separator = "\n\nBody:\n";
        
        // Calculate lengths
        size_t header_prefix_len = strlen(header_prefix);
        size_t separator_len = strlen(separator);
        size_t headers_len = sdata->headers ? sdata->headers_len - 1 : 0; // exclude null terminator
        size_t body_len = sdata->body ? sdata->body_len : 0;
        
        // Total response length (without null terminator)
        size_t response_len = header_prefix_len + headers_len + separator_len + body_len;
        
        // Allocate memory for response body (plus null terminator)
        char *response_body = (char *)malloc(response_len + 1);
        if (!response_body) {
            // Handle allocation failure
            return 0;
        }
        
        char *ptr = response_body;
        
        // Build response body manually
        if (sdata->headers) {
            memcpy(ptr, header_prefix, header_prefix_len);
            ptr += header_prefix_len;
            
            memcpy(ptr, sdata->headers, headers_len);
            ptr += headers_len;
        }
        
        memcpy(ptr, separator, separator_len);
        ptr += separator_len;
        
        if (sdata->body) {
            memcpy(ptr, sdata->body, body_len);
            ptr += body_len;
        }
        
        // Add null terminator
        *ptr = '\0';
        
        // Free old body and set new response body
        if (sdata->body) free(sdata->body);
        sdata->body = response_body;
        sdata->body_len = response_len;  // length without null terminator
        sdata->body_offset = 0;
        
        // Prepare data provider
        nghttp2_data_provider data_prd;
        data_prd.source.ptr = sdata;
        data_prd.read_callback = data_read_callback;
        
        // Submit response
        nghttp2_submit_response(session, stream_id, headers, 2, &data_prd);
    }
    return 0;
}

/* Stream close callback: clean up resources */
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data) {
    stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (sdata) {
        if (sdata->headers) free(sdata->headers);
        if (sdata->body) free(sdata->body);
        free(sdata);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }
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
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    
    // Create HTTP/2 session for this connection
    nghttp2_session *session;
    nghttp2_session_server_new(&session, callbacks, &client_fd);
    
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
        
        // Create thread for this connection
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_connection, conn_data);
        pthread_detach(thread_id); // Detach thread to avoid resource leak
    }

    close(sockfd);
    return 0;
}
