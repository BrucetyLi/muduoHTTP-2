#include "util.h"
#include <dirent.h>
#include <iostream>

// http/2相关

// Default request handler implementation 默认是实现回声服务
void default_request_handler(RequestHandler *self, 
                             nghttp2_session *session, 
                             int32_t stream_id, 
                             stream_data *sdata) {
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
        return;
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
    
    // Set response fields (preserve request body)
    sdata->response_body = response_body;
    sdata->response_len = response_len;  // length without null terminator
    sdata->response_offset = 0;
    
    // Prepare data provider
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = sdata;
    data_prd.read_callback = data_read_callback;
    
    // Submit response
    nghttp2_submit_response(session, stream_id, headers, 2, &data_prd);
}

// API request handler implementation
void api_request_handler(RequestHandler *self, 
                         nghttp2_session *session, 
                         int32_t stream_id, 
                         stream_data *sdata) {
    // Prepare response headers
    const nghttp2_nv headers[] = {
        {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-type", (uint8_t*)"application/json", 12, 16, NGHTTP2_NV_FLAG_NONE}
    };
    
    // Prepare JSON response
    const char *json = "{\"status\":\"success\",\"message\":\"API response\"}";
    size_t json_len = strlen(json);
    
    // Allocate separate memory for response body
    char *response_body = (char *)malloc(json_len);
    if (!response_body) {
        return;
    }
    memcpy(response_body, json, json_len);
    
    // Set response fields (preserve request body)
    sdata->response_body = response_body;
    sdata->response_len = json_len;
    sdata->response_offset = 0;
    
    // Prepare data provider
    nghttp2_data_provider data_prd;
    data_prd.source.ptr= sdata;
    data_prd.read_callback = data_read_callback;
    
    // Submit response
    nghttp2_submit_response(session, stream_id, headers, 2, &data_prd);
}

// Root request handler implementation
void root_request_handler(RequestHandler *self, 
                          nghttp2_session *session, 
                          int32_t stream_id, 
                          stream_data *sdata) {
    // Prepare response headers
    const nghttp2_nv headers[] = {
        {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-type", (uint8_t*)"text/html", 12, 9, NGHTTP2_NV_FLAG_NONE}
    };
    
    // Prepare HTML response
    const char *html = "<html><body><h1>Welcome to Root</h1></body></html>";
    size_t html_len = strlen(html);
    
    // Allocate separate memory for response body
    char *response_body = (char *)malloc(html_len);
    if (!response_body) {
        return;
    }
    memcpy(response_body, html, html_len);
    
    // Set response fields (preserve request body)
    sdata->response_body = response_body;
    sdata->response_len = html_len;
    sdata->response_offset = 0;
    
    // Prepare data provider
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = sdata;
    data_prd.read_callback = data_read_callback;
    
    // Submit response
    nghttp2_submit_response(session, stream_id, headers, 2, &data_prd);
}

ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data) {
    muduo::net::TcpConnectionPtr conn = *(muduo::net::TcpConnectionPtr *)user_data;
    conn->send(data,length); 
    return (length >= 0) ? length : NGHTTP2_ERR_CALLBACK_FAILURE; 
}


/* Data read callback for response body */
ssize_t data_read_callback(nghttp2_session *session, 
                                  int32_t stream_id,
                                  uint8_t *buf, 
                                  size_t length,
                                  uint32_t *data_flags,
                                  nghttp2_data_source *source,
                                  void *user_data) {
    stream_data *sdata = (stream_data *)source->ptr;
    
    // Use response_body for sending response
    size_t remaining = sdata->response_len - sdata->response_offset;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF; // End of data flag
        return 0;
    }

    // Calculate how much data to send this time
    size_t send_len = (remaining > length) ? length : remaining;
    
    // Copy data to buffer
    memcpy(buf, sdata->response_body + sdata->response_offset, send_len);
    sdata->response_offset += send_len;

    return send_len; 
}


/* Header callback: collect request headers and set handler based on path */
int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && 
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        
        // Get stream user data
        stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        connection_data *conn_data = (connection_data *)user_data;
        
        if (!sdata) {
            // Allocate stream data if not exists
            sdata = (stream_data *)calloc(1, sizeof(stream_data));
            nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sdata);
            
            // Set default handler for this stream
            sdata->handler = conn_data->default_handler;
        }
        
        // Check if this is the :path header
        if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            // Set handler based on path
            if (valuelen >= 4 && memcmp(value, "/api", 4) == 0) {
                sdata->handler = &api_handler_impl;
            } else if (valuelen == 1 && memcmp(value, "/", 1) == 0) {
                sdata->handler = &root_handler_impl;
            }
            // Otherwise keep the default handler
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
int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
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
int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
    // Only process when we have END_STREAM flag (request complete)
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        int32_t stream_id = frame->hd.stream_id;
        stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, stream_id);
        
        if (!sdata) {
            // This should not happen because we create stream data in header callback
            return 0;
        }
        
        // If handler is set, let it handle the request
        if (sdata->handler && sdata->handler->handle_request) {
            sdata->handler->handle_request(sdata->handler, session, stream_id, sdata);
        }
    }
    return 0;
}


/* Stream close callback: clean up resources */
int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                                    uint32_t error_code, void *user_data) {
    stream_data *sdata = (stream_data *)nghttp2_session_get_stream_user_data(session, stream_id);
    if (sdata) {
        if (sdata->headers) free(sdata->headers);
        if (sdata->body) free(sdata->body);
        if (sdata->response_body) free(sdata->response_body); // Free response body
        free(sdata);
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    }
    return 0;
}


// Global handler instances
RequestHandler default_handler_impl = {
    .handle_request = default_request_handler,
    .data = NULL
};

RequestHandler api_handler_impl = {
    .handle_request = api_request_handler,
    .data = NULL
};

RequestHandler root_handler_impl = {
    .handle_request = root_request_handler,
    .data = NULL
};
