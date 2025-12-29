// Copyright [2025] <Piotr Ruszkiewicz> <pruszkie@student.42warsaw.pl>

#ifndef COREENGINE_HPP
#define COREENGINE_HPP

#include <arpa/inet.h> // inet_pton, inet_ntop DELETE if dont need
#include <cerrno>      // defines errno
#include <cstring>     // defines strerror()
#include <fcntl.h>     // to set NONBLOCK
#include <iostream>
#include <netinet/in.h> // struct sockaddr_in, struct in_addr
#include <netdb.h>      // for getaddrinfo and addrinfo
#include <poll.h>       // for poolfd
#include <string>
#include <stdint.h>
#include <stdlib.h>     // for exit
#include <sys/socket.h> // for sockets
#include <unistd.h>     // for close read write
#include <vector>
#include <sstream>
#include <vector>
#include <sys/time.h> 

#include "../configReader/config.hpp"
#include "../errors/error.hpp"
#include "../http/Http.hpp"

class CoreEngine
{
private:
    struct client
        {
            sockaddr_storage clientSockaddr;
            ServerConfig serverCfg;
            std::string sendBuffer;
            std::string leftooverBuffer;
            std::vector<std::string> requestBufferVec;
            char inputBuffer[1024];
            size_t sendOffset;
            bool hasError;
            int byteRecived;
            int FD;
            bool isHeader;
            bool isChunked;
            bool hasContentLength;
            bool bodyComplete;
            timeval lastAction;
            std::string fullHeader;
            int uploadFile;
            size_t contentLength;
        }; 
        client &getClientByFD(int socketFD);
        void closeCLient(int el);
        std::string prepareResponse(client &client, size_t el, size_t res);
        addrinfo hints;                       // base config of server
        std::vector<ServerConfig> serversCfg; // list of object parsed cfgfiles
        addrinfo *serv;
        std::vector<int> socketFD;    // vector holding all socketsFD
        std::vector<bool> isClientFD; // consider switching it to map
        std::vector<client> clientVec;
        std::vector<pollfd> pollFDs;
        size_t lSockNum;   // number of listening sockets
        int poolTimeout;   // time interval for poll() checking for event
        size_t pollFDsNum; // number of structs corresponding total number of sockets
        size_t backlogNum; // number of pending connection that listen can hold
        int idleClientTimeout;
        bool appendHeader(client *client);
        void appendChunks(client *client);
        void appendContnent(client *client);

    public:
        CoreEngine(const std::vector<ServerConfig> &serverCfg);
        ~CoreEngine();
        void setSocket(size_t i);
        void setConnection(size_t i);
        void recivNClose(size_t i);
        void sendToClient(size_t i);
        void coreEngine();
        std::string generatePath();
        long getTimeStamp(timeval countFrom, timeval compareTime);
        static volatile sig_atomic_t alive;
};

#endif