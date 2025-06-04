#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string>
#include <string.h>

void handle_http11(int client_fd) {
    char buffer[4096];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    
    // 解析文本协议
    std::string request(buffer, bytes_read);
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = request.substr(0, header_end);
    
    // 构造响应
    const char* response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n\r\n"
        "Hello World!";
    
    write(client_fd, response, strlen(response));
}

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{AF_INET, htons(8080), INADDR_ANY};
    bind(sockfd, (sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 10);
    
    while(true) {
        int client_fd = accept(sockfd, nullptr, nullptr);
        handle_http11(client_fd);
        close(client_fd);
    }
}