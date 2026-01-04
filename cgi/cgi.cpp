#include "cgi.hpp"
#include "CgiCommunication.hpp"
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/select.h>
#include <algorithm>
#include <sstream>
#include <sys/wait.h>

extern char **environ;

static void handleChildProcess(int pipeStout[2], int pipeStin[2], 
                       const std::string& scriptPath, 
                       const std::string& interpreterPath,
                       std::map<std::string, std::string>& envVars) 
{
    std::cout << "DEBUG: Child process started" << std::endl;
    close(pipeStout[0]);
    close(pipeStin[1]);

    dup2(pipeStin[0], STDIN_FILENO);
    close(pipeStin[0]);

    dup2(pipeStout[1], STDOUT_FILENO);
    close(pipeStout[1]);

    for (std::map<std::string, std::string>::iterator it = envVars.begin(); it != envVars.end(); ++it)
    {
        setenv(it->first.c_str(), it->second.c_str(), 1);
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

static std::string formatCgiResponse(const std::string& responseBody) {
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
    return response.str();
}

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
        
        if (pid < 0) // Fork error
        {
            close(pipeStout[0]);
            close(pipeStout[1]);
            close(pipeStin[0]);
            close(pipeStin[1]);
            throw std::runtime_error("Fork Error");
        }

        if (pid == 0) // Child process
        {
            handleChildProcess(pipeStout, pipeStin, scriptPath, interpreterPath, envVars);
            _exit(0);
        }
        else // Parent process
        {
            std::string rawOutput = communicateWithCgi(pipeStout, pipeStin, pid, body);
            return formatCgiResponse(rawOutput);
        }
    }
    catch (const std::exception& e)
    {
        return "HTTP/1.1 500 Internal Server Error\r\n\r\n500 " + std::string(e.what());
    }
    return "";
}