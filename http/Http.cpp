#include "Http.hpp"
#include "../errors/error.hpp"
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <regex.h>
#include <algorithm>

static bool extractBoundary(const std::string& contentType, std::string& boundaryOut);
static bool parseMultipartFilePart(const std::string& body,
                                   const std::string& boundary,
                                   MultipartFile& out,
                                   const std::string& fieldName);

static bool regexMatch(const std::string &pattern, const std::string &text)
{
    regex_t re;
    if (regcomp(&re, pattern.c_str(), REG_EXTENDED) != 0)
        return false;
    int rc = regexec(&re, text.c_str(), 0, NULL, 0);
    regfree(&re);
    return rc == 0;
}

static void applyLocationOverride(LocationConfig &base, const LocationConfig &over)
{
    if (!over.client_max_body_size.empty()) base.client_max_body_size = over.client_max_body_size;
    if (!over.index.empty()) base.index = over.index;
    if (!over.upload_dir.empty()) base.upload_dir = over.upload_dir;
    if (!over.scripts_path.empty()) base.scripts_path = over.scripts_path;
    if (!over.root.empty()) base.root = over.root;
    if (!over.allow_methods.empty()) base.allow_methods = over.allow_methods;
    for (std::map<std::string, std::string>::const_iterator it = over.interpreters.begin(); it != over.interpreters.end(); ++it)
        base.interpreters[it->first] = it->second;
    if (!over.error_pages.empty()) base.error_pages = over.error_pages;
    if (over.has_redirect) { base.has_redirect = true; base.redirect_code = over.redirect_code; base.redirect_url = over.redirect_url; }
}

// Constructor
Http::Http (std::string &rawRequest, ServerConfig serverData){

    // Initialize primitive types
    _rawRequestPtr = NULL;
    _bodyStart = 0;
    _bodyLen = 0;
    

    //std::cout << "Debug: Entering Http constructor" << std::endl;

    _rawRequestPtr = &rawRequest;
    _serverData = serverData;
    
    requestBilder(rawRequest);

    // Get location-specific configuration
    // _myConfig = 0;
    _myConfig = getMyConfig();
        size_t queryPos = _s_requestData._request.find('?');
    if (queryPos != std::string::npos) {
        _s_requestData._queryString =  _s_requestData._request.substr(queryPos + 1);
        _s_requestData._fileName = _s_requestData._request.substr(0, queryPos);
    }
    else {
        _s_requestData._fileName = _s_requestData._request;
    }
    while (_s_requestData._fileName[0] == '/')
    _s_requestData._fileName = _s_requestData._fileName.substr(1, _s_requestData._fileName.length() - 1); // Remove leading '/'
    
    // Debug - sprawdź wartości po getMyConfig()
    // std::cout << YELLOW << "DEBUG _myConfig.has_redirect: " << _myConfig.has_redirect << RESET << std::endl;
    // std::cout << YELLOW << "DEBUG _myConfig.redirect_url: '" << _myConfig.redirect_url << "'" << RESET << std::endl;
    // std::cout << YELLOW << "DEBUG _myConfig.redirect_code: " << _myConfig.redirect_code << RESET << std::endl;
    
    // Initialize HttpError with server or location-specific error pages
    _httpError = HttpError(_myConfig.error_pages);

    if (_s_requestData._headers["Transfer-Encoding"] == "chunked") {
        if (!isBodySizeAllowed()) {
            _s_responseData._hasError = true;
            _s_responseData._responseStatusCode = 413;
            _s_responseData._response = _httpError.generateErrorResponse(413);
            return;
        }
    }

    responseBuilder();
    //testResponseBuilder();
    // std::cout << GREEN << "Debug: Exiting Http constructor" << std::endl;
    // std::cout << _s_responseData._response << RESET << std::endl;
}
void Http::chankuj(std::string& rawRequest) 
 {
    std::string unchunkedBody;
    size_t pos = _bodyStart;

    while (pos < rawRequest.size()) {
        size_t lineEnd = rawRequest.find("\r\n", pos);
        if (lineEnd == std::string::npos)
            break;

        std::string sizeStr = rawRequest.substr(pos, lineEnd - pos);
        size_t chunkSize = std::strtoul(sizeStr.c_str(), NULL, 16);
        if (chunkSize == 0)
            break; // Last chunk

        pos = lineEnd + 2; // Move past \r\n
        // Ensure there is enough data remaining for this chunk
        if (pos > rawRequest.size())
            break;
        if (chunkSize > rawRequest.size() - pos)
            break;

        unchunkedBody.append(rawRequest, pos, chunkSize);

        // Move past chunk and trailing \r\n, ensuring we stay in bounds
        if (chunkSize + 2 > rawRequest.size() - pos)
            break;
        pos += chunkSize + 2;
        if (chunkSize == 0) break; // Last chunk
    }
    // Replace the original body with the unchunked body
    rawRequest.replace(_bodyStart, _bodyLen, unchunkedBody);
    _bodyLen = unchunkedBody.size();
}

