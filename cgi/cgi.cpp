/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   cgi.cpp                                            :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: gbuczyns <gbuczyns@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/11/24 13:33:44 by agorski           #+#    #+#             */
/*   Updated: 2025/12/17 14:30:10 by gbuczyns         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "cgi.hpp"

std::string executeCgi(const std::string& scriptPath,
                              const std::string& interpreterPath,
                              const std::string& body, 
                              std::map <std::string, std::string> envVars)
{
    int pipeStout[2];
    int pipeStin[2];
    pid_t pid;

    try
    {
    if (pipe(pipeStout) == -1 || pipe(pipeStin) == -1)
        throw std::runtime_error("Pipe Error");

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

        char* args[3];
        args[0] = const_cast<char*>(execPath.c_str());
        args[1] = const_cast<char*>(scriptPath.c_str());
        args[2] = NULL;

        // std::cerr << "About to execve: " << execPath << " " << scriptPath << std::endl;
        if (execve(execPath.c_str(), args, environ) == -1)
        {
            std::cerr << "execve error: " << std::endl;
            _exit(127);
        }
    }
    else // Parent process
    {
        close(pipeStout[1]);
        close(pipeStin[0]);
        
        // write body to CGI stdin
        if (!body.empty())
            write(pipeStin[1], body.c_str(), body.size());
        close(pipeStin[1]);

        // read CGI output from stdout
        
        std::string body;
        char buffer[4096];
        ssize_t bytesRead;

        while ((bytesRead = read(pipeStout[0], buffer, sizeof(buffer))) > 0)
        {
            body.append(buffer, bytesRead);
        }

        close(pipeStout[0]);

        // wait for child process to finish
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            throw std::runtime_error("CGI Execution Failed");
        
        std::ostringstream response;
        if (!body.find("\r\n\r\n"))
            {
                int bodyLength = body.length() - body.find("\r\n\r\n") - 4;
                response    << "HTTP/1.1 200 OK\r\n"
                            << "Content-Length: " << bodyLength << "\r\n"
                            << body;        
            }
        else if (!body.empty())
        {
            response << "HTTP/1.1 200 OK\r\n"
                    << "Content-Length: " << body.length() << "\r\n"
                    << "Content-Type: text/html\r\n"
                    << "Content-Type: application/octet-stream\r\n"
                    << "Connection: close\r\n"
                    << "\r\n"
                    << body;
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