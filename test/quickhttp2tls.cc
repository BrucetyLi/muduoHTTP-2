#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* 定义响应内容和头部 */
#define RESPONSE_DATA "Hello from HTTP/2 Server"
const nghttp2_nv headers[] = {
    {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
    {(uint8_t*)"content-type", (uint8_t*)"text/plain", 12, 10, NGHTTP2_NV_FLAG_NONE}
};

/* 发送回调：将HTTP/2帧数据写入TCP套接字 */
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data) {
    SSL *ssl = (SSL *)user_data;
    ssize_t sent = SSL_write(ssl, data, length);
    return (sent > 0) ? sent : NGHTTP2_ERR_CALLBACK_FAILURE;
}

typedef struct {
    const char *data;   // 响应数据指针
    size_t offset;      // 当前读取偏移量
} response_data_source; // 只是需要一种方式记录响应内容和当前读取偏移量（比如分块发送大文件、流式数据等）。自定义即可

response_data_source src = {
     RESPONSE_DATA,
    0
};

static ssize_t data_read_callback(nghttp2_session *session, 
                                  int32_t stream_id,
                                  uint8_t *buf, 
                                  size_t length,
                                  uint32_t *data_flags,
                                  nghttp2_data_source *source,
                                  void *user_data) {
    // 获取数据源
    response_data_source *src = (response_data_source*)source->ptr;
    
    // 计算剩余待发送数据长度
    size_t remaining = strlen(src->data) - src->offset;
    if(remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF; // 数据结束标记. 只有当你返回 0 并设置 NGHTTP2_DATA_FLAG_EOF，nghttp2 才会结束该流的数据发送。
        return 0;
    }

    // 计算本次可发送的数据量
    size_t send_len = (remaining > length) ? length : remaining;
    
    // 复制数据到缓冲区
    memcpy(buf, src->data + src->offset, send_len);
    src->offset += send_len;

    return send_len;    // 如果 data_read_callback 每次返回的字节数 > 0，nghttp2 会继续调用它，直到你返回 0
}


/* 接收帧回调：处理接收到的HTTP/2帧 */
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        src.offset = 0; // 重置响应数据偏移量
        // 构造响应数据提供器
        nghttp2_data_provider data_prd;
        data_prd.source.ptr = &src;
        data_prd.read_callback = data_read_callback;

        // 提交响应（流ID与请求一致）
        nghttp2_submit_response(session, frame->hd.stream_id, headers, 2, &data_prd);
    }
    return 0;
}

/* 头部回调：解析请求头 */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags, void *user_data) {
    printf("Received header: %.*s -> %.*s\n", (int)namelen, name, (int)valuelen, value);
    return 0;
}

int main() {
    // 初始化 OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    // 加载证书和私钥
    SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM);

    SSL_CTX_set_alpn_select_cb(ctx,
        [](SSL *ssl, const unsigned char **out, unsigned char *outlen,
           const unsigned char *in, unsigned int inlen, void *arg) -> int {
            int rv = nghttp2_select_next_protocol(   // 此函数自动帮我选 http/2
                (unsigned char **)out, outlen, in, inlen);
            if (rv == 1) {
                return SSL_TLSEXT_ERR_OK;
            }
            return SSL_TLSEXT_ERR_NOACK;
        }, NULL);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
         AF_INET,
        htons(9000),
        {INADDR_ANY}
    };
    bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 10);

    // 初始化nghttp2回调
    nghttp2_session_callbacks *callbacks; 
    nghttp2_session_callbacks_new(&callbacks); // 创建回调函数集合
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback); //设置发送回调：当需要发送HTTP/2帧数据时触发
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);

    while(1) {
        int client_fd = accept(sockfd, NULL, NULL);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            close(client_fd);
            continue;
        }
        const unsigned char *alpn = NULL;
        unsigned int alpnlen = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
        if (alpnlen > 0) {
            printf("ALPN negotiated: %.*s\n", alpnlen, alpn);
        } else {
            printf("No ALPN negotiated\n");
        }
        nghttp2_session *session;
        nghttp2_session_server_new(&session, callbacks, ssl); // 这里传 ssl 而不是 &client_fd

        // 发送初始SETTINGS帧
        nghttp2_settings_entry iv = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}; 
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &iv, 1); // 发送SETTINGS帧，设置最大并发流数为100

        uint8_t buf[4096];
        ssize_t recv_len;
        while((recv_len = SSL_read(ssl, buf, sizeof(buf))) > 0) {
            // 处理接收到的HTTP/2数据
            nghttp2_session_mem_recv(session, buf, recv_len); // 解析接收到的二进制HTTP/2帧数据
            // 发送待处理的HTTP/2帧
            nghttp2_session_send(session);  // 发送所有待处理的HTTP/2帧（如响应头、数据帧）
        }

        nghttp2_session_del(session);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
    }

    nghttp2_session_callbacks_del(callbacks);
    SSL_CTX_free(ctx);
    close(sockfd);
    return 0;
}