void Http::requestBilder(std::string &rawRequest) 
{
    parseRequest(rawRequest);
    parseHeader();
    if (_s_requestData._headers["Transfer-Encoding"] == "chunked")
        chankuj(rawRequest);
}

// Parse HTTP request
void Http::parseRequest(std::string &rawRequest) {
    size_t headerEnd = rawRequest.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        _s_requestData._rawHeader = rawRequest.substr(0, headerEnd);
        _rawRequestPtr = &rawRequest;
        _bodyStart = headerEnd + 4;
        _bodyLen = rawRequest.length() - (headerEnd + 4);
    } else {
        _s_requestData._rawHeader = rawRequest;
        // Debug output
        std::cout << RED << "=== NO \\r\\n\\r\\n FOUND, RAW HEADER ===\n"
                    << _s_requestData._rawHeader
                    << "\n=== END ===" << std::endl;
        std::cout << "=== RAW REQUEST ===\n" 
                    << rawRequest
                    << "\n=== END RAW REQUEST ===" << RESET << std::endl;
        _rawRequestPtr = NULL;
        _bodyStart = 0;
        _bodyLen = 0;
    }

}

// Parse HTTP headers
void Http::parseHeader() {
    std::istringstream stream(_s_requestData._rawHeader);
    std::string line;

    if (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        std::string method, path, version;
        
        lineStream >> method >> path >> version;
    
        if (!version.empty() && version[version.length() - 1] == '\r') {
            version.erase(version.length() - 1);
        }
        
        _s_requestData._method = method;
        _s_requestData._uri = path;
        _s_requestData._httpVersion = version;
    }

    // Parse headers
    // std::cout << CYAN << "=== INCOMING REQUEST HEADERS ===" << RESET << std::endl;
    while (std::getline(stream, line)) {
        if (line == "\r" || line.empty()) {
            break; // End of headers
        }
        if (!line.empty() && line[line.length() - 1] == '\r') {
            line = line.substr(0, line.length() - 1); // Remove trailing \r
        }
        size_t delimiterPos = line.find(": ");
        if (delimiterPos != std::string::npos) {
            std::string key = line.substr(0, delimiterPos);
            std::string value = line.substr(delimiterPos + 2);
            _s_requestData._headers[key] = value;
            // std::cout << CYAN << key << ": " << value << RESET << std::endl;
        }
    }
    // std::cout << CYAN << "=== END HEADERS ===" << RESET << std::endl;
}

bool Http::isBodySizeAllowed() {
    if (_myConfig.client_max_body_size.empty()) {
        return true;
    }
    
    size_t maxBodySize = static_cast<size_t>(std::atoi(_myConfig.client_max_body_size.c_str()));
    std::map<std::string, std::string>::iterator it = _s_requestData._headers.find("Content-Length");
    
    if (it != _s_requestData._headers.end()) {
        size_t contentLength = static_cast<size_t>(std::atoi(it->second.c_str()));
        if (contentLength > maxBodySize) {
            return false;
        }
    } else {
        // For chunked encoding or when Content-Length is missing, check the actual body size
        if (_bodyLen > maxBodySize) {
            return false;
        }
    }
    return true;
}

std::string Http::getCGIinterpreter(const std::string& fileName) {
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    std::string extension = fileName.substr(dotPos);
    std::map<std::string, std::string>::iterator it = _myConfig.interpreters.find(extension);
    if (it != _myConfig.interpreters.end()) {
        return it->second;
    }
    return "";
}

