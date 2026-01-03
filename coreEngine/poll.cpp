// Copyright [2025] <Piotr Ruszkiewicz> <pruszkie@student.42warsaw.pl>

#include "CoreEngine.hpp"

#include <cctype>


void CoreEngine::failClient(size_t el, client &client, int code)
{
   HttpError errorHandler;
   client.hasError = true;
   client.sendOffset = 0;
   client.sendBuffer = errorHandler.generateErrorResponse(code);
   pollFDs[el].events = POLLOUT;
   // client.parser.reset();
}

void CoreEngine::failClient(size_t el, client &client, int code, const char *message)
{
   HttpError errorHandler;
   client.hasError = true;
   client.sendOffset = 0;
   client.sendBuffer = errorHandler.generateErrorResponse(code, message);
   pollFDs[el].events = POLLOUT;
   // client.parser.reset();
}

void CoreEngine::setConnection(size_t i)
{
   socklen_t addrLen = sizeof(sockaddr_storage);
   client client;
   memset(&client.clientSockaddr, 0, sizeof(sockaddr_in));
   client.serverCfg = serversCfg[i];
   client.http.setServerConfig(client.serverCfg);
   client.hasError = false;
   client.closeAfterSend = false;
   client.sendOffset = 0;
   gettimeofday(&client.lastAction, NULL);


   client.FD = accept(pollFDs[i].fd, (struct sockaddr *)&client.clientSockaddr, &addrLen);
   if (client.FD == -1)
   {
      std::cerr << "accept() failed: " << strerror(errno) << std::endl;
      exit(1);
   }
   pollfd pfd;

   pfd.fd = client.FD;
   pfd.revents = 0;
   pollFDs.push_back(pfd);

   isClientFD.push_back(true);
   pollFDs.back().events = POLLIN;
   pollFDsNum = pollFDs.size();
   this->clientVec.push_back(client);
   // std::cout << "socket: " << pollFDs[i].fd << " ready to connect (client FD: " << client.FD << ")" << std::endl;
}


void CoreEngine::recivNClose(size_t el)
{
   client &client = this->getClientByFD(pollFDs[el].fd);
   gettimeofday(&client.lastAction, NULL);

   HttpError errorHandler;
   client.byteRecived = recv(pollFDs[el].fd, client.inputBuffer, sizeof(client.inputBuffer), 0);

   if (client.byteRecived > 0) {
       // std::cout << "DEBUG: recv() read " << client.byteRecived << " bytes from fd " << pollFDs[el].fd << std::endl;
   }

   if (client.byteRecived < 0)
   {
      std::cerr << "recv() failed: " << strerror(errno) << std::endl;
      client.hasError = true;
      client.sendOffset = 0;
      client.sendBuffer = errorHandler.generateErrorResponse(500);
      pollFDs[el].events = POLLOUT;
      return;
   }
   else if (client.byteRecived == 0)
   {
      closeCLient(el);
      return;
   }

   ParseStatus::Enum status = client.http.consume(client.inputBuffer, client.byteRecived);
   
   if (status == ParseStatus::Error_TooLarge) {
       client.hasError = true;
       client.sendOffset = 0;
       client.sendBuffer = client.http.getResponse();
       pollFDs[el].events = POLLOUT;
       return;
   } else if (status == ParseStatus::Error_InvalidFormat) {
       failClient(el, client, 400);
       return;
   }

   if (status == ParseStatus::Done) {
       client.sendBuffer += client.http.getResponse();
       pollFDs[el].events = POLLOUT;
       if (client.http.shouldClose()) {
           client.closeAfterSend = true;
       }
       // TODO: Handle pipelining / reset Http
   }
}


void CoreEngine::sendToClient(size_t el)
{
   client &client = this->getClientByFD(pollFDs[el].fd);
   {
      std::string::size_type eol = client.sendBuffer.find("\r\n");
      std::string firstLine = (eol == std::string::npos) ? client.sendBuffer : client.sendBuffer.substr(0, eol);
      // std::cout << "actual client: " << client.FD
      //           << " | sending: " << client.sendBuffer.size() << " bytes"
      //           << " | " << firstLine << std::endl;
   }
   try
   {
      int toSend = client.sendBuffer.size() - client.sendOffset;
      int byteSend = send(pollFDs[el].fd, client.sendBuffer.c_str() + client.sendOffset, toSend, 0);

      if (byteSend <= 0)
      {
         std::cerr << "send() failed: " << strerror(errno) << std::endl;
         closeCLient(el);
         return;
      }

      client.sendOffset += byteSend;

      if (client.sendOffset < client.sendBuffer.size())
      {
         // Partial send, wait for next POLLOUT
         return;
      }

      // Full send completed
      if (client.hasError || client.closeAfterSend)
      {
         closeCLient(el);
         return;
      }

   }
   catch (const std::exception &e)
   {
      std::cerr << "Exception in sendToClient: " << e.what() << std::endl;
      HttpError errorHandler;
      std::string errorResponse = errorHandler.generateErrorResponse(500,
                                                                     "An internal error occurred while processing your request.");
      client.hasError = true;
      client.sendBuffer = errorResponse;
      return;
   }
   client.sendBuffer.clear();
   client.sendOffset = 0;
   client.http.reset();
   pollFDs[el].events = POLLIN;
   // std::cout << "client: " << pollFDs[el].fd << " ready to send" << std::endl;
}
