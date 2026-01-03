#ifndef REQUESTPARSER_HPP
#define REQUESTPARSER_HPP

#include <string>
#include <iostream>
#include <cstdlib>

struct ParseStatus {
    enum Enum {
        Continue,
        Done,
        Error_TooLarge,
        Error_InvalidFormat
    };
};

class RequestParser {
    private:
        std::string _buffer;
        size_t _maxBodySize;
        size_t _contentLength;
        bool _headerParsed;
        bool _isChunked;
        size_t _bodyStart;
        size_t _chunkedPos; // Tracks position in chunked body

        ParseStatus::Enum parseHeaders();
        ParseStatus::Enum processChunkedBody(size_t length);
        ParseStatus::Enum processRegularBody();
        ParseStatus::Enum checkHeaderSize();

        void logChunkedProgress();
        bool isChunkedBodyComplete(size_t length);
        ParseStatus::Enum checkChunkedBodyLimit();

    public:
        RequestParser();
        void setMaxBodySize(size_t size);
        ParseStatus::Enum consume(const char* buffer, size_t length);
        bool hasRequest() const;
        std::string extractRequest();
        void reset();
};

#endif
