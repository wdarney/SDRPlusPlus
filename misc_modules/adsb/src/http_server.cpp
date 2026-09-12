#include "http_server.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <map>
#include <cerrno>
#include <cstring>

namespace adsb {
bool HttpServer::start(const std::string& assets, Handler handler, std::string& error) {
    stop();
    if (!std::filesystem::is_regular_file(std::filesystem::path(assets)/"index.html")) {
        error = "Bundled tar1090 assets not found: " + assets; return false;
    }
    assets_ = std::filesystem::canonical(assets).string();
    handler_ = std::move(handler);
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) { error = strerror(errno); return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(socket_,16)) {
        error = strerror(errno); ::close(socket_); socket_=-1; return false;
    }
    socklen_t length = sizeof(address);
    getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    stopping_ = false;
    worker_ = std::thread(&HttpServer::run, this);
    return true;
}
void HttpServer::stop() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1; port_ = 0;
}
Response HttpServer::route(const std::string& path) {
    if (path.rfind("/data/",0)==0) return handler_(path);
    if (path == "/chunks/chunks.json") return {404,"application/json","{}"};
    if (path == "/upintheair.json") return {404,"application/json","{}"};
    // Metadata is optional; an empty local database avoids external lookups.
    if (path == "/db2/ranges.js") return {200,"application/json","{\"military\":[]}"};
    if (path.rfind("/db2/",0)==0) return {200,"application/json","{}"};
    if (path.empty() || path[0]!='/' || path.find("..")!=std::string::npos ||
        path.find('%')!=std::string::npos || path.find('\\')!=std::string::npos)
        return {400,"text/plain","Invalid path"};
    std::error_code ec;
    auto file = std::filesystem::weakly_canonical(std::filesystem::path(assets_) / (path=="/" ? "index.html" : path.substr(1)),ec);
    if (ec || file.string().rfind(assets_+"/",0)!=0 || !std::filesystem::is_regular_file(file,ec))
        return {404,"text/plain","Not found"};
    auto size = std::filesystem::file_size(file,ec);
    if (ec || size > 16*1024*1024) return {413,"text/plain","Asset too large"};
    static const std::map<std::string,std::string> types = {
        {".html","text/html; charset=utf-8"},{".js","application/javascript"},{".css","text/css"},
        {".json","application/json"},{".geojson","application/geo+json"},{".png","image/png"},
        {".svg","image/svg+xml"},{".jpg","image/jpeg"},{".woff2","font/woff2"},{".ico","image/x-icon"}};
    std::ifstream stream(file,std::ios::binary);
    if (!stream) return {404,"text/plain","Not found"};
    auto it=types.find(file.extension().string());
    return {200,it==types.end()?"application/octet-stream":it->second,
        std::string(std::istreambuf_iterator<char>(stream),{})};
}
void HttpServer::run() {
    while (!stopping_) {
        pollfd p{socket_,POLLIN,0};
        if (::poll(&p,1,100)<=0 || !(p.revents&POLLIN)) continue;
        int client = ::accept(socket_,nullptr,nullptr);
        if (client < 0) continue;
#ifdef SO_NOSIGPIPE
        int yes=1; setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
        timeval timeout{0,200000};
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        std::string request;
        while (!stopping_ && request.size()<8192 && request.find("\r\n\r\n")==std::string::npos && std::chrono::steady_clock::now()<deadline) {
            char buffer[1024]; auto n=recv(client,buffer,sizeof(buffer),0);
            if (n>0) request.append(buffer,n);
            else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) break;
        }
        Response response{400,"text/plain","Invalid request"};
        std::istringstream input(request);
        std::string method,target,version;
        input>>method>>target>>version;
        std::string line,host;
        std::getline(input,line);
        while (std::getline(input,line) && line!="\r") {
            auto colon=line.find(':');
            if(colon==std::string::npos) continue;
            auto key=line.substr(0,colon);
            for(char& c:key)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if(key=="host") { host=line.substr(colon+1); host.erase(0,host.find_first_not_of(" \t")); if(!host.empty()&&host.back()=='\r')host.pop_back(); }
        }
        if(request.find("\r\n\r\n")!=std::string::npos && request.size()<=8192) {
            if(host!="127.0.0.1:"+std::to_string(port_)) response={403,"text/plain","Invalid host"};
            else if(method!="GET" && method!="HEAD") response={405,"text/plain","Read-only endpoint"};
            else try { response=route(target.substr(0,target.find('?'))); }
            catch(const std::exception&) { response={500,"text/plain","Request failed"}; }
        }
        std::string output="HTTP/1.1 "+std::to_string(response.status)+" Response\r\nContent-Type: "+response.type+
            "\r\nContent-Length: "+std::to_string(response.body.size())+
            "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
        if(method!="HEAD")output+=response.body;
        size_t sent=0;
        while(!stopping_ && sent<output.size() && std::chrono::steady_clock::now()<deadline) {
            auto n=::send(client,output.data()+sent,output.size()-sent,0);
            if(n>0)sent+=n; else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)break;
        }
        ::close(client);
    }
}
}