// Build CGI response
void Http::cgiResponseBuilder() {
    std::cout << "DEBUG: Starting CGI response build for " << _s_requestData._fileName << std::endl;

    std::string scriptPath = _myConfig.scripts_path +_s_requestData._fileName;
    std::string cgiInterpreter = getCGIinterpreter(_s_requestData._fileName);
    // if (!resourceExists(scriptPath)) {
    //     sendError(404);
    //     return;
    // }
    
    std::string body;
    if (_rawRequestPtr != NULL && _bodyLen > 0) {
        body = _rawRequestPtr->substr(_bodyStart, _bodyLen);
    }
    
    std::map<std::string, std::string> envVars;
    
    envVars["REQUEST_METHOD"] = _s_requestData._method;
    envVars["SCRIPT_FILENAME"] = scriptPath;
    envVars["QUERY_STRING"] = _s_requestData._queryString;
    envVars["SERVER_PROTOCOL"] = _s_requestData._httpVersion;
    envVars["REQUEST_URI"] = _s_requestData._uri;
    envVars["PATH_INFO"] = _s_requestData._uri;
    envVars["SCRIPT_NAME"] = _s_requestData._uri;
    envVars["PATH_TRANSLATED"] = scriptPath;
    
    if (!body.empty()) {
        std::ostringstream oss;
        oss << body.length();
        envVars["CONTENT_LENGTH"] = oss.str();
    }
    
    std::map<std::string, std::string>::iterator it = 
        _s_requestData._headers.find("Content-Type");
    if (it != _s_requestData._headers.end()) {
        envVars["CONTENT_TYPE"] = it->second;
    }
    
    for (std::map<std::string, std::string>::iterator it = _s_requestData._headers.begin();
         it != _s_requestData._headers.end(); ++it)
         {
        std::string key = "HTTP_" + it->first;
        for (size_t i = 0; i < key.length(); ++i) {
            if (key[i] == '-') key[i] = '_';
            key[i] = std::toupper(key[i]);
        }
        envVars[key] = it->second;
        }
    
    std::string response = executeCgi(scriptPath, cgiInterpreter, body, envVars);
    
    _s_responseData._response = response;
    _s_responseData._responseStatusCode = 200;  
}

// Build GET response
void Http::getResponseBuilder() {
    if (_myConfig.has_redirect) {
        sendRedirect(_myConfig.redirect_url, _myConfig.redirect_code);
        return;
    }

    std::string fullPath = _myConfig.root + _s_requestData._fileName;
    
    // std::cout << "Handling GET for filePath: " << _s_requestData._fileName << std::endl;
    if (resourceExists(fullPath)) {
        if (isDirectory(fullPath) && _s_requestData._fileName.empty() && _myConfig.index.empty()) {
            // std::cout << "Serving directory: " << _s_requestData._fileName << std::endl;
            generateDirectoryListing(fullPath);
        } else if (isFile(fullPath)) {
            // std::cout << "Serving file: " << _s_requestData._fileName << std::endl;
            handleFile(fullPath);
        } else {
            // std::cout << "Serving static file: " << _s_requestData._fileName << std::endl;
            if (fullPath[fullPath.length() - 1] != '/')
                fullPath += "/";
            handleFile(fullPath + _myConfig.index);
        }
    } else {
        std::cout << "File not found: " << _s_requestData._fileName << std::endl;
        sendError(404);
        return;
    }
}

bool Http::isFile(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
    return S_ISREG(buffer.st_mode);
}

bool Http::isDirectory(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
    bool isDir = S_ISDIR(buffer.st_mode);
    return isDir;
}

void Http::sendError(int errorCode) {
    _s_responseData._hasError = true;
    _s_responseData._responseStatusCode = errorCode;
    _s_responseData._response = _httpError.generateErrorResponse(errorCode);
}



bool Http::resourceExists(const std::string& path) {
    bool exists = access(path.c_str(), F_OK) == 0;
    return exists;
}

void Http::sendRedirect(const std::string& newLocation, int code) {
    if (code == 0) {
        code = 301;
    }
    
    _s_responseData._responseStatusCode = code;
    std::ostringstream response;
    
    std::string statusText;
    switch (code) {
        case 301: statusText = "Moved Permanently"; break;
        case 302: statusText = "Found"; break;
        case 303: statusText = "See Other"; break;
        case 307: statusText = "Temporary Redirect"; break;
        case 308: statusText = "Permanent Redirect"; break;
        default: statusText = "Moved Permanently"; code = 301; break;
    }
    
    response << "HTTP/1.1 " << code << " " << statusText << "\r\n"
             << "Location: " << newLocation << "\r\n"
             << "Connection: close\r\n"
             << "\r\n";
    
    _s_responseData._response = response.str();
}

