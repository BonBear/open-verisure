/*
  GuardedWebServer.h - dead simple web server with a bounded run time for every request.

  Derived from the WebServer library of Arduino-ESP32 3.3.12 (framework package
  framework-arduinoespressif32 4.30312.261116, libraries/WebServer: WebServer.h, WebServer.cpp,
  Parsing.cpp), which is
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

  ---- Why a copy, and what changed (all changes are by this project) ----
  The gateway's loop task is subscribed to the 5 s task watchdog, and the whole request is handled
  inside handleClient(). The core library blocks there for much longer than 5 s before any handler
  (and so any password check) runs: up to 5 s per header line and 10 s for all headers, 5 s per
  gap while a plain POST body arrives (restarted by every byte, so a trickle never ends), the same
  for multipart bodies, and up to 10 x 1 s in every write to a client that does not read. An
  unauthenticated client could so reboot the gateway. Changes against the core:
   1. One deadline for the whole request: request line, headers and (non-raw) body must be in
      within GWS_REQUEST_BUDGET_MS (3 s) of the first byte, else the client is dropped (408).
      The loop task watchdog is fed in every wait loop only while still inside the deadline.
   2. Raw uploads of a route registered with onRawUpload() (/api/ota) are exempt from that
      deadline, but every piece (GWS_RAW_BUFLEN bytes) must arrive within GWS_RAW_PIECE_MS (3 s),
      else the upload is aborted (RAW_ABORTED). The watchdog is fed while waiting for a piece.
   3. Plain bodies are limited to GWS_MAX_BODY bytes (413), also for unknown URIs; the body is read
      into one String, not through the core's realloc loop.
   4. Multipart bodies are not parsed (nothing in the gateway uses them): the handler is called
      without a body, and the connection is closed after the reply.
   5. Every response is written in pieces by gwsWriteBounded() (GuardedSocket.h): the watchdog is
      fed between pieces, and a client that accepts no byte for GWS_SEND_STALL_MS (3 s), or the
      whole response taking GWS_SEND_TOTAL_MS (10 s), drops the connection.
   6. Trimmed to what the gateway uses: plain function pointers instead of std::function, a fixed
      route table and fixed argument/header tables (no heap use for them), exact-path routes only;
      removed: Digest/Basic authentication helpers, middleware, static file serving, chunked
      responses, SSE, ETag/CORS, upload (multipart) handlers, Uri classes, MIME table.
   7. Renamed to GuardedWebServer so it cannot clash with the core class; the core WebServer
      library is excluded in platformio.ini (lib_ignore).
*/

#ifndef GUARDED_WEBSERVER_H
#define GUARDED_WEBSERVER_H

#include <Arduino.h>
#include "Network.h"
#include "http_parser.h"
#include "GuardedSocket.h"

typedef enum http_method HTTPMethod;
#ifndef HTTP_ANY
#define HTTP_ANY (HTTPMethod)(255)
#endif

// Time and size limits (override with -D in build_flags). Everything that can block is bounded
// by these, and all the times are below the 5 s loop task watchdog.
#ifndef GWS_REQUEST_BUDGET_MS
#define GWS_REQUEST_BUDGET_MS 3000   // request line + headers + non-raw body, from the first byte
#endif
#ifndef GWS_RAW_PIECE_MS
#define GWS_RAW_PIECE_MS 3000        // one raw upload piece (GWS_RAW_BUFLEN bytes)
#endif
#ifndef GWS_SEND_STALL_MS
#define GWS_SEND_STALL_MS 3000       // a response write without any progress
#endif
#ifndef GWS_SEND_TOTAL_MS
#define GWS_SEND_TOTAL_MS 10000      // one whole response
#endif
#ifndef GWS_ERROR_WRITE_MS
#define GWS_ERROR_WRITE_MS 300       // the short error reply of a dropped request
#endif
#ifndef GWS_MAX_DATA_WAIT_MS
#define GWS_MAX_DATA_WAIT_MS 5000    // a connected client that sends nothing is closed after this (never blocks)
#endif
#ifndef GWS_MAX_LINE_LEN
#define GWS_MAX_LINE_LEN 4096        // request line / header line
#endif
#ifndef GWS_MAX_URI_LEN
#define GWS_MAX_URI_LEN 2048
#endif
#ifndef GWS_MAX_BODY
#define GWS_MAX_BODY 2048            // plain POST body (the largest, /api/save, is about 1 kB)
#endif
#ifndef GWS_RAW_BUFLEN
#define GWS_RAW_BUFLEN 1436
#endif
#define GWS_MAX_ROUTES 24
#define GWS_MAX_HEADERS 8            // collected request headers (Authorization is always one)
#define GWS_MAX_ARGS 8               // query/form arguments, incl. "plain" for a plain body
#define GWS_MAX_RESP_HEADERS 6

