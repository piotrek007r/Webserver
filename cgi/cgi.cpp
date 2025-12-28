#include "cgi.hpp"
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/select.h>
#include <algorithm>

extern char **environ;

std::string executeCgi(const std::string& scriptPath,
                              const std::string& interpreterPath,
                              const std::string& body, 
                              std::map <std::string, std::string> envVars)
{
    std::cout << "DEBUG: executeCgi called with scriptPath: " << scriptPath << ", interpreterPath: " << interpreterPath << ", body size: " << body.size() << std::endl;
    int pipeStout[2];
    int pipeStin[2];
    pid_t pid;

    try
    {
    if (pipe(pipeStout) == -1 || pipe(pipeStin) == -1)
        throw std::runtime_error("Pipe Error");

    std::cout << "DEBUG: Pipes created" << std::endl;
    pid = fork();
    
    if (pid < 0)
        {
        close(pipeStout[0]);
        close(pipeStout[1]);
        close(pipeStin[0]);
        close(pipeStin[1]);
        throw std::runtime_error("Fork Error");
        }

    if (pid == 0) // Child process
    {
        std::cout << "DEBUG: Child process started" << std::endl;
        // close unused pipe ends
        close(pipeStout[0]);
        close(pipeStin[1]);

        // redirect stdin from pipe (body to CGI)
        dup2(pipeStin[0], STDIN_FILENO);
        close(pipeStin[0]);

        // redirect stdout to pipe (CGI output to parent)
        dup2(pipeStout[1], STDOUT_FILENO);
        close(pipeStout[1]);

        // set environment variables
        for (std::map<std::string, std::string>::iterator it = envVars.begin(); it != envVars.end(); ++it)
        {
            setenv(it->first.c_str(), it->second.c_str(), 1); //wedlug pawla nie moge
        }

        std::string execPath = interpreterPath;
        if (!execPath.empty() && execPath[execPath.size() - 1] == '/')
            execPath.erase(execPath.size() - 1);

        bool hasSlash = (execPath.find('/') != std::string::npos);
        if (!hasSlash)
        {
            std::string local = "./" + execPath;
            if (access(local.c_str(), X_OK) == 0)
                execPath = local;
        }

        char* args[3];
        args[0] = const_cast<char*>(execPath.c_str());
        args[1] = const_cast<char*>(scriptPath.c_str());
        args[2] = NULL;

        std::cerr << "DEBUG: About to exec: " << execPath << " " << scriptPath << std::endl;
        if (execPath.find('/') == std::string::npos)
        {
            execvp(execPath.c_str(), args);
            std::cerr << "execvp error" << std::endl;
            _exit(127);
        }
        if (execve(execPath.c_str(), args, environ) == -1)
        {
            std::cerr << "execve error: " << std::endl;
            _exit(127);
        }
    }
    else // Parent process
    {
        std::cout << "DEBUG: Parent process, child pid: " << pid << std::endl;
        close(pipeStout[1]);
        close(pipeStin[0]);
        
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
                size_t chunkSize = 65536; // 64KB
                size_t remaining = body.size() - bytesWritten;
                size_t toWrite = std::min(chunkSize, remaining);
                
                ssize_t written = write(pipeStin[1], body.c_str() + bytesWritten, toWrite);
                if (written > 0) {
                    bytesWritten += written;
                } else if (written == -1) {
                    perror("write error");
                    writeClosed = true;
                    close(pipeStin[1]);
                }
                
                if (bytesWritten == body.size()) {
                    close(pipeStin[1]);
                    writeClosed = true;
                    std::cout << "DEBUG: Finished writing body, closed pipeStin[1]" << std::endl;
                }
            }
            
            if (FD_ISSET(pipeStout[0], &readfds)) {
                char buffer[4096];
                ssize_t bytesRead = read(pipeStout[0], buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    responseBody.append(buffer, bytesRead);
                } else if (bytesRead == 0) {
                    std::cout << "DEBUG: EOF from child" << std::endl;
                    break;
                } else {
                    perror("read error");
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

        std::ostringstream response;
        if (responseBody.find("HTTP/1.1") != std::string::npos)
        {
            response << responseBody;
        }
        else if (responseBody.find("\r\n\r\n") != std::string::npos)
        {
            int bodyLength = responseBody.length() - responseBody.find("\r\n\r\n") - 4;
            response    << "HTTP/1.1 200 OK\r\n"
                        << "Connection: close\r\n"
                        << "Content-Length: " << bodyLength << "\r\n"
                        << responseBody;        
        }
        else if (!responseBody.empty())
        {
            response << "HTTP/1.1 200 OK\r\n"
                    << "Content-Length: " << responseBody.length() << "\r\n"
                    << "Content-Type: text/html\r\n"
                    << "Content-Type: application/octet-stream\r\n"
                    << "Connection: close\r\n"
                    << "\r\n"
                    << responseBody;
        }
        else
        {
            response << "HTTP/1.1 204 No Content\r\n"
                    << "Connection: close\r\n"
                    << "\r\n";
        }
        return (response.str());
    }
}
    catch (const std::exception& e)
    {
        // error handling
        return "HTTP/1.1 500 Internal Server Error\r\n\r\n500 " + std::string(e.what());
    }
    return "";
}