std::string Http::determineMimeType(const std::string& path) {
    size_t dotPos = path.rfind('.');
    if (dotPos == std::string::npos) {
        return "application/octet-stream";
    }
    
    std::string extension = path.substr(dotPos);
    
    if (extension == ".html" || extension == ".htm")
        return "text/html";
    if (extension == ".css")
        return "text/css";
    if (extension == ".js")
        return "application/javascript";
    if (extension == ".png")
        return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".gif")
        return "image/gif";
    if (extension == ".txt")
        return "text/plain";
    
    return "application/octet-stream";
}

void Http::handleFile(const std::string& filePath) {
    int fd = open(filePath.c_str(), O_RDONLY);
    
    if (fd == -1) {
        sendError(404);
        return;
    }
    
    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1) {
        close(fd);
        sendError(500);
        return;
    }
    
    char* buffer = new char[fileStat.st_size + 1];
    ssize_t bytesRead = read(fd, buffer, fileStat.st_size);
    close(fd);
    
    if (bytesRead == -1) {
        delete[] buffer;
        sendError(500);
        return;
    }
    
    buffer[bytesRead] = '\0';
    std::string body(buffer, bytesRead);
    delete[] buffer;
    
    std::string mimeType = determineMimeType(filePath);
    
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: " << mimeType << "\r\n"
             << "Content-Length: " << body.length() << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << body;
    
    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 200;
}

void Http::generateDirectoryListing(const std::string& dirPath) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) {
        sendError(500);
        return;
    }
    
    std::ostringstream html;
    html << "<html>\n<head>\n<title>Index of " << _s_requestData._uri << "</title>\n</head>\n"
         << "<body>\n<h1>Index of " << _s_requestData._uri << "</h1>\n<hr>\n<ul>\n";
    
    if (_s_requestData._uri != "/") {
        html << "<li><a href=\"..\">../</a></li>\n";
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        
        if (name == "." || name == "..") {
            continue;
        }
        
        std::string fullPath = dirPath + "/" + name;
        
        if (isDirectory(fullPath)) {
            html << "<li><a href=\"" << name << "/\">" << name << "/</a></li>\n";
        } else {
            html << "<li><a href=\"" << name << "\">" << name << "</a></li>\n";
        }
    }
    
    closedir(dir);
    
    html << "</ul>\n<hr>\n</body>\n</html>";
    
    std::string body = html.str();
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html\r\n"
             << "Content-Length: " << body.length() << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << body;
    
    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 200;
}

void Http::handleRootPath() {
    if (!_myConfig.index.empty()) {
        std::string indexPath = _myConfig.root + "/" + _myConfig.index;

    if (!indexPath.empty() && indexPath[0] == '/')
        indexPath = indexPath.substr(1);
        
    if (resourceExists(indexPath)) {
        handleFile(indexPath);
        return;
    }
    }
    if (_myConfig.autoindex) {
        generateDirectoryListing(_myConfig.root);
    } else {
        sendError(403);
    }
}


void Http::putResponseBuilder() {
    if (_rawRequestPtr == NULL) { sendError(400); return; }

    std::string reqBody = _rawRequestPtr->substr(_bodyStart, _bodyLen);

    if (_myConfig.upload_dir.empty()) {
        std::ostringstream html;
        html << "<html><body>"
             << "<h1>PUT Data Received</h1>"
             << "<p>Content-Length: " << reqBody.size() << " bytes</p>"
             << "<pre>" << reqBody << "</pre>"
             << "</body></html>";
        const std::string respBody = html.str();

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: text/html\r\n"
                 << "Content-Length: " << respBody.size() << "\r\n"
                 << "Connection: keep-alive\r\n"
                 << "\r\n"
                 << respBody;

        _s_responseData._response = response.str();
        _s_responseData._responseStatusCode = 200;
        return;
    }

    std::string contentType = "";
    std::map<std::string, std::string>::iterator it = _s_requestData._headers.find("Content-Type");
    if (it != _s_requestData._headers.end()) contentType = it->second;

    std::string boundary;
    if (contentType.find("multipart/form-data") == std::string::npos || !extractBoundary(contentType, boundary)) {
        sendError(400);
        return;
    }

    MultipartFile mf;

    if (!parseMultipartFilePart(reqBody, boundary, mf, "note")) {
        if (!parseMultipartFilePart(reqBody, boundary, mf, "")) {
            sendError(400);
            return;
        }
    }

    std::string fullPath = _myConfig.upload_dir + "/" + mf.filename;

    int code;
    int fd = 0;     // = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fd = open(fullPath.c_str(), O_WRONLY | O_TRUNC, 0644);
    if (fd > 2) {
        code = 200; // updated existing file
        }
    else {
        fd = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        code = 201; // created new file
    }
    if (fd == -1) {
        sendError(500);
        return;
    }

    ssize_t written = write(fd, mf.data.data(), mf.data.size());
    close(fd);

    if (written == -1 || static_cast<size_t>(written) != mf.data.size()) {
        std::remove(fullPath.c_str());
        sendError(500);
        return;
    }

    std::ostringstream html;
    html << "<html><body>"
         << "<h1>File Uploaded</h1>"
         << "<p>Saved as: " << mf.filename << "</p>"
         << "<p>Bytes: " << mf.data.size() << "</p>"
         << "</body></html>";
    const std::string respBody = html.str();

    std::ostringstream response;
    response << "HTTP/1.1 " << code << (code == 201 ? " Created" : " OK") << "\r\n"
             << "Content-Type: text/html\r\n"
             << "Content-Length: " << respBody.size() << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << respBody;

    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = code;
 
}


