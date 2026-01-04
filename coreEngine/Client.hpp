#pragma once
 
#include <string>
#include <cstddef>
#include <sys/time.h>
#include <iostream>
#include <cstdlib>
#include <sys/socket.h> // for sockets
#include "../configReader/config.hpp"


//  #include "RequestParser.hpp"

struct requestData {
    long long _contentLength;
    std::string _Root;
    std::string _Index;
    std::string _Redirect;
    std::string _CgiPath;
    std::string _CgiExt;
    std::string _UploadDir;
    bool Autoindex;
    std::vector<std::string> _allowedMethods;
    std::string _rawHeader;
    std::string _body;
    int FD;
    std::string _uri;
    std::string _request;
    std::string _queryString;
    std::string  _fileName;
    std::string _httpVersion;
    std::string _method;
    std::map<std::string, std::string> _headers;
    requestData() : 
        _contentLength(0),
        _Root(""),
        _Index(""),
        _Redirect(""),
        _CgiPath(""),
        _CgiExt(""),
        _UploadDir(""),
        Autoindex(false),
        _allowedMethods(),
        _rawHeader(""),
        _body(""),
        FD(-1),
        _request(""),
        _queryString(""),
        _fileName(""),
        _uri(""),
        _httpVersion(""),
        _method(""),
        _headers()
        {}
};

 struct Client
{
    sockaddr_storage clientSockaddr;
    ServerConfig serverCfg;
    char inputBuffer[65536];
    std::string _buffer;
    std::string sendBuffer;
    size_t sendOffset;
    bool closeAfterSend;

    requestData _requestData;

    size_t _chunkedPos; // Tracks position in chunked body

    int byteRecived;
    size_t _maxBodySize;
    size_t _contentLength;

    bool hasError;
    bool _headerParsed;
    bool _isChunked;
    timeval lastAction;
    Client() : sendOffset(0), closeAfterSend(false), _chunkedPos(0),
               byteRecived(0), _maxBodySize(100 * 1024 * 1024), _contentLength(0),
               hasError(false), _headerParsed(false), _isChunked(false) {}
    ~Client() {}
}; 