#include "RequestParser.hpp"
#include "Http.hpp"

RequestParser::RequestParser() 
        : _buffer(),
            _maxBodySize(100 * 1024 * 1024),
            _contentLength(0),
            _headerParsed(false),
            _isChunked(false),
            _bodyStart(0),
            _chunkedPos(0) {}

void RequestParser::setMaxBodySize(size_t size) { _maxBodySize = size; }

ParseStatus::Enum RequestParser::consume(const char* incommingData, size_t length) {
    _buffer.append(incommingData, length);

    std::cout << std::endl << YELLOW << "DEBUG Buffer: " << length << " bytes" << std::endl << std::string(incommingData, length) << RESET << std::endl;

    if (!_headerParsed) {
        std::cout << "DEBUG: Parsing headers" << std::endl;
        ParseStatus::Enum status = parseHeaders();
        if (status != ParseStatus::Continue) return status;
    }

    if (_headerParsed) {
        if (_isChunked) {
            return processChunkedBody(length);
        } else {
            return processRegularBody();
        }
    }
    
    return checkHeaderSize();
}

ParseStatus::Enum RequestParser::parseHeaders() {
    size_t headerEnd = _buffer.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        _headerParsed = true;
        _bodyStart = headerEnd + 4;
        std::string headers = _buffer.substr(0, headerEnd);
        
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
    return ParseStatus::Continue;
}

ParseStatus::Enum RequestParser::processChunkedBody(size_t length) {
    logChunkedProgress();

    if (isChunkedBodyComplete(length)) {
        return ParseStatus::Done;
    }
    
    return checkChunkedBodyLimit();
}

void RequestParser::logChunkedProgress() {
    if (_buffer.size() > _bodyStart + 1000000) {
            static size_t lastReportedSizeChunked = 0;
            if (_buffer.size() > lastReportedSizeChunked + 1000000) {
                std::cout << "DEBUG: Received chunked data, buffer size: " << _buffer.size() << " bytes" << std::endl;
                lastReportedSizeChunked = _buffer.size();
            }
    }
}

bool RequestParser::isChunkedBodyComplete(size_t length) {
    size_t searchStart = _bodyStart;
    if (_buffer.size() > length + 20) {
        size_t calculatedStart = _buffer.size() - length - 20;
        if (calculatedStart > _bodyStart) searchStart = calculatedStart;
    }

    return _buffer.find("0\r\n\r\n", searchStart) != std::string::npos;
}

ParseStatus::Enum RequestParser::checkChunkedBodyLimit() {
    // Allow overhead for chunked encoding (e.g. 50% more to be safe, or just a fixed buffer)
    // The strict check is done in Http::isBodySizeAllowed after decoding
    size_t limit = _maxBodySize + (50 * 1024 * 1024); 
    if (_buffer.size() - _bodyStart > limit) {
        std::cout << "DEBUG: Chunked body too large. Size: " << (_buffer.size() - _bodyStart) << ", Limit: " << limit << std::endl;
        return ParseStatus::Error_TooLarge;
    }
    return ParseStatus::Continue;
}

ParseStatus::Enum RequestParser::processRegularBody() {
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
    return ParseStatus::Continue;
}

ParseStatus::Enum RequestParser::checkHeaderSize() {
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