// Build POST response
void Http::postResponseBuilder() {
    if (_rawRequestPtr == NULL) { sendError(400); return; }

    std::string reqBody = _rawRequestPtr->substr(_bodyStart, _bodyLen);

    if (_myConfig.upload_dir.empty()) {
        std::ostringstream html;
        html << "<html><body>"
             << "<h1>POST Data Received</h1>"
             << "<p>Content-Length: " << reqBody.size() << " bytes</p>"
             << "<pre>" << reqBody << "</pre>"
             << "</body></html>";
        const std::string respBody = html.str();

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: text/html\r\n"
                 << "Content-Length: " << respBody.size() << "\r\n"
                 << "Connection: keep-alive\r\n"
                 << "\r\n"
                 << respBody;

        _s_responseData._response = response.str();
        _s_responseData._responseStatusCode = 200;
        return;
    }

    std::string contentType = "";
    std::map<std::string, std::string>::iterator it = _s_requestData._headers.find("Content-Type");
    if (it != _s_requestData._headers.end()) contentType = it->second;

    std::string boundary;
    if (contentType.find("multipart/form-data") == std::string::npos || !extractBoundary(contentType, boundary)) {
        sendError(400);
        return;
    }

    MultipartFile mf;

    if (!parseMultipartFilePart(reqBody, boundary, mf, "file")) {
        if (!parseMultipartFilePart(reqBody, boundary, mf, "")) {
            sendError(400);
            return;
        }
    }

    std::string fullPath = _myConfig.upload_dir + mf.filename;

    {
        int probe = open(fullPath.c_str(), O_RDONLY);
        if (probe != -1) {
            close(probe);
            std::ostringstream oss;
            oss << _myConfig.upload_dir << "/" << std::time(NULL) << "_" << mf.filename;
            fullPath = oss.str();
        }
    }

    int fd = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { sendError(500); return; }

    ssize_t written = write(fd, mf.data.data(), mf.data.size());
    close(fd);

    if (written == -1 || static_cast<size_t>(written) != mf.data.size()) {
        std::remove(fullPath.c_str());
        sendError(500);
        return;
    }

    std::ostringstream html;
    html << "<html><body>"
         << "<h1>File Uploaded</h1>"
         << "<p>Saved as: " << mf.filename << "</p>"
         << "<p>Bytes: " << mf.data.size() << "</p>"
         << "</body></html>";
    const std::string respBody = html.str();

    std::ostringstream response;
    response << "HTTP/1.1 201 Created\r\n"
             << "Content-Type: text/html\r\n"
             << "Content-Length: " << respBody.size() << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << respBody;

    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 201;
}

// Build DELETE response
void Http::deleteResponseBuilder() {
    std::string fullPath = _myConfig.upload_dir + _s_requestData._fileName;
 
    std::cout << YELLOW << "DELETE fullPath: '" << fullPath << "'" << RESET << std::endl;
    
    if (!resourceExists(fullPath) || fullPath == _myConfig.upload_dir) {
        std::cout << RED << "DELETE 404: file not found" << RESET << std::endl;
        sendError(404);
        return;
    }
    
    if (isDirectory(fullPath)) {
        deleteDirectory(fullPath);
    } else {
        deleteFile(fullPath);
    }
}

