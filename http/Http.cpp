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
#include <limits>
#include <cerrno>

static bool extractBoundary(const std::string& contentType, std::string& boundaryOut);
static bool parseMultipartFilePart(const std::string& body,
                                   const std::string& boundary,
                                   MultipartFile& out,
                                   const std::string& fieldName);

static size_t safeParseSize(const std::string& value) {
    errno = 0;
    char* end = NULL;
    unsigned long long v = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || v > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(v);
}

static bool regexMatch(const std::string &pattern, const std::string &text)
{
    regex_t re;
    if (regcomp(&re, pattern.c_str(), REG_EXTENDED) != 0)
        return false;
    int rc = regexec(&re, text.c_str(), 0, NULL, 0);
    regfree(&re);
    return rc == 0;
}

// Constructor
Http::Http (std::string &rawRequest, ServerConfig serverData) 
    : _serverData(serverData), _state(STATE_REQUEST_LINE), 
      _contentLength(0), _isChunked(false), _chunkSize(0), 
      _bodyBytesReceived(0) 
{
    _rawRequestPtr = &rawRequest;
    requestBilder(rawRequest);
}

void Http::requestBilder(std::string &rawRequest) {
    consume(rawRequest.c_str(), rawRequest.length());
}

ParseStatus::Enum Http::consume(const char* buffer, size_t length) {
    // If already in error state, reject any new data and return immediately without re-parsing
    if (_s_responseData._hasError && _state == STATE_COMPLETE && !_s_responseData._response.empty()) {
        std::cout << RED << "DEBUG: Already sent error response, ignoring new data" << RESET << std::endl;
        return ParseStatus::Done;
    }

    _parsingBuffer.append(buffer, length);
    while (true) {
        if (_state == STATE_REQUEST_LINE) parseRequestLine();
        else if (_state == STATE_HEADERS) parseHeaders();
        else if (_state == STATE_BODY) parseBody();
        else if (_state == STATE_CHUNK_SIZE || _state == STATE_CHUNK_DATA) parseChunkedBody();
        else if (_state == STATE_CHUNK_TRAILER) _state = STATE_COMPLETE;
        else if (_state == STATE_COMPLETE) {
            handleRequestCompletion();
            return ParseStatus::Done;
        }

        if (_s_responseData._hasError && _state == STATE_COMPLETE) {
            // Error was set during parsing (e.g., body too large)
            // Call handleRequestCompletion to generate the error response
            std::cout << RED << "DEBUG: Error state detected during parsing, calling handleRequestCompletion" << RESET << std::endl;
            handleRequestCompletion();
            return ParseStatus::Done;
        }
        
        if ((_state == STATE_REQUEST_LINE || _state == STATE_HEADERS) && 
            _parsingBuffer.find("\r\n") == std::string::npos) break;
        if (_state == STATE_BODY && _parsingBuffer.size() < _contentLength) break;
        if (_state == STATE_CHUNK_DATA && _parsingBuffer.size() < _chunkSize + 2) {
            std::cout << YELLOW << "DEBUG WAITING FOR CHUNK DATA: have=" << _parsingBuffer.size()
                      << " need=" << (_chunkSize + 2) << RESET << std::endl;
            break;
        }
        if (_state == STATE_CHUNK_SIZE && _parsingBuffer.find("\r\n") == std::string::npos) {
            std::cout << YELLOW << "DEBUG WAITING FOR CHUNK SIZE LINE" << RESET << std::endl;
            break;
        }
    }
    return ParseStatus::Continue;
}

void Http::parseRequestLine() {
    size_t pos = _parsingBuffer.find("\r\n");
    if (pos == std::string::npos) return;
    std::string line = _parsingBuffer.substr(0, pos);
    _parsingBuffer.erase(0, pos + 2);

    // Ignore stray empty lines (e.g., keep-alive with extra CRLF) instead of treating them as a broken request.
    if (line.empty()) {
        return;
    }

    std::istringstream iss(line);
    iss >> _s_requestData._method >> _s_requestData._uri >> _s_requestData._httpVersion;
    _state = STATE_HEADERS;
}

