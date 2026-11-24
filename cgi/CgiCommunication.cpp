#include "CgiCommunication.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <algorithm>
#include <cstdio>

static void handleCgiWrite(int fd, const std::string& body, size_t& bytesWritten, bool& writeClosed) {
    size_t chunkSize = 65536; // 64KB
    size_t remaining = body.size() - bytesWritten;
    size_t toWrite = std::min(chunkSize, remaining);
    
    ssize_t written = write(fd, body.c_str() + bytesWritten, toWrite);
    if (written > 0) {
        bytesWritten += written;
    } else if (written == -1) {
        perror("write error");
        writeClosed = true;
        close(fd);
        return;
    }
    
    if (bytesWritten == body.size()) {
        close(fd);
        writeClosed = true;
        std::cout << "DEBUG: Finished writing body, closed pipeStin[1]" << std::endl;
    }
}

static bool handleCgiRead(int fd, std::string& responseBody) {
    char buffer[4096];
    ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
    if (bytesRead > 0) {
        responseBody.append(buffer, bytesRead);
        return true;
    } else if (bytesRead == 0) {
        std::cout << "DEBUG: EOF from child" << std::endl;
        return false;
    } else {
        perror("read error");
        return false;
    }
}

std::string communicateWithCgi(int pipeStout[2], int pipeStin[2], pid_t pid, const std::string& body) {
    std::cout << "DEBUG: Parent process, child pid: " << pid << std::endl;
    close(pipeStout[1]); // Close write end of stdout pipe
    close(pipeStin[0]); // Close read end of stdin pipe
    
    std::string responseBody;
    size_t bytesWritten = 0;
    bool writeClosed = false;

    if (body.empty()) {
        close(pipeStin[1]);
        writeClosed = true;
        std::cout << "DEBUG: Body empty, closed pipeStin[1]" << std::endl;
    }

    while (true) {
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        
        int maxFd = 0;
        
        if (!writeClosed) {
            FD_SET(pipeStin[1], &writefds);
            maxFd = std::max(maxFd, pipeStin[1]);
        }
        
        FD_SET(pipeStout[0], &readfds);
        maxFd = std::max(maxFd, pipeStout[0]);
        
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        
        int activity = select(maxFd + 1, &readfds, &writefds, NULL, &timeout);
        
        if (activity < 0) {
            perror("select error");
            break;
        }
        if (activity == 0) {
            std::cerr << "DEBUG: select timeout" << std::endl;
            break;
        }
        
        if (!writeClosed && FD_ISSET(pipeStin[1], &writefds)) {
            handleCgiWrite(pipeStin[1], body, bytesWritten, writeClosed);
        }
        
        if (FD_ISSET(pipeStout[0], &readfds)) {
            if (!handleCgiRead(pipeStout[0], responseBody)) {
                break;
            }
        }
    }
    
    if (!writeClosed) close(pipeStin[1]);
    close(pipeStout[0]);
    
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            std::cerr << "CGI Execution Failed with status " << WEXITSTATUS(status) << std::endl;
    
    std::cout << "DEBUG: Finished reading CGI output, total size: " << responseBody.size() << std::endl;
    if (responseBody.size() > 0) {
        std::string preview = responseBody.substr(0, std::min<size_t>(responseBody.size(), 200));
        std::cout << "DEBUG: CGI Output Preview (first 200 bytes):\n" << preview << "\n[End Preview]" << std::endl;
    }
    return responseBody;
}