void Http::deleteFile(const std::string& filePath) {

    if (filePath.find("..") != std::string::npos) {
        sendError(403); 
        return;
    }
    
    if (filePath.find(_myConfig.upload_dir) != 0) {
        sendError(403); 
        return;
    }

    if (access(filePath.c_str(), W_OK) != 0) {
        sendError(403); 
        return;
    }
    
    if (std::remove(filePath.c_str()) != 0) {
        sendError(500);
        return;
    }
    
    std::ostringstream response;
    response << "HTTP/1.1 204 No Content\r\n"
             << "Access-Control-Allow-Origin: *\r\n";
    
    for (size_t i = 0; i < _myConfig.allow_methods.size(); ++i) {
        if (i != 0) response << ", ";
        response << _myConfig.allow_methods[i];
    }
    response << "\r\n"
             << "Connection: close\r\n"
             << "\r\n";
    
    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 204;
}

void Http::deleteDirectory(const std::string& dirPath) {
    std::string urlPath = _s_requestData._uri;
    if (urlPath[urlPath.length() - 1] != '/') {
        sendError(409); 
        return;
    }
    
    if (access(dirPath.c_str(), W_OK) != 0) {
        sendError(403);
        return;
    }
    
    if (rmdir(dirPath.c_str()) != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST) {
            sendError(409); 
        } else {
            sendError(500);
        }
        return;
    }
    
    std::ostringstream response;
    response << "HTTP/1.1 204 No Content\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, POST, DELETE\r\n"
             << "Connection: close\r\n"
             << "\r\n";
    
    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 204;
}

bool Http::isCGI() {
    if (_myConfig.interpreters.empty()) {
        return false;
    }
    
    size_t dotPos = _s_requestData._uri.rfind('.');
    if (dotPos == std::string::npos) {
        return false;
    }
    
    std::string extension = _s_requestData._uri.substr(dotPos);
    if (extension.empty()) {
        return false;
    }
    if (_myConfig.interpreters.find(extension) == _myConfig.interpreters.end()) {
        return false;
    }
    
    return (true);
}

bool Http::isMethodAllowed() {
    for (size_t i = 0; i < _myConfig.allow_methods.size(); ++i) {
        if (_s_requestData._method == _myConfig.allow_methods[i]) {
            return true;
        }
    }
    return false;
}

void Http::responseBuilder() {

    if (_s_requestData._uri == "/favicon.ico") {
        _s_responseData._response = "HTTP/1.1 204 No Content\r\n"
                                   "Connection: close\r\n"
                                   "\r\n";
        return;
    }

    if (!isBodySizeAllowed()) {
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 413;
        _s_responseData._response = _httpError.generateErrorResponse(413);
        return;
    }

    if (isCGI()) {
        cgiResponseBuilder();
        return;
    }
   if (!isMethodAllowed()) {
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 405;
        _s_responseData._response = _httpError.generateErrorResponse(405);
        return;
    }
    
    // Obsłuż według metody
    if (_s_requestData._method == "GET") {
        getResponseBuilder();
    } else if (_s_requestData._method == "POST") {
        postResponseBuilder();
    } else if (_s_requestData._method == "DELETE") {
        deleteResponseBuilder();
    } else if (_s_requestData._method == "PUT") {
        putResponseBuilder();
    } else {
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 501;
        _s_responseData._response = _httpError.generateErrorResponse(501);
    }
}

LocationConfig Http::getMyConfig() {
    LocationConfig myConfig;
    
    myConfig.location_path = "";
    myConfig.root = "";
    myConfig.index = "";
    myConfig.autoindex = false;
    myConfig.client_max_body_size = "";
    myConfig.upload_dir = "";
    myConfig.has_redirect = false;
    myConfig.redirect_url = "";
    myConfig.redirect_code = 0;
    myConfig.allow_methods.clear();
    myConfig.error_pages.clear();
    
    bool baseFound = false;
    std::string baseKey;
    LocationConfig baseLoc;
    bool regexFound = false;
    LocationConfig regexLoc;

    for (std::map<std::string, LocationConfig>::iterator it = _serverData.locations.begin();
         it != _serverData.locations.end(); ++it) {
        const LocationConfig &loc = it->second;

        if (loc.is_regex) {
            if (!loc.regex_pattern.empty() && regexMatch(loc.regex_pattern, _s_requestData._uri)) {
                if (!regexFound) { regexFound = true; regexLoc = loc; }
            }
            continue;
        }

        if (_s_requestData._uri.find(it->first) == 0) {
            if (!baseFound || it->first.length() > baseKey.length()) {
                baseFound = true;
                baseKey = it->first;
                baseLoc = loc;
            }
        }
    }

    if (!baseFound) {
        myConfig.root = _serverData.root;
        myConfig.allow_methods = _serverData.allow_methods;
        myConfig.index = _serverData.index;
        myConfig.client_max_body_size = _serverData.client_max_body_size;
        myConfig.autoindex = _serverData.autoindex;
        myConfig.upload_dir = _serverData.upload_dir;
        myConfig.interpreters = _serverData.interpreters;
        myConfig.scripts_path = _serverData.scripts_path;
        myConfig.error_pages = _serverData.error_pages;
        _s_requestData._request = _s_requestData._uri;
    } else {
        myConfig = Config::getMergedLocationConfig(_serverData, baseLoc);
        _s_requestData._request = _s_requestData._uri.substr(baseKey.length());
    }

    if (regexFound)
        applyLocationOverride(myConfig, regexLoc);

    if (!myConfig.location_path.empty() && myConfig.location_path[myConfig.location_path.length() - 1] == '/')
        myConfig.location_path.erase(myConfig.location_path.length() - 1);

    if (!myConfig.location_path.empty() && myConfig.location_path[0] != '/' && !myConfig.is_regex)
        myConfig.location_path = "/" + myConfig.location_path;

    if (myConfig.location_path.empty() && baseFound)
        myConfig.location_path = baseKey;

    if (myConfig.location_path.empty() && !baseFound)
        myConfig.location_path = "";
    return myConfig;
}