void Http::parseHeaders() {
    size_t pos = _parsingBuffer.find("\r\n");
    if (pos == std::string::npos) return;
    std::string line = _parsingBuffer.substr(0, pos);
    _parsingBuffer.erase(0, pos + 2);
    if (line.empty()) {
        processHeadersEnd();
        return;
    }
    size_t colon = line.find(": ");
    if (colon != std::string::npos) {
        _s_requestData._headers[line.substr(0, colon)] = line.substr(colon + 2);
    }
}

void Http::processHeadersEnd() {
    // Populate config NOW so we can check limits during body/chunk parsing
    _myConfig = getMyConfig();
    _httpError = HttpError(_myConfig.error_pages);

    std::cout << YELLOW << "DEBUG: Config for " << _s_requestData._uri << ": limit=" 
              << (_myConfig.client_max_body_size.empty() ? "(empty)" : _myConfig.client_max_body_size) 
              << RESET << std::endl;

    if (_s_requestData._headers.count("Content-Length")) {
        _contentLength = std::strtoul(_s_requestData._headers["Content-Length"].c_str(), NULL, 10);
        _state = STATE_BODY;
    } else if (_s_requestData._headers["Transfer-Encoding"] == "chunked") {
        _isChunked = true;
        _state = STATE_CHUNK_SIZE;
    } else {
        _state = STATE_COMPLETE;
    }
    
    // Check body size limit early
    if (!isBodySizeAllowed()) {
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 413;
        // Set to COMPLETE so the main loop calls handleRequestCompletion
        // and generates the 413 response before returning to the caller
        _state = STATE_COMPLETE;
    }
}

void Http::parseBody() {
    if (_parsingBuffer.size() >= _contentLength) {
        _bodyBuffer = _parsingBuffer.substr(0, _contentLength);
        _parsingBuffer.erase(0, _contentLength);
        _state = STATE_COMPLETE;
        _bodyLen = _bodyBuffer.size();
        _bodyStart = 0;
    }
}

void Http::parseChunkedBody() {
    if (_state == STATE_CHUNK_SIZE) {
        size_t pos = _parsingBuffer.find("\r\n");
        if (pos == std::string::npos) return;
        std::string line = _parsingBuffer.substr(0, pos);
        _chunkSize = std::strtoul(line.c_str(), NULL, 16);
        _parsingBuffer.erase(0, pos + 2);
        std::cout << YELLOW << "DEBUG CHUNK SIZE: " << _chunkSize << RESET << std::endl;
        
        // Check limit AFTER reading chunk size but BEFORE trying to read chunk data
        if (_chunkSize > 0 && !_myConfig.client_max_body_size.empty()) {
            size_t maxBodySize = safeParseSize(_myConfig.client_max_body_size);
            size_t futureSize = _bodyBuffer.size() + _chunkSize;
            if (futureSize > maxBodySize) {
                std::cout << RED << "DEBUG: CHUNK SIZE EXCEEDS LIMIT (" << futureSize << " > " << maxBodySize << "), REJECTING" << RESET << std::endl;
                _s_responseData._hasError = true;
                _s_responseData._responseStatusCode = 413;
                _s_responseData._response.clear();
                _state = STATE_COMPLETE;
                _parsingBuffer.clear();
                return;
            }
        }
        
        _state = (_chunkSize == 0) ? STATE_CHUNK_TRAILER : STATE_CHUNK_DATA;
    } else if (_state == STATE_CHUNK_DATA) {
        if (_parsingBuffer.size() >= _chunkSize + 2) {
            // Check limit BEFORE appending to avoid buffering huge bodies
            size_t futureSize = _bodyBuffer.size() + _chunkSize;
            if (!_myConfig.client_max_body_size.empty()) {
                size_t maxBodySize = safeParseSize(_myConfig.client_max_body_size);
                if (futureSize > maxBodySize) {
                    std::cout << RED << "DEBUG: BODY WOULD BE TOO LARGE (" << futureSize << " > " << maxBodySize << "), REJECTING" << RESET << std::endl;
                    _s_responseData._hasError = true;
                    _s_responseData._responseStatusCode = 413;
                    _s_responseData._response.clear();
                    _state = STATE_COMPLETE;
                    // Clear parsing buffer to stop processing more data
                    _parsingBuffer.clear();
                    return;
                }
            }

            _bodyBuffer.append(_parsingBuffer.substr(0, _chunkSize));
            _parsingBuffer.erase(0, _chunkSize + 2);
            std::cout << YELLOW << "DEBUG CHUNK APPENDED, BODY NOW: " << _bodyBuffer.size() << RESET << std::endl;
            _state = STATE_CHUNK_SIZE;
        }
    }
}


