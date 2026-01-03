#ifndef HTTP_HPP
#define HTTP_HPP

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctime>
#include <cstdio> // For remove
#include <cerrno> // For errno
#include "../configReader/config.hpp"
#include "../errors/error.hpp"
#include "../cgi/cgi.hpp"

#define RESET       "\033[0m"
#define BLACK       "\033[30m"
#define RED         "\033[31m"
#define GREEN       "\033[32m"
#define YELLOW      "\033[33m"
#define BLUE        "\033[34m"
#define MAGENTA     "\033[35m"
#define CYAN        "\033[36m"
#define WHITE       "\033[37m"
#define ORANGE      "\033[38;5;208m"

struct MultipartFile {
    std::string filename;
    std::string data;
};

struct CGIinterpreter {
    std::string cgiExtension;
    std::string interpreterPath;
};

struct ParseStatus {
    enum Enum {
        Continue,
        Done,
        HeadersDone,
        Error_TooLarge,
        Error_InvalidFormat
    };
};




class Http {
    
    private:
        ServerConfig _serverData;
        const std::string* _rawRequestPtr;  // wskaźnik do oryginalnego requesta
        size_t _bodyStart;
        size_t _bodyLen;
        HttpError _httpError;
        
        // Parsing state
        enum InternalState {
            STATE_REQUEST_LINE,
            STATE_HEADERS,
            STATE_BODY,
            STATE_CHUNK_SIZE,
            STATE_CHUNK_DATA,
            STATE_CHUNK_TRAILER,
            STATE_COMPLETE
        };
        InternalState _state;
        std::string _parsingBuffer;
        size_t _contentLength;
        bool _isChunked;
        size_t _chunkSize;
        size_t _bodyBytesReceived;

        std::string _bodyBuffer;
        std::string _rawBuffer;
        std::string _tempFilePath;
        size_t _cleanBodySize;
        bool _usingTempFile;
        int _tempFileFd;
        

        
        void handleBodyData(const std::string& data);
        void createTempFile();
        void writeToTempFile(const std::string& data);

        // Helper functions for parsing
        void parseRequestLine();
        void parseHeaders();
        void processHeadersEnd();
        void parseBody();
        void parseChunkedBody();

        struct requestData {
            long long _contentLength;
            std::string _Root;
            std::string _Index;
            std::string _Redirect;
            std::string _CgiPath;
            std::string _CgiExt;
            std::string _UploadDir;
            bool Autoindex;
            std::vector<std::string> _allowedMethods;
            std::string _rawHeader;
            std::string _uri;
            std::string _request;
            std::string _queryString;
            std::string  _fileName;
            std::string _httpVersion;
            std::string _method;
            std::map<std::string, std::string> _headers;
            requestData() : 
                _contentLength(0),
                _Root(""),
                _Index(""),
                _Redirect(""),
                _CgiPath(""),
                _CgiExt(""),
                _UploadDir(""),
                Autoindex(false),
                _allowedMethods(),
                _rawHeader(""),
                _uri(""),
                _httpVersion(""),
                _method(""),
                _headers()
                {}
            } _s_requestData;
            
            LocationConfig _myConfig;
            struct responseData {
                bool _hasError;
                std::string _responseHttpVersion;
            std::map<std::string, std::string> _responseHeaders;
            long long _clientMaxBodySize;
            std::string _responseHeader;
            int _responseStatusCode;
            std::string _responseBody;
            std::string _response;
            responseData() : 
                _hasError(false),
                _responseHttpVersion(""),
                _responseHeaders()  ,
                _clientMaxBodySize(0),
                _responseHeader(""),
                _responseStatusCode(0),
                _responseBody(""),
                _response("")
            {}
        } _s_responseData;

        // Parses a raw HTTP request.
        // void parseRequest(std::string &rawRequest); // Removed
        // void parseHeader(); // Removed
        bool isMethodAllowed();
        bool isBodySizeAllowed();
        size_t getCurrentBodySize() const;

        // Response functions
        void responseBuilder();
        void requestBilder(std::string &rawRequest);
        LocationConfig getMyConfig();
        bool isCGI();
        void handleRootPath();
        // void handleDirectory(const std::string& dirPath, const std::string& urlPath);
        void sendRedirect(const std::string& url, int code);
        bool resourceExists(const std::string& path);
        void sendError(int errorCode);
        bool isDirectory(const std::string& path);
        bool isFile(const std::string& path);
        void handleFile(const std::string& filePath);
        std::string determineMimeType(const std::string& path);
        void generateDirectoryListing(const std::string& dirPath);
        void deleteFile(const std::string& filePath);
        void deleteDirectory(const std::string& dirPath);
        std::string getCGIinterpreter(const std::string& fileName);
        // void chankuj(std::string& rawRequest); // Removed

        void cgiResponseBuilder();
        void getResponseBuilder();
        void postResponseBuilder();
        void deleteResponseBuilder();
        void putResponseBuilder();

        // Refactoring helpers
        void handleRequestCompletion();
        void processRequestURI();
        void setupCgiEnv(std::map<std::string, std::string>& envVars, const std::string& scriptPath, const std::string& body);
        void handlePutPostUpload(const std::string& reqBody, bool isPost);
        void serveFileOrIndex(const std::string& fullPath);
        void tryIndexFiles();
        void findLocationConfig(LocationConfig& myConfig, bool& baseFound, std::string& baseKey, LocationConfig& baseLoc, bool& regexFound, LocationConfig& regexLoc);
        void finalizeLocationConfig(LocationConfig& myConfig, bool baseFound, const std::string& baseKey, const LocationConfig& baseLoc, bool regexFound, const LocationConfig& regexLoc);
        
        bool handleFavicon();
        bool checkBodySize();
        bool checkMethodAllowed();
        void dispatchMethod();
        
        void handleSimplePost();
        void handleMultipartPost();
        void saveUploadedFile(MultipartFile& mf);
        std::string resolveUniquePath(const std::string& filename);
        bool writeFile(const std::string& path, const std::string& data);
        void sendUploadSuccess(const MultipartFile& mf);
        std::string getHeader(const std::string& key);

    public:
        Http();
        Http(const Http& other);
        Http& operator=(const Http& other);

        // Constructor for creating HTTP responses
        Http (std::string &rawRequest, ServerConfig serverData);
        Http (ServerConfig serverData); // New constructor
        ~Http();

        void setServerConfig(const ServerConfig& serverData);
        ParseStatus::Enum consume(const char* buffer, size_t length);
        void reset();

        // Generates the HTTP response string
        bool getIsError() const;
        int getStatusCode() const;
        std::string getResponse() const;
        bool shouldClose() const;
};

#endif