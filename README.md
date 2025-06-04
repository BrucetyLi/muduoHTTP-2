一个基于muduo库与nghttp2的http/2多线程反应堆服务器，用作学习使用，简单实现回显功能。
<!--
g++ -o quickhttp2tls.out quickhttp2tls.cc -lnghttp2 -lssl -lcrypto


 带tls的版本：
curl --http2-prior-knowledge -k https://127.0.0.1:9000/
nghttp -v https://127.0.0.1:9000/


不带tls的版本
curl --http2-prior-knowledge -k http://127.0.0.1:8443/
nghttp -v http://127.0.0.1:8443/


回显服务器：
curl --http2-prior-knowledge -k http://127.0.0.1:8443/ \
  -H "Content-Type: application/json" \
  -d '{"key": "value"}' -->
