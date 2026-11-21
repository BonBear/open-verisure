/*
  GuardedWebServer.cpp - dead simple web server with a bounded run time for every request.

  Derived from the WebServer library of Arduino-ESP32 3.3.12 (WebServer.cpp, Parsing.cpp):
    Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
    Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  The list of changes against the core library is in GuardedWebServer.h.
*/

#include "GuardedWebServer.h"

#define GWS_STR2(a) #a
#define GWS_STR(a) GWS_STR2(a)
static const char* const s_httpMethodNames[] = {
#define XX(num, name, string) GWS_STR(name),
    HTTP_METHOD_MAP(XX)
#undef XX
};

static const char* responseCodeToString(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Time-out";
        case 409: return "Conflict";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Large";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "";
    }
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static String urlDecode(const String& text) {
    String out;
    const unsigned int len = text.length();
    out.reserve(len);
    for (unsigned int i = 0; i < len; i++) {
        const char c = text.charAt(i);
        if (c == '%' && i + 2 < len && hexValue(text.charAt(i + 1)) >= 0 && hexValue(text.charAt(i + 2)) >= 0) {
            out += (char)((hexValue(text.charAt(i + 1)) << 4) | hexValue(text.charAt(i + 2)));
            i += 2;
        } else if (c == '+') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

GuardedWebServer::GuardedWebServer(int port) : _server(port) {}

void GuardedWebServer::begin() {
    _server.begin();
    _server.setNoDelay(true);
}

void GuardedWebServer::_addRoute(const char* uri, HTTPMethod method, THandlerFunction fn, THandlerFunction rawFn) {
    if (_routeCount >= GWS_MAX_ROUTES) return;   // a route table that is too small must be noticed in testing
    Route& r = _routes[_routeCount++];
    r.uri = uri;
    r.method = method;
    r.fn = fn;
    r.rawFn = rawFn;
}

void GuardedWebServer::on(const char* uri, HTTPMethod method, THandlerFunction fn) {
    _addRoute(uri, method, fn, nullptr);
}

void GuardedWebServer::onRawUpload(const char* uri, HTTPMethod method, THandlerFunction fn, THandlerFunction rawFn) {
    _addRoute(uri, method, fn, rawFn);
}

void GuardedWebServer::onNotFound(THandlerFunction fn) {
    _notFound = fn;
}

void GuardedWebServer::collectHeaders(const char* const headerKeys[], size_t headerKeysCount) {
    _headerCount = 0;
    _headers[_headerCount++].key = "Authorization";
    for (size_t i = 0; i < headerKeysCount && _headerCount < GWS_MAX_HEADERS; i++) {
        bool known = false;
        for (uint8_t j = 0; j < _headerCount; j++) {
            if (_headers[j].key.equalsIgnoreCase(headerKeys[i])) known = true;
        }
        if (!known) _headers[_headerCount++].key = headerKeys[i];
    }
}

String GuardedWebServer::header(const String& name) const {
    for (uint8_t i = 0; i < _headerCount; i++) {
        if (_headers[i].key.equalsIgnoreCase(name)) return _headers[i].value;
    }
    return "";
}

String GuardedWebServer::arg(const String& name) const {
    for (uint8_t i = 0; i < _argCount; i++) {
        if (_args[i].key == name) return _args[i].value;
    }
    return "";
}

bool GuardedWebServer::hasArg(const String& name) const {
    for (uint8_t i = 0; i < _argCount; i++) {
        if (_args[i].key == name) return true;
    }
    return false;
}

// ---- receiving a request (all waits bounded) ----

/** @return true once the request deadline has passed (never when the deadline is off). */
bool GuardedWebServer::_expired() const {
    return _budgetMs && (millis() - _reqStart) >= _budgetMs;
}

void GuardedWebServer::_resetRequest() {
    for (uint8_t i = 0; i < _headerCount; i++) _headers[i].value = "";
    for (uint8_t i = 0; i < _argCount; i++) {
        _args[i].key = "";
        _args[i].value = "";
    }
    _argCount = 0;
    _hostHeader = "";
    _currentUri = "";
    _clientContentLength = 0;
    _route = nullptr;
    _rawActive = false;
    _clearResponseHeaders();
}

/** Reads one line (LF-terminated, CRs dropped). Waits only inside the request deadline. */
GuardedWebServer::LineStatus GuardedWebServer::_readLine(String& line, size_t maxLength) {
    line = "";
    line.reserve(96);
    for (;;) {
        if (_expired()) return LINE_TIMEOUT;
        const int c = _currentClient.read();
        if (c < 0) {
            if (!_currentClient.connected()) return LINE_TIMEOUT;
            feedLoopWDT();   // still inside the deadline
            delay(1);
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') return LINE_OK;
        if (line.length() >= maxLength) return LINE_TOO_LONG;
        line += (char)c;
    }
}

/** Reads exactly length bytes into out, inside the request deadline. */
bool GuardedWebServer::_readBody(String& out, size_t length) {
    out = "";
    out.reserve(length);
    uint8_t tmp[64];
    size_t got = 0;
    while (got < length) {
        if (_expired()) return false;
        const size_t left = length - got;
        const int r = _currentClient.read(tmp, left < sizeof(tmp) ? left : sizeof(tmp));
        if (r < 0) return false;
        if (r == 0) {
            feedLoopWDT();   // still inside the deadline
            delay(1);
            continue;
        }
        out.concat((const char*)tmp, (unsigned int)r);
        got += (size_t)r;
    }
    return true;
}

/** Reads one raw upload piece; gives up when it is not complete GWS_RAW_PIECE_MS after it began. */
size_t GuardedWebServer::_readPiece(uint8_t* buf, size_t want) {
    size_t got = 0;
    const unsigned long start = millis();
    while (got < want) {
        const int r = _currentClient.read(buf + got, want - got);
        if (r < 0) break;                       // connection failed or was closed (also by the handler)
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (millis() - start >= GWS_RAW_PIECE_MS) break;
        feedLoopWDT();                          // still inside the piece deadline
        delay(1);
    }
    return got;
}

/** Streams the body to the route's rawFn() piece by piece. Exempt from the request deadline. */
bool GuardedWebServer::_readRaw() {
    _budgetMs = 0;
    _rawActive = true;
    _raw.status = RAW_START;
    _raw.totalSize = 0;
    _raw.currentSize = 0;
    _route->rawFn();
    _raw.status = RAW_WRITE;
    while (_raw.totalSize < (size_t)_clientContentLength) {
        const size_t left = (size_t)_clientContentLength - _raw.totalSize;
        const size_t want = left < GWS_RAW_BUFLEN ? left : GWS_RAW_BUFLEN;
        const size_t got = _readPiece(_raw.buf, want);
        _raw.currentSize = got;
        if (got < want) {                       // dropped, stalled or too slow: abort the upload
            _raw.status = RAW_ABORTED;
            _route->rawFn();
            return false;
        }
        _raw.totalSize += got;
        _route->rawFn();
    }
    _raw.status = RAW_END;
    _raw.currentSize = 0;
    _route->rawFn();
    return true;
}

void GuardedWebServer::_collectHeader(const String& name, const String& value) {
    if (name.equalsIgnoreCase("Host")) _hostHeader = value;
    for (uint8_t i = 0; i < _headerCount; i++) {
        if (_headers[i].key.equalsIgnoreCase(name)) {
            _headers[i].value = value;
            return;
        }
    }
}

void GuardedWebServer::_addArg(const String& key, const String& value) {
    if (_argCount >= GWS_MAX_ARGS) return;
    _args[_argCount].key = key;
    _args[_argCount].value = value;
    _argCount++;
}

/** Splits "a=1&b=2" into arguments (at most maxArgs in total, extra ones are ignored). */
void GuardedWebServer::_parseArguments(const String& data, uint8_t maxArgs) {
    const int len = (int)data.length();
    int pos = 0;
    while (pos < len && _argCount < maxArgs) {
        const int amp = data.indexOf('&', pos);
        const int end = (amp == -1) ? len : amp;
        const int eq = data.indexOf('=', pos);
        if (eq != -1 && eq < end) {
            _addArg(urlDecode(data.substring(pos, eq)), urlDecode(data.substring(eq + 1, end)));
        }
        pos = end + 1;
    }
}

/** Short reply for a request that is dropped part-way; the peer's pending input is drained first
 *  (briefly, bounded) because closing with unread input resets the connection and loses the reply. */
void GuardedWebServer::_error(const char* status) {
    char msg[112];
    const int n = snprintf(msg, sizeof(msg), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    if (n > 0 && (size_t)n < sizeof(msg)) {
        gwsWriteBounded(_currentClient, (const uint8_t*)msg, (size_t)n, GWS_ERROR_WRITE_MS, GWS_ERROR_WRITE_MS);
    }
    uint8_t discard[64];
    size_t drained = 0;
    const unsigned long start = millis();
    while (drained < 2048 && millis() - start < 50) {
        const int avail = _currentClient.available();
        if (avail <= 0) break;
        const int r = _currentClient.read(discard, (size_t)avail < sizeof(discard) ? (size_t)avail : sizeof(discard));
        if (r <= 0) break;
        drained += (size_t)r;
    }
}

bool GuardedWebServer::_parseRequest() {
    _resetRequest();
    _reqStart = millis();
    _budgetMs = GWS_REQUEST_BUDGET_MS;
    feedLoopWDT();   // a fresh 5 s window: the deadline below keeps this whole function under 3 s

    String req;
    switch (_readLine(req, GWS_MAX_LINE_LEN)) {
        case LINE_TOO_LONG: _error("414 URI Too Long"); return false;
        case LINE_TIMEOUT:  _error("408 Request Timeout"); return false;
        case LINE_OK:       break;
    }
    // "GET /path?query HTTP/1.1"
    const int addrStart = req.indexOf(' ');
    const int addrEnd = req.indexOf(' ', addrStart + 1);
    if (addrStart == -1 || addrEnd == -1) return false;
    const String methodStr = req.substring(0, addrStart);
    String url = req.substring(addrStart + 1, addrEnd);
    String search;
    const int query = url.indexOf('?');
    if (query != -1) {
        search = url.substring(query + 1);
        url = url.substring(0, query);
    }
    if (url.length() + search.length() > GWS_MAX_URI_LEN) {
        _error("414 URI Too Long");
        return false;
    }
    HTTPMethod method = HTTP_ANY;
    for (size_t i = 0; i < sizeof(s_httpMethodNames) / sizeof(s_httpMethodNames[0]); i++) {
        if (methodStr == s_httpMethodNames[i]) {
            method = (HTTPMethod)i;
            break;
        }
    }
    if (method == HTTP_ANY) return false;
    _currentMethod = method;
    _currentUri = url;

    for (uint8_t i = 0; i < _routeCount; i++) {
        if ((_routes[i].method == HTTP_ANY || _routes[i].method == method) && url == _routes[i].uri) {
            _route = &_routes[i];
            break;
        }
    }

    // headers
    bool multipart = false;
    bool urlEncoded = false;
    for (;;) {
        switch (_readLine(req, GWS_MAX_LINE_LEN)) {
            case LINE_TOO_LONG: _error("431 Request Header Fields Too Large"); return false;
            case LINE_TIMEOUT:  _error("408 Request Timeout"); return false;
            case LINE_OK:       break;
        }
        if (req.length() == 0) break;           // end of the headers
        const int colon = req.indexOf(':');
        if (colon == -1) break;
        const String name = req.substring(0, colon);
        String value = req.substring(colon + 1);
        value.trim();
        _collectHeader(name, value);
        if (name.equalsIgnoreCase("Content-Type")) {
            if (value.startsWith("multipart/")) multipart = true;
            else if (value.startsWith("application/x-www-form-urlencoded")) urlEncoded = true;
        } else if (name.equalsIgnoreCase("Content-Length")) {
            const long v = value.toInt();
            _clientContentLength = (v > 0 && v < 0x7FFFFFFFL) ? (int)v : 0;
        }
    }

    _parseArguments(search, GWS_MAX_ARGS - 2);  // two slots stay free for the body

    const bool hasBody = (method == HTTP_POST || method == HTTP_PUT || method == HTTP_PATCH || method == HTTP_DELETE);
    if (hasBody && !multipart && _route && _route->rawFn) {
        if (!_readRaw()) return false;
    } else if (hasBody && !multipart) {
        // unknown URIs and plain routes alike: a small body that must be complete inside the deadline
        if (_clientContentLength > GWS_MAX_BODY) {
            _error("413 Request Entity Too Large");
            return false;
        }
        String body;
        if (_clientContentLength > 0) {
            if (!_readBody(body, (size_t)_clientContentLength)) {
                _error("408 Request Timeout");
                return false;
            }
            if (urlEncoded) _parseArguments(body, GWS_MAX_ARGS);
            else _addArg("plain", body);
        }
    }
    // multipart bodies are not read at all (the handler refuses them); a GET has no body

    _currentClient.clear();
    return true;
}

void GuardedWebServer::_handleRequest() {
    feedLoopWDT();   // the request came in legitimately: a fresh window for the handler and its reply
    if (_route) {
        _route->fn();
    } else if (_notFound) {
        _notFound();
    } else {
        send(404, "text/plain", "Not found");
    }
}

void GuardedWebServer::handleClient() {
    if (!_haveClient) {
        _currentClient = _server.accept();
        if (!_currentClient) {
            delay(1);
            return;
        }
        _haveClient = true;
        _statusChange = millis();
    }

    bool keep = false;
    if (_currentClient.connected()) {
        if (_currentClient.available()) {
            if (_parseRequest()) _handleRequest();
        } else if (millis() - _statusChange <= GWS_MAX_DATA_WAIT_MS) {
            keep = true;   // no data yet: look again on the next loop() pass, never wait here
        }
    }
    if (!keep) {
        _currentClient = NetworkClient();
        _haveClient = false;
        _rawActive = false;
    }
}

// ---- sending a response (every write bounded, see GuardedSocket.h) ----

void GuardedWebServer::_clearResponseHeaders() {
    for (uint8_t i = 0; i < _respHeaderCount; i++) {
        _respHeaders[i].key = "";
        _respHeaders[i].value = "";
    }
    _respHeaderCount = 0;
}

void GuardedWebServer::sendHeader(const String& name, const String& value, bool first) {
    if (name.indexOf('\r') != -1 || name.indexOf('\n') != -1 || value.indexOf('\r') != -1 || value.indexOf('\n') != -1) {
        return;   // no header injection
    }
    if (_respHeaderCount >= GWS_MAX_RESP_HEADERS) return;
    if (first) {
        for (uint8_t i = _respHeaderCount; i > 0; i--) _respHeaders[i] = _respHeaders[i - 1];
        _respHeaders[0].key = name;
        _respHeaders[0].value = value;
    } else {
        _respHeaders[_respHeaderCount].key = name;
        _respHeaders[_respHeaderCount].value = value;
    }
    _respHeaderCount++;
}

void GuardedWebServer::_sendResponse(int code, const char* contentType, const char* body, size_t length) {
    feedLoopWDT();
    String h;
    h.reserve(224);
    h += "HTTP/1.1 ";
    h += String(code);
    h += ' ';
    h += responseCodeToString(code);
    h += "\r\nContent-Type: ";
    h += (contentType && contentType[0]) ? contentType : "text/html";
    h += "\r\nContent-Length: ";
    h += String((unsigned long)length);
    h += "\r\n";
    for (uint8_t i = 0; i < _respHeaderCount; i++) {
        h += _respHeaders[i].key;
        h += ": ";
        h += _respHeaders[i].value;
        h += "\r\n";
    }
    h += "Connection: close\r\n\r\n";
    _clearResponseHeaders();
    if (gwsWriteBounded(_currentClient, (const uint8_t*)h.c_str(), h.length(), GWS_SEND_STALL_MS, GWS_SEND_TOTAL_MS) != h.length()) {
        return;   // the client is gone or was dropped
    }
    if (length && body) {
        gwsWriteBounded(_currentClient, (const uint8_t*)body, length, GWS_SEND_STALL_MS, GWS_SEND_TOTAL_MS);
    }
}

void GuardedWebServer::send(int code, const char* contentType, const String& content) {
    _sendResponse(code, contentType, content.c_str(), content.length());
}

void GuardedWebServer::send(int code, const char* contentType, const char* content) {
    _sendResponse(code, contentType, content, content ? strlen(content) : 0);
}

void GuardedWebServer::send_P(int code, const char* contentType, const char* content) {
    _sendResponse(code, contentType, content, content ? strlen(content) : 0);
}

void GuardedWebServer::send_P(int code, const char* contentType, const char* content, size_t contentLength) {
    _sendResponse(code, contentType, content, contentLength);
}
