#include <iostream>
#include <string>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#include <functional>
#include <unordered_map>

#include "util.h"

struct all_data{
    nghttp2_session_callbacks *callbacks;
    connection_data *conn_data;
    nghttp2_session *session;
};

class http2Server
{
public:
    http2Server(muduo::net::EventLoop* loop,
        const muduo::net::InetAddress& listenAddr,
        const std::string& nameArg):_tcpServer(loop,listenAddr,nameArg),_loop(loop)
        {
            _tcpServer.setConnectionCallback(std::bind(&http2Server::ConnectionCallback, this  ,std::placeholders::_1));
            _tcpServer.setMessageCallback(std::bind(&http2Server::MessageCallback,this, std::placeholders::_1,std::placeholders::_2,std::placeholders::_3));
        }
    void setThreadNum(int num = 2)
    {
        _tcpServer.setThreadNum(num);
    }
    void start()
    {
        _tcpServer.start();
    }

private:
    void ConnectionCallback(const muduo::net::TcpConnectionPtr& conn)
    {
        if(!conn->connected())
        {
            if(all_data_map.find(conn) != all_data_map.end())
            {
                all_data *data = all_data_map[conn];
                nghttp2_session_del(data->session);
                nghttp2_session_callbacks_del(data->callbacks);
                free(data->conn_data);
                delete data;
                all_data_map.erase(conn);
            }
            conn->shutdown();
        }
        else
        {
            nghttp2_session_callbacks *callbacks; 
            nghttp2_session_callbacks_new(&callbacks);
            nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
            nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
            nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
            nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
            nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);

            connection_data *conn_data = (connection_data *)malloc(sizeof(connection_data));
            conn_data->client_fd = conn;
            conn_data->default_handler = &default_handler_impl; // Set default handler

            nghttp2_session *session;
            nghttp2_session_server_new(&session, callbacks, conn_data);

            // Set default handler if not set
            if (!conn_data->default_handler) {
                conn_data->default_handler = &default_handler_impl;
            }

            nghttp2_settings_entry iv = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}; 
            nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, &iv, 1);

            all_data *data = new all_data;
            data->callbacks = callbacks;
            data->conn_data = conn_data;
            data->session = session;
            all_data_map[conn] = data;
        }
    }
    
    void MessageCallback(const muduo::net::TcpConnectionPtr& conn,muduo::net::Buffer*buffer,muduo::Timestamp time)
    {
        uint8_t* begin = (uint8_t *)buffer->peek();
        ssize_t processed_len = nghttp2_session_mem_recv(all_data_map[conn]->session, begin, buffer->readableBytes());
        if (processed_len < 0) {
            std::cerr << "Error processing HTTP/2 data: " << nghttp2_strerror(processed_len) << std::endl;
            conn->shutdown();
            return;
        }
        buffer->retrieve(processed_len);
        nghttp2_session_send(all_data_map[conn]->session);

    }
    muduo::net::TcpServer _tcpServer;
    muduo::net::EventLoop* _loop;

    std::unordered_map<muduo::net::TcpConnectionPtr, all_data*> all_data_map;
};
