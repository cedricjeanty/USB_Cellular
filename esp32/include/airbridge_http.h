#pragma once
// AirBridge HTTP helpers — response reading and S3 API calls via HAL network
// Extracted from main.cpp for testing.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_proto.h"
#include <cstdio>
#include <cstring>
#include <string>

// Read an HTTP response from TLS connection: parse headers, extract body.
// Handles chunked transfer encoding. Optionally captures ETag header.
inline std::string halHttpReadResponse(TlsHandle tls, char* etag = nullptr, size_t etagSz = 0) {
    if (!g_hal || !g_hal->network) return "";
    bool chunked = false;
    char linebuf[512];

    int statusCode = 0;
    // Read headers line by line
    while (true) {
        int pos = 0;
        while (pos < (int)sizeof(linebuf) - 1) {
            char c;
            int r = g_hal->network->read(tls, &c, 1);
            if (r <= 0) goto done_headers;
            linebuf[pos++] = c;
            if (c == '\n') break;
        }
        linebuf[pos] = '\0';

        // Parse HTTP status code from first line
        if (statusCode == 0 && strncmp(linebuf, "HTTP/", 5) == 0) {
            const char* sp = strchr(linebuf, ' ');
            if (sp) statusCode = atoi(sp + 1);
        }

        if (strstr(linebuf, "chunked")) chunked = true;

        // Capture ETag header
        if (etag && etagSz > 0 && strncasecmp(linebuf, "ETag:", 5) == 0) {
            char* val = linebuf + 5;
            while (*val == ' ') val++;
            char* end = val + strlen(val) - 1;
            while (end > val && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = '\0';
            if (val[0] == '"') {
                val++;
                char* eq = strrchr(val, '"');
                if (eq) *eq = '\0';
            }
            strlcpy(etag, val, etagSz);
        }

        if (pos <= 2 && (linebuf[0] == '\r' || linebuf[0] == '\n')) break;
    }
done_headers:
    (void)statusCode;

    // Read body
    std::string raw;
    char rbuf[1024];
    while (true) {
        int r = g_hal->network->read(tls, rbuf, sizeof(rbuf));
        if (r > 0) raw.append(rbuf, r);
        else break;
    }

    if (!chunked) return raw;
    return dechunk(raw);
}

// Build an S3 API GET request string
inline std::string buildApiGetRequest(const char* host, const char* apiKey, const char* queryParams) {
    char req[512];
    snprintf(req, sizeof(req),
        "GET /prod/presign?%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Connection: close\r\n\r\n",
        queryParams, host, apiKey);
    return std::string(req);
}

// Build an S3 API POST (complete) request string
inline std::string buildApiCompleteRequest(const char* host, const char* apiKey,
                                            const char* uploadId, const char* key,
                                            const char* partsJson) {
    char body[2048];
    snprintf(body, sizeof(body),
        "{\"upload_id\":\"%s\",\"key\":\"%s\",\"parts\":[%s]}",
        uploadId, key, partsJson);
    int bodyLen = strlen(body);

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "POST /prod/complete HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        host, apiKey, bodyLen);

    return std::string(hdr) + body;
}

// Perform a full S3 API GET: connect, send request, read response, parse JSON.
inline std::string s3ApiGetViaHal(const char* host, const char* apiKey, const char* queryParams) {
    if (!g_hal || !g_hal->network) return "";
    TlsHandle tls = g_hal->network->connect(host);
    if (!tls) return "";
    std::string req = buildApiGetRequest(host, apiKey, queryParams);
    if (!g_hal->network->write(tls, req.c_str(), req.size())) {
        g_hal->network->destroy(tls);
        return "";
    }
    std::string resp = halHttpReadResponse(tls);
    g_hal->network->destroy(tls);
    return resp;
}
