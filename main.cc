#include <iostream>
#include <http2Server.hpp>

int main(int argc, char* argv[])
{
    if(argc <2)
    {
        std::cout << "./muduohttp port "<< std::endl; 
        return 0;
    }
    unsigned short port = atoi(argv[1]);
    muduo::net::EventLoop loop;
    muduo::net::InetAddress addr("0.0.0.0", port);
    http2Server httpserver(&loop,addr,"myHTTPserver");
    httpserver.setThreadNum(4);
    httpserver.start();
    loop.loop();
    return 0;
}