enum HTTPRawStatus {
    RAW_START,
    RAW_WRITE,
    RAW_END,
    RAW_ABORTED
};

typedef struct {
    HTTPRawStatus status;
    size_t totalSize;     // bytes of the body handed over so far
    size_t currentSize;   // size of the data currently in buf
    uint8_t buf[GWS_RAW_BUFLEN];
} HTTPRaw;

class GuardedWebServer {
public:
    typedef void (*THandlerFunction)(void);

    explicit GuardedWebServer(int port = 80);

    void begin();
    void handleClient();

    /** Exact-path route. A request for another method gets the not-found handler. */
    void on(const char* uri, HTTPMethod method, THandlerFunction fn);
    /** Route whose request body is streamed to rawFn() in RAW_START / RAW_WRITE... / RAW_END (or
     *  RAW_ABORTED) pieces, then fn() is called. Exempt from the request deadline, see header. */
    void onRawUpload(const char* uri, HTTPMethod method, THandlerFunction fn, THandlerFunction rawFn);
    void onNotFound(THandlerFunction fn);

    NetworkClient& client() { return _currentClient; }
    HTTPRaw& raw() { return _raw; }
    bool hasRaw() const { return _rawActive; }

    String arg(const String& name) const;
    bool hasArg(const String& name) const;
    void collectHeaders(const char* const headerKeys[], size_t headerKeysCount);
    String header(const String& name) const;
    String hostHeader() const { return _hostHeader; }
    int clientContentLength() const { return _clientContentLength; }

    void sendHeader(const String& name, const String& value, bool first = false);
    void send(int code, const char* contentType = nullptr, const String& content = String(""));
    void send(int code, const char* contentType, const char* content);
    void send_P(int code, const char* contentType, const char* content);
    void send_P(int code, const char* contentType, const char* content, size_t contentLength);

private:
    enum LineStatus { LINE_OK, LINE_TOO_LONG, LINE_TIMEOUT };
    struct Route {
        const char* uri;
        HTTPMethod method;
        THandlerFunction fn;
        THandlerFunction rawFn;
    };
    struct KeyValue {
        String key;
        String value;
    };

    bool _parseRequest();
    void _handleRequest();
    bool _readRaw();
    LineStatus _readLine(String& line, size_t maxLength);
    bool _readBody(String& out, size_t length);
    size_t _readPiece(uint8_t* buf, size_t want);
    bool _expired() const;
    void _resetRequest();
    void _collectHeader(const String& name, const String& value);
    void _parseArguments(const String& data, uint8_t maxArgs);
    void _addArg(const String& key, const String& value);
    void _addRoute(const char* uri, HTTPMethod method, THandlerFunction fn, THandlerFunction rawFn);
    void _error(const char* status);
    void _sendResponse(int code, const char* contentType, const char* body, size_t length);
    void _clearResponseHeaders();

    NetworkServer _server;
    NetworkClient _currentClient;
    bool _haveClient = false;
    unsigned long _statusChange = 0;

    Route _routes[GWS_MAX_ROUTES];
    uint8_t _routeCount = 0;
    const Route* _route = nullptr;
    THandlerFunction _notFound = nullptr;

    unsigned long _reqStart = 0;
    unsigned long _budgetMs = 0;   // 0 = no request deadline (raw upload)
    HTTPMethod _currentMethod = HTTP_ANY;
    String _currentUri;
    String _hostHeader;
    int _clientContentLength = 0;
    KeyValue _headers[GWS_MAX_HEADERS];
    uint8_t _headerCount = 0;
    KeyValue _args[GWS_MAX_ARGS];
    uint8_t _argCount = 0;
    KeyValue _respHeaders[GWS_MAX_RESP_HEADERS];
    uint8_t _respHeaderCount = 0;
    HTTPRaw _raw;
    bool _rawActive = false;
};

#endif  // GUARDED_WEBSERVER_H