bool Http::isBodySizeAllowed() {
    if (_myConfig.client_max_body_size.empty()) {
        std::cout << YELLOW << "DEBUG isBodySizeAllowed: no limit configured" << RESET << std::endl;
        return true;
    }
    
    size_t maxBodySize = safeParseSize(_myConfig.client_max_body_size);
    const size_t bodySize = getCurrentBodySize();

    std::cout << YELLOW << "DEBUG isBodySizeAllowed: body=" << bodySize << " max=" << maxBodySize 
              << " result=" << (bodySize <= maxBodySize ? "OK" : "TOO_LARGE") << RESET << std::endl;

    if (bodySize > maxBodySize) return false;
    return true;
}

size_t Http::getCurrentBodySize() const {
    if (_isChunked) return _bodyBuffer.size();
    if (!_bodyBuffer.empty()) return _bodyBuffer.size();
    return _contentLength;
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
    
    std::string body = _bodyBuffer;
    
    std::map<std::string, std::string> envVars;
    
    envVars["AUTH_TYPE"] = _s_requestData._headers["Authorization"];
    envVars["CONTENT_LENGTH"] = _s_requestData._headers["Content-Length"];
    envVars["CONTENT_TYPE"] = _s_requestData._headers["Content-Type"];
    envVars["GATEWAY_INTERFACE"] = "CGI/1.1";
    envVars["PATH_TRANSLATED"] = scriptPath;
    envVars["REMOTE_ADDR"] = _s_requestData._headers["Host"];
    envVars["REMOTE_HOST"] = _s_requestData._headers["Host"];
    envVars["REMOTE_IDENT"] = "";
    envVars["REMOTE_USER"] = "";
    envVars["REQUEST_METHOD"] = _s_requestData._method;
    envVars["SCRIPT_FILENAME"] = scriptPath;
    envVars["SERVER_NAME"] = _serverData.server_name;
    envVars["SERVER_PORT"] = _serverData.listen_port;
    envVars["SERVER_PROTOCOL"] = _s_requestData._httpVersion;
    envVars["SERVER_SOFTWARE"] = "WebServer/1.0";
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

        // Only serve index fallbacks when the request targets a directory (empty file name).
        if (_s_requestData._fileName.empty()) {
            // Fallback 1: Location Index
            if (!_myConfig.index.empty()) {
                std::string indexPath = _myConfig.root;
                if (!indexPath.empty() && indexPath[indexPath.length() - 1] != '/') indexPath += "/";
                indexPath += _myConfig.index;
                
                if (resourceExists(indexPath) && isFile(indexPath)) {
                    handleFile(indexPath);
                    return;
                }
            }

            // Fallback 2: Server Root Index
            if (!_serverData.index.empty()) {
                std::string serverIndexPath = _serverData.root;
                if (!serverIndexPath.empty() && serverIndexPath[serverIndexPath.length() - 1] != '/') serverIndexPath += "/";
                serverIndexPath += _serverData.index;
                
                if (resourceExists(serverIndexPath) && isFile(serverIndexPath)) {
                    handleFile(serverIndexPath);
                    return;
                }
            }
        }

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
    std::string reqBody = _bodyBuffer;

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
    if (_myConfig.upload_dir.empty()) {
        handleSimplePost();
        return;
    }
    handleMultipartPost();
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
    std::cout << YELLOW << "DEBUG: Attempting to delete file: " << filePath << RESET << std::endl;

    if (filePath.find("..") != std::string::npos) {
        std::cout << RED << "DEBUG: Delete failed - path contains '..'" << RESET << std::endl;
        sendError(403); 
        return;
    }
    
    if (filePath.find(_myConfig.upload_dir) != 0) {
        std::cout << RED << "DEBUG: Delete failed - path not in upload dir: " << _myConfig.upload_dir << RESET << std::endl;
        sendError(403); 
        return;
    }

    if (access(filePath.c_str(), W_OK) != 0) {
        std::cout << RED << "DEBUG: Delete failed - no write access" << RESET << std::endl;
        sendError(403); 
        return;
    }
    
    if (std::remove(filePath.c_str()) != 0) {
        std::cout << RED << "DEBUG: Delete failed - std::remove error" << RESET << std::endl;
        sendError(500);
        return;
    }
    
    std::cout << GREEN << "DEBUG: File deleted successfully" << RESET << std::endl;
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
    std::cout << YELLOW << "DEBUG: Attempting to delete directory: " << dirPath << RESET << std::endl;
    std::string urlPath = _s_requestData._uri;
    if (urlPath[urlPath.length() - 1] != '/') {
        std::cout << RED << "DEBUG: Delete dir failed - URI missing trailing slash" << RESET << std::endl;
        sendError(409); 
        return;
    }
    
    if (access(dirPath.c_str(), W_OK) != 0) {
        std::cout << RED << "DEBUG: Delete dir failed - no write access" << RESET << std::endl;
        sendError(403);
        return;
    }
    
    if (rmdir(dirPath.c_str()) != 0) {
        std::cout << RED << "DEBUG: Delete dir failed - rmdir error: " << errno << RESET << std::endl;
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
    if (handleFavicon()) return;
    if (checkBodySize()) return;

    if ((_s_requestData._method == "GET" || _s_requestData._method == "POST") && isCGI()) {
        cgiResponseBuilder();
        return;
    }
    
    if (checkMethodAllowed()) return;
    
    dispatchMethod();
}

LocationConfig Http::getMyConfig() {
    LocationConfig myConfig;
    bool baseFound = false;
    std::string baseKey;
    LocationConfig baseLoc;
    bool regexFound = false;
    LocationConfig regexLoc;

    findLocationConfig(myConfig, baseFound, baseKey, baseLoc, regexFound, regexLoc);

    finalizeLocationConfig(myConfig, baseFound, baseKey, baseLoc, regexFound, regexLoc);

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

// // RequestParser::RequestParser() request parser ma zniknac, nie uzywamy tej klasy
//     : _maxBodySize(100*1024*1024), _headerParsed(false), _contentLength(0), _isChunked(false), _bodyStart(0) {}

// void RequestParser::setMaxBodySize(size_t size) { _maxBodySize = size; }



Http::Http() : _rawRequestPtr(NULL), _bodyStart(0), _bodyLen(0), _state(STATE_REQUEST_LINE), _contentLength(0), _isChunked(false), _chunkSize(0), _bodyBytesReceived(0) {}

Http::Http(const Http& other) {
    *this = other;
}

Http& Http::operator=(const Http& other) {
    if (this != &other) {
        _serverData = other._serverData;
        _rawRequestPtr = other._rawRequestPtr;
        _bodyStart = other._bodyStart;
        _bodyLen = other._bodyLen;
        _httpError = other._httpError;
        _state = other._state;
        _parsingBuffer = other._parsingBuffer;
        _contentLength = other._contentLength;
        _isChunked = other._isChunked;
        _chunkSize = other._chunkSize;
        _bodyBytesReceived = other._bodyBytesReceived;
        _bodyBuffer = other._bodyBuffer;
        _s_requestData = other._s_requestData;
        _myConfig = other._myConfig;
        _s_responseData = other._s_responseData;
    }
    return *this;
}

Http::~Http() {}

void Http::setServerConfig(const ServerConfig& serverData) {
    _serverData = serverData;
}

void Http::reset() {
    _state = STATE_REQUEST_LINE;
    _contentLength = 0;
    _isChunked = false;
    _chunkSize = 0;
    _bodyBytesReceived = 0;
    _bodyBuffer.clear();
    _s_requestData = requestData();
    _s_responseData = responseData();
}

void Http::processRequestURI() {
    size_t queryPos = _s_requestData._request.find('?');
    if (queryPos != std::string::npos) {
        _s_requestData._queryString = _s_requestData._request.substr(queryPos + 1);
        _s_requestData._fileName = _s_requestData._request.substr(0, queryPos);
    } else {
        _s_requestData._fileName = _s_requestData._request;
    }
    
    while (!_s_requestData._fileName.empty() && _s_requestData._fileName[0] == '/')
        _s_requestData._fileName = _s_requestData._fileName.substr(1);
}

void Http::handleRequestCompletion() {
    if (!_s_responseData._response.empty()) return;

    // Config was already populated in processHeadersEnd for body size checks
    // If not set (e.g., no body), populate now
    if (_myConfig.root.empty()) {
        _myConfig = getMyConfig();
        _httpError = HttpError(_myConfig.error_pages);
    }

    processRequestURI();

    std::cout << YELLOW << "DEBUG REQUEST: " << _s_requestData._method << " " << _s_requestData._uri << RESET << std::endl;
    std::cout << YELLOW << "DEBUG FILENAME: " << _s_requestData._fileName << RESET << std::endl;
    std::cout << YELLOW << "DEBUG META: CLEN=" << _contentLength
              << " TE=" << (_isChunked ? "chunked" : "none")
              << " HDR_CLEN='" << (_s_requestData._headers.count("Content-Length") ? _s_requestData._headers.at("Content-Length") : "") << "'"
              << RESET << std::endl;

    if (_s_responseData._hasError) {
        int code = _s_responseData._responseStatusCode ? _s_responseData._responseStatusCode : 500;
        _s_responseData._response = _httpError.generateErrorResponse(code);
        std::cout << RED << "DEBUG: SENDING ERROR RESPONSE " << code << RESET << std::endl;
        return;
    }
    responseBuilder();

    std::cout << YELLOW << "DEBUG RESPONSE STATUS: " << _s_responseData._responseStatusCode << RESET << std::endl;
}

void Http::findLocationConfig(LocationConfig& myConfig, bool& baseFound, std::string& baseKey, LocationConfig& baseLoc, bool& regexFound, LocationConfig& regexLoc) {
    (void)myConfig;
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
}

void Http::finalizeLocationConfig(LocationConfig& myConfig, bool baseFound, const std::string& baseKey, const LocationConfig& baseLoc, bool regexFound, const LocationConfig& regexLoc) {
    if (!baseFound) {
        myConfig.root = _serverData.root;
        myConfig.allow_methods = _serverData.allow_methods;
        myConfig.index = _serverData.index;
        myConfig.client_max_body_size = _serverData.client_max_body_size;
        myConfig.autoindex = _serverData.autoindex;
        myConfig.has_redirect = _serverData.has_redirect;
        myConfig.redirect_url = _serverData.redirect_url;
        myConfig.redirect_code = _serverData.redirect_code;
        myConfig.upload_dir = _serverData.upload_dir;
        myConfig.interpreters = _serverData.interpreters;
        myConfig.scripts_path = _serverData.scripts_path;
        myConfig.error_pages = _serverData.error_pages;
        _s_requestData._request = _s_requestData._uri;
    } else {
        myConfig = Config::getMergedLocationConfig(_serverData, baseLoc);
        if (_s_requestData._uri.find(baseKey) == 0 && _s_requestData._uri[baseKey.length()] == '/')
            _s_requestData._request = _s_requestData._uri.substr(baseKey.length());
        else if (_s_requestData._uri.find(baseKey) == 0 && _s_requestData._uri.length() != baseKey.length())
            _s_requestData._request = _s_requestData._uri;
        else if (_s_requestData._uri == baseKey)
             _s_requestData._request = "";
        else
            _s_requestData._request = _s_requestData._uri.substr(baseKey.length());
    }

    if (regexFound) {
        LocationConfig mergedRegex = Config::getMergedLocationConfig(_serverData, regexLoc);
        if (!mergedRegex.root.empty()) myConfig.root = mergedRegex.root;
        if (!mergedRegex.index.empty()) myConfig.index = mergedRegex.index;
        if (!mergedRegex.client_max_body_size.empty()) myConfig.client_max_body_size = mergedRegex.client_max_body_size;
        if (!mergedRegex.upload_dir.empty()) myConfig.upload_dir = mergedRegex.upload_dir;
        if (!mergedRegex.allow_methods.empty()) myConfig.allow_methods = mergedRegex.allow_methods;
        if (!mergedRegex.error_pages.empty()) myConfig.error_pages = mergedRegex.error_pages;
        if (!mergedRegex.interpreters.empty()) myConfig.interpreters = mergedRegex.interpreters;
        myConfig.autoindex = mergedRegex.autoindex;
    }
}

bool Http::handleFavicon() {
    if (_s_requestData._uri == "/favicon.ico") {
        _s_responseData._response = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        return true;
    }
    return false;
}

bool Http::checkBodySize() {
    const size_t bodySize = getCurrentBodySize();

    std::cout << YELLOW << "DEBUG BODY SIZE: " << bodySize
              << " / LIMIT: " << (_myConfig.client_max_body_size.empty() ? "(none)" : _myConfig.client_max_body_size)
              << " CLEN=" << _contentLength
              << " CHUNKED=" << (_isChunked ? "yes" : "no")
              << RESET << std::endl;

    if (!isBodySizeAllowed()) {
        std::cout << RED << "DEBUG: BODY TOO LARGE (" << bodySize << ") > LIMIT (" 
                  << (_myConfig.client_max_body_size.empty() ? "none" : _myConfig.client_max_body_size)
                  << ")" << RESET << std::endl;
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 413;
        _s_responseData._response = _httpError.generateErrorResponse(413);
        return true;
    }
    return false;
}

bool Http::checkMethodAllowed() {
   if (!isMethodAllowed()) {
        _s_responseData._hasError = true;
        _s_responseData._responseStatusCode = 405;
        _s_responseData._response = _httpError.generateErrorResponse(405);
        return true;
    }
    return false;
}

void Http::dispatchMethod() {
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

std::string Http::getHeader(const std::string& key) {
    std::map<std::string, std::string>::iterator it = _s_requestData._headers.find(key);
    if (it != _s_requestData._headers.end()) return it->second;
    return "";
}

void Http::handleSimplePost() {
    std::ostringstream html;
    html << "<html><body><h1>POST Data Received</h1>"
         << "<p>Content-Length: " << _bodyBuffer.size() << " bytes</p>"
         << "<pre>" << _bodyBuffer << "</pre></body></html>";
    
    std::string respBody = html.str();
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
             << "Content-Length: " << respBody.size() << "\r\n"
             << "Connection: keep-alive\r\n\r\n" << respBody;

    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 200;
}

void Http::handleMultipartPost() {
    std::string contentType = getHeader("Content-Type");
    std::string boundary;
    if (contentType.find("multipart/form-data") == std::string::npos || !extractBoundary(contentType, boundary)) {
        sendError(400);
        return;
    }

    MultipartFile mf;
    if (!parseMultipartFilePart(_bodyBuffer, boundary, mf, "file")) {
        if (!parseMultipartFilePart(_bodyBuffer, boundary, mf, "")) {
            sendError(400);
            return;
        }
    }
    saveUploadedFile(mf);
}

std::string Http::resolveUniquePath(const std::string& filename) {
    std::string fullPath = _myConfig.upload_dir + filename;
    int probe = open(fullPath.c_str(), O_RDONLY);
    if (probe != -1) {
        close(probe);
        std::ostringstream oss;
        oss << _myConfig.upload_dir << "/" << std::time(NULL) << "_" << filename;
        return oss.str();
    }
    return fullPath;
}

bool Http::writeFile(const std::string& path, const std::string& data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return false;

    ssize_t written = write(fd, data.data(), data.size());
    close(fd);

    if (written == -1 || static_cast<size_t>(written) != data.size()) {
        std::remove(path.c_str());
        return false;
    }
    return true;
}

void Http::sendUploadSuccess(const MultipartFile& mf) {
    std::ostringstream html;
    html << "<html><body><h1>File Uploaded</h1>"
         << "<p>Saved as: " << mf.filename << "</p>"
         << "<p>Bytes: " << mf.data.size() << "</p></body></html>";
    
    std::string respBody = html.str();
    std::ostringstream response;
    response << "HTTP/1.1 201 Created\r\nContent-Type: text/html\r\n"
             << "Content-Length: " << respBody.size() << "\r\n"
             << "Connection: keep-alive\r\n\r\n" << respBody;

    _s_responseData._response = response.str();
    _s_responseData._responseStatusCode = 201;
}

void Http::saveUploadedFile(MultipartFile& mf) {
    std::string fullPath = resolveUniquePath(mf.filename);
    if (!writeFile(fullPath, mf.data)) {
        sendError(500);
        return;
    }
    sendUploadSuccess(mf);
}
