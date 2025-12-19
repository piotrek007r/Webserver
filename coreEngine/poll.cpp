// Copyright [2025] <Piotr Ruszkiewicz> <pruszkie@student.42warsaw.pl>

#include "CoreEngine.hpp"

void CoreEngine::setConnection(size_t i)
{
   socklen_t addrLen = sizeof(sockaddr_storage);
   client client;
   memset(&client.clientSockaddr, 0, sizeof(sockaddr_in));
   client.serverCfg = serversCfg[i];
   client.hasError = false;
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

   isClientFD[pollFDsNum] = true;
   pollFDs[pollFDsNum].events = POLLIN;
   pollFDsNum++;
   this->clientVec.push_back(client);
   std::cout << "socket: " << pollFDs[i].fd << " ready to connect" << std::endl;
}

void CoreEngine::recivNClose(size_t el)
{
   client &client = this->getClientByFD(pollFDs[el].fd);
   gettimeofday(&client.lastAction, NULL);

   HttpError errorHandler;

   client.byteRecived = recv(pollFDs[el].fd, client.inputBuffer, sizeof(client.inputBuffer), 0);

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

   client.leftooverBuffer.append(client.inputBuffer, client.byteRecived);


   std::string &temp = client.leftooverBuffer;
   size_t begin = 0;

   while (true)
   {
      size_t end = temp.find("\r\n\r\n", begin);
      if (end == std::string::npos)
      {
         temp.erase(0, begin);
         break;
      }

      std::string headers = temp.substr(begin, end - begin + 4);

      if (headers.find("HTTP/") == std::string::npos)
      {
         std::cout << "Invalid HTTP headers detected!" << std::endl;
         client.hasError = true;
         client.sendOffset = 0;
         client.sendBuffer = errorHandler.generateErrorResponse(
            400, "The request does not contain valid HTTP headers."
         );
         pollFDs[el].events = POLLOUT;
         return;
      }

      size_t contentLength = 0;
      bool isChunked = false;
      bool needMoreData = false;
      
      // Check for Transfer-Encoding: chunked
      size_t tePos = headers.find("Transfer-Encoding:");
      if (tePos != std::string::npos)
      {
         tePos += 18;
         while (tePos < headers.size() && (headers[tePos] == ' ' || headers[tePos] == '\t')) tePos++;
         size_t teEnd = headers.find("\r\n", tePos);
         std::string teValue = headers.substr(tePos, teEnd - tePos);
         if (teValue.find("chunked") != std::string::npos)
         {
            isChunked = true;
            std::cout << CYAN << "DEBUG: Transfer-Encoding: chunked" << RESET << std::endl;
         }
      }
      
      // Check for Content-Length
      size_t clPos = headers.find("Content-Length:");
      if (clPos != std::string::npos)
      {
         clPos += 15;
         while (clPos < headers.size() && (headers[clPos] == ' ' || headers[clPos] == '\t')) clPos++;
         size_t clEnd = headers.find("\r\n", clPos);
         std::string clStr = headers.substr(clPos, clEnd - clPos);
         contentLength = static_cast<size_t>(std::strtoul(clStr.c_str(), NULL, 10));
         std::cout << CYAN << "DEBUG: Content-Length = " << contentLength << RESET << std::endl;
      }

      size_t fullRequestLen = 0;
      
      if (isChunked)
      {
         // Parse chunked encoding
         size_t bodyStart = end + 4;
         size_t pos = bodyStart;
         size_t totalBodySize = 0;
         
         while (pos < temp.size() && !needMoreData)
         {
            // Find chunk size line
            size_t chunkLineEnd = temp.find("\r\n", pos);
            if (chunkLineEnd == std::string::npos)
            {
               // Need more data
               std::cout << YELLOW << "DEBUG: Chunked - waiting for chunk size..." << RESET << std::endl;
               needMoreData = true;
               break;
            }
            
            // Parse chunk size (hex)
            std::string chunkSizeStr = temp.substr(pos, chunkLineEnd - pos);
            size_t chunkSize = std::strtoul(chunkSizeStr.c_str(), NULL, 16);
            
            if (chunkSize == 0)
            {
               // Last chunk - check for final \r\n
               if (chunkLineEnd + 4 > temp.size())
               {
                  std::cout << YELLOW << "DEBUG: Chunked - waiting for final CRLF..." << RESET << std::endl;
                  needMoreData = true;
                  break;
               }
               // Complete chunked request
               fullRequestLen = chunkLineEnd + 4 - begin;
               std::cout << GREEN << "DEBUG: Chunked complete, total body size = " << totalBodySize << RESET << std::endl;
               break;
            }
            
            // Check if we have the full chunk data
            size_t chunkDataStart = chunkLineEnd + 2;
            size_t chunkDataEnd = chunkDataStart + chunkSize;
            
            if (chunkDataEnd + 2 > temp.size())
            {
               // Need more data
               std::cout << YELLOW << "DEBUG: Chunked - waiting for chunk data (need " << chunkSize << " bytes)..." << RESET << std::endl;
               needMoreData = true;
               break;
            }
            
            totalBodySize += chunkSize;
            pos = chunkDataEnd + 2; // Skip chunk data and trailing \r\n
         }
         
         if (needMoreData)
         {
            // Don't erase anything - keep all data for next recv
            break; // Exit the main while loop and wait for more data
         }
      }
      else
      {
         fullRequestLen = (end - begin + 4) + contentLength;
      }
      
      std::cout << CYAN << "DEBUG: fullRequestLen = " << fullRequestLen 
                << ", available = " << (temp.size() - begin) << RESET << std::endl;

      if (temp.size() - begin < fullRequestLen)
      {
         std::cout << YELLOW << "DEBUG: Waiting for more data..." << RESET << std::endl;
         temp.erase(0, begin);
         break;
      }

      std::string request = temp.substr(begin, fullRequestLen);
      client.requestBufferVec.push_back(request);
      begin += fullRequestLen;
   }

   if (!client.requestBufferVec.empty())
   {
      client.sendBuffer.clear();
      client.sendOffset = 0;

      for (size_t i = 0; i < client.requestBufferVec.size(); i++)
         client.sendBuffer += prepareResponse(client, el, i);

      client.requestBufferVec.clear();
      pollFDs[el].events = POLLOUT;
   }
}

void CoreEngine::sendToClient(size_t el)
{
   client &client = this->getClientByFD(pollFDs[el].fd);
   std::cout << "actual client: " << client.FD << std::endl;
   std::cout << "send buffer: " << client.sendBuffer << std::endl;
   try
   {
      int byteSend = send(pollFDs[el].fd, client.sendBuffer.c_str() + client.sendOffset,
                          client.sendBuffer.size() - client.sendOffset, 0); // check if string functions are ok
      if (byteSend <= 0)
      {
         std::cerr << "send() failed: " << strerror(errno) << std::endl;
         HttpError errorHandler;
         std::string errorResponse = errorHandler.generateErrorResponse(500);
         closeCLient(el);
         return;
      }
      if ((0 < byteSend) && (byteSend < (int)client.sendBuffer.size()))
      {
         client.sendOffset += byteSend;
         return;
      }
      if (client.hasError == true)
      {
         closeCLient(el);
         return;
      }
      client.sendOffset = 0;
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
   pollFDs[el].events = POLLIN;
   // std::cout << "client: " << pollFDs[el].fd << " ready to send" << std::endl;
}