bool Http::getIsError() const { return _s_responseData._hasError; }

int Http::getStatusCode() const { return _s_responseData._responseStatusCode; }

std::string Http::getResponse() const { return _s_responseData._response; }

bool Http::shouldClose() const { return _s_requestData._headers.count("Connection") && _s_requestData._headers.at("Connection") == "close"; }

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

static std::string sanitizeFilename(std::string name) {
    size_t p = name.find_last_of("/\\");
    if (p != std::string::npos) name = name.substr(p + 1);


    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-' ) out.push_back(c);
        else out.push_back('_');
    }

    if (out == "." || out == "..") return "upload.bin";
    return out;
}

static bool extractBoundary(const std::string& contentType, std::string& boundaryOut) {
    // Content-Type: multipart/form-data; boundary=----WebKitFormBoundary...
    std::string ct = contentType;
    size_t bpos = ct.find("boundary=");
    if (bpos == std::string::npos) return false;
    bpos += 9;

    std::string b = ct.substr(bpos);
    b = trim(b);
    if (!b.empty() && b[0] == '"') {
        size_t q = b.find('"', 1);
        if (q == std::string::npos) return false;
        b = b.substr(1, q - 1);
    } else {
        // utnij po ';'
        size_t sc = b.find(';');
        if (sc != std::string::npos) b = b.substr(0, sc);
        b = trim(b);
    }

    if (b.empty()) return false;
    boundaryOut = b;
    return true;
}

static bool parseMultipartFilePart(const std::string& body,
                                   const std::string& boundary,
                                   MultipartFile& out,
                                   const std::string& fieldName) {
    const std::string b = "--" + boundary;
    size_t pos = 0;

    while (true) {
        size_t start = body.find(b, pos);
        if (start == std::string::npos) return false;
        start += b.size();

        if (start + 2 <= body.size() && body.compare(start, 2, "--") == 0) return false;
        if (start + 2 <= body.size() && body.compare(start, 2, "\r\n") == 0) start += 2;

        size_t headerEnd = body.find("\r\n\r\n", start);
        if (headerEnd == std::string::npos) return false;

        std::string partHeaders = body.substr(start, headerEnd - start);
        size_t dataStart = headerEnd + 4;

        size_t next = body.find("\r\n" + b, dataStart);
        if (next == std::string::npos) return false;

        if (partHeaders.find("Content-Disposition:") != std::string::npos) {
            if (!fieldName.empty()) {
                std::string needle = "name=\"" + fieldName + "\"";
                if (partHeaders.find(needle) == std::string::npos) {
                    pos = next + 2;
                    continue;
                }
            }

            size_t fn = partHeaders.find("filename=");
            if (fn == std::string::npos) {
                pos = next + 2;
                continue;
            }

            std::string filename;
            fn += 9;
            if (fn < partHeaders.size() && partHeaders[fn] == '"') {
                size_t q = partHeaders.find('"', fn + 1);
                if (q != std::string::npos) filename = partHeaders.substr(fn + 1, q - (fn + 1));
            } else {
                size_t e = partHeaders.find_first_of(";\r\n", fn);
                filename = partHeaders.substr(fn, e == std::string::npos ? std::string::npos : (e - fn));
                filename = trim(filename);
            }

            out.filename = sanitizeFilename(filename.empty() ? "upload.bin" : filename);
            out.data = body.substr(dataStart, next - dataStart);
            return true;
        }

        pos = next + 2;
    }
}

