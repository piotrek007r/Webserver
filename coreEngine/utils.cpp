#include "CoreEngine.hpp"

CoreEngine::client &CoreEngine::getClientByFD(int socketFD)
{
    for (size_t i = 0; i < this->clientVec.size(); i++)
        if (clientVec[i].FD == socketFD)
            return clientVec[i];
    std::stringstream ss;
    ss << "Client not found for socket FD: " << socketFD;
    throw std::runtime_error(ss.str());
}

void CoreEngine::closeCLient(int el)
{
    // std::cout << "DEBUG: Closing client FD: " << pollFDs[el].fd << std::endl;
    for (size_t i = 0; i < clientVec.size(); i++)
        if (clientVec[i].FD == pollFDs[el].fd)
        {
            clientVec.erase(clientVec.begin() + i);
            break;
        }
    close(pollFDs[el].fd);
    pollFDs.erase(pollFDs.begin() + el);
    isClientFD.erase(isClientFD.begin() + el);
    pollFDsNum--;
}

std::string CoreEngine::prepareResponse(client &client, size_t el, std::string rawRequest)
{
    static unsigned long long requestCounter = 0;
    requestCounter++;
    if (requestCounter % 500 == 0) {
        std::cout << MAGENTA << ">>> Total requests processed: " << requestCounter << " <<<" << RESET << std::endl;
    }

    {
        std::string::size_type eol = rawRequest.find("\r\n");
        std::string firstLine = (eol == std::string::npos) ? rawRequest : rawRequest.substr(0, eol);
        std::cout << YELLOW << "--> Request: " << firstLine << " (" << rawRequest.size() << " bytes)" << RESET << std::endl;
    }
    Http object(rawRequest, client.serverCfg);
    if (object.getIsError())
        client.hasError = true;
    pollFDs[el].events = POLLOUT;
    {
        const std::string &resp = object.getResponse();
        std::string::size_type eol = resp.find("\r\n");
        std::string firstLine = (eol == std::string::npos) ? resp : resp.substr(0, eol);
        // std::cout << ORANGE << "--> Response: " << firstLine << " (" << resp.size() << " bytes)" << RESET << std::endl;
    }
    if (object.shouldClose() || object.getResponse().find("Connection: close") != std::string::npos)
        client.closeAfterSend = true;
    return object.getResponse();
}

long CoreEngine::getTimeStamp(timeval countFrom, timeval compareTime)
{
    long start;
    long current;
    long timeElapsed;

    start = (countFrom.tv_sec * 1000000) + countFrom.tv_usec;
    current = (compareTime.tv_sec * 1000000) + compareTime.tv_usec;
    timeElapsed = current - start;

    return timeElapsed;
}