/*
 * SwanStationUWP - minimal HTTPS client on WinHTTP (the same three calls as net.h's libcurl version, which the PS5
 * build used; a UWP package has no libcurl and the system already carries the certificates).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../net.h"

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char *data;
    size_t length, capacity;
} Buffer;

static bool buffer_add(Buffer *b, const void *data, size_t n)
{
    if (b->length + n + 1 > b->capacity)
    {
        size_t cap = (b->length + n + 1) * 2;
        char *grown = (char *)realloc(b->data, cap);
        if (!grown)
            return false;
        b->data = grown;
        b->capacity = cap;
    }
    memcpy(b->data + b->length, data, n);
    b->length += n;
    b->data[b->length] = '\0';
    return true;
}

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0)
    {
        free(w);
        w = NULL;
    }
    return w;
}

/* One request. The body goes to `sink` (a file) or `buf` (memory). Returns the HTTP status, or -1 when none was got. */
static int http_do(const char *url, const char *post_data, const char *content_type, const char *user_agent, FILE *sink,
                   Buffer *buf)
{
    int status = -1;
    wchar_t *wurl = widen(url), *wua = widen(user_agent ? user_agent : SwanStationPS5_NAME "/" SwanStationPS5_VERSION);
    HINTERNET session = NULL, connect = NULL, request = NULL;
    if (!wurl || !wua)
        goto done;

    wchar_t host[256], path[2048];
    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts))
        goto done;
    /* the query string is part of the path to ask for */
    if (parts.dwExtraInfoLength && parts.lpszExtraInfo)
    {
        size_t used = wcslen(path);
        if (used + parts.dwExtraInfoLength + 1 >= 2048)
            goto done;
        memcpy(path + used, parts.lpszExtraInfo, parts.dwExtraInfoLength * sizeof(wchar_t));
        path[used + parts.dwExtraInfoLength] = 0;
    }

    session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!session)
        goto done;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 25000);
    connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect)
        goto done;
    request = WinHttpOpenRequest(connect, post_data ? L"POST" : L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
        goto done;

    wchar_t *header = NULL;
    if (post_data && content_type)
    {
        char line[200];
        snprintf(line, sizeof(line), "Content-Type: %s", content_type);
        header = widen(line);
    }
    DWORD post_length = post_data ? (DWORD)strlen(post_data) : 0;
    BOOL sent = WinHttpSendRequest(request, header ? header : WINHTTP_NO_ADDITIONAL_HEADERS, header ? (DWORD)-1 : 0,
                                   (LPVOID)post_data, post_length, post_length, 0);
    free(header);
    if (!sent || !WinHttpReceiveResponse(request, NULL))
        goto done;

    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                             &size, WINHTTP_NO_HEADER_INDEX))
        goto done;
    status = (int)code;
    for (;;)
    {
        char chunk[16384];
        DWORD got = 0;
        if (!WinHttpReadData(request, chunk, sizeof(chunk), &got))
        {
            status = -1;
            break;
        }
        if (!got)
            break;
        if (sink ? fwrite(chunk, 1, got, sink) != got : !buffer_add(buf, chunk, got))
        {
            status = -1;
            break;
        }
    }

done:
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);
    free(wurl);
    free(wua);
    return status;
}

bool net_available(void)
{
    return true;
}

NetResult net_download(const char *url, const char *dest)
{
    char part[SwanStationPS5_PATH_MAX];
    snprintf(part, sizeof(part), "%s.part", dest);
    FILE *f = fopen(part, "wb");
    if (!f)
        return NET_UNAVAILABLE;
    int status = http_do(url, NULL, NULL, NULL, f, NULL);
    bool written = fclose(f) == 0;
    if (status == 200 && written && rename(part, dest) == 0)
        return NET_OK;
    remove(part);
    if (status == 404)
        return NET_NOT_FOUND;
    SwanStationPS5_log("net: %s -> http %d", url, status);
    return status > 0 ? NET_NOT_FOUND : NET_UNAVAILABLE;
}

int net_request(const char *url, const char *post_data, const char *content_type, const char *user_agent, char **body,
                size_t *body_length)
{
    Buffer b = {0};
    *body = NULL;
    *body_length = 0;
    int status = http_do(url, post_data, content_type, user_agent, NULL, &b);
    if (status < 0)
    {
        SwanStationPS5_log("net: %s -> failed", url);
        free(b.data);
        return -1;
    }
    *body = b.data;
    *body_length = b.length;
    return status;
}