RequestParser::RequestParser() 
    : _maxBodySize(100*1024*1024), _headerParsed(false), _contentLength(0), _isChunked(false), _bodyStart(0) {}

void RequestParser::setMaxBodySize(size_t size) { _maxBodySize = size; }

ParseStatus::Enum RequestParser::consume(const char* buffer, size_t length) {
    _buffer.append(buffer, length);

    if (!_headerParsed) {
        size_t headerEnd = _buffer.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            _headerParsed = true;
            _bodyStart = headerEnd + 4;
            std::string headers = _buffer.substr(0, headerEnd);
            // std::cout << "DEBUG: Headers parsed. Body start at " << _bodyStart << std::endl;
            
            size_t pos = headers.find("Content-Length:");
            if (pos != std::string::npos) {
                _contentLength = std::strtoul(headers.c_str() + pos + 15, NULL, 10);
            } else {
                _contentLength = 0;
            }

            if (headers.find("Transfer-Encoding: chunked") != std::string::npos) {
                _isChunked = true;
            }
            
             if (!_isChunked && _contentLength > _maxBodySize) {
                return ParseStatus::Error_TooLarge;
            }
        }
    }

    if (_headerParsed) {
        if (_isChunked) {
            if (_buffer.size() > _bodyStart + 1000000) {
                 static size_t lastReportedSizeChunked = 0;
                 if (_buffer.size() > lastReportedSizeChunked + 1000000) {
                     std::cout << "DEBUG: Received chunked data, buffer size: " << _buffer.size() << " bytes" << std::endl;
                     lastReportedSizeChunked = _buffer.size();
                 }
            }

            size_t searchStart = _bodyStart;
            if (_buffer.size() > length + 20) {
                size_t calculatedStart = _buffer.size() - length - 20;
                if (calculatedStart > _bodyStart) searchStart = calculatedStart;
            }

            if (_buffer.find("0\r\n\r\n", searchStart) != std::string::npos) {
                return ParseStatus::Done;
            }
            
            // Allow overhead for chunked encoding (e.g. 50% more to be safe, or just a fixed buffer)
            // The strict check is done in Http::isBodySizeAllowed after decoding
            size_t limit = _maxBodySize + (50 * 1024 * 1024); 
            if (_buffer.size() - _bodyStart > limit) {
                std::cout << "DEBUG: Chunked body too large. Size: " << (_buffer.size() - _bodyStart) << ", Limit: " << limit << std::endl;
                return ParseStatus::Error_TooLarge;
            }
        } else {
            // Debugging for large body
            if (_contentLength > 1000000) {
                 static size_t lastReportedSize = 0;
                 if (_buffer.size() > lastReportedSize + 1000000) {
                     std::cout << "DEBUG: Received " << _buffer.size() << " / " << (_bodyStart + _contentLength) << " bytes" << std::endl;
                     lastReportedSize = _buffer.size();
                 }
            }

            if (_buffer.size() >= _bodyStart + _contentLength) {
                return ParseStatus::Done;
            }
        }
    }
    
    if (!_headerParsed && _buffer.size() > 16384) {
        std::cout << "DEBUG: Header too large. Size: " << _buffer.size() << std::endl;
        return ParseStatus::Error_InvalidFormat;
    }

    return ParseStatus::Continue;
}

bool RequestParser::hasRequest() const {
    if (!_headerParsed) return false;
    if (_isChunked) return _buffer.find("0\r\n\r\n", _bodyStart) != std::string::npos;
    return _buffer.size() >= _bodyStart + _contentLength;
}

std::string RequestParser::extractRequest() {
    if (!hasRequest()) return "";
    std::string req;
    if (_isChunked) {
        size_t end = _buffer.find("0\r\n\r\n", _bodyStart);
        req = _buffer.substr(0, end + 5);
        _buffer.erase(0, end + 5);
    } else {
        req = _buffer.substr(0, _bodyStart + _contentLength);
        _buffer.erase(0, _bodyStart + _contentLength);
    }
    _headerParsed = false;
    _contentLength = 0;
    _isChunked = false;
    _bodyStart = 0;
    consume("", 0);
    return req;
}

void RequestParser::reset() {
    _buffer.clear();
    _headerParsed = false;
    _contentLength = 0;
    _isChunked = false;
    _bodyStart = 0;
}
