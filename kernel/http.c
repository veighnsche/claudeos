/*
 * TinyOS HTTP Client Implementation
 */

#include "http.h"
#include "tcp.h"
#include "net.h"
#include "memory.h"

/* String utilities */
static int str_len(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int str_ncmp(const char* a, const char* b, int n) {
    while (n > 0 && *a && *b && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return *a - *b;
}

static void str_cpy(char* dst, const char* src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

void http_init(void) {
    tcp_init();
}

int http_parse_url(const char* url, http_url_t* parsed) {
    memset(parsed, 0, sizeof(http_url_t));
    parsed->port = 80;
    parsed->is_https = 0;
    str_cpy(parsed->path, "/", HTTP_MAX_PATH);

    const char* p = url;

    /* Check scheme */
    if (str_ncmp(p, "https://", 8) == 0) {
        parsed->is_https = 1;
        parsed->port = 443;
        p += 8;
    } else if (str_ncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    /* Parse host */
    int host_len = 0;
    const char* host_start = p;
    while (*p && *p != ':' && *p != '/' && host_len < HTTP_MAX_HOST - 1) {
        parsed->host[host_len++] = *p++;
    }
    parsed->host[host_len] = 0;

    if (host_len == 0) {
        return -1;  /* No host */
    }

    /* Parse port if present */
    if (*p == ':') {
        p++;
        parsed->port = 0;
        while (*p >= '0' && *p <= '9') {
            parsed->port = parsed->port * 10 + (*p - '0');
            p++;
        }
    }

    /* Parse path */
    if (*p == '/') {
        str_cpy(parsed->path, p, HTTP_MAX_PATH);
    }

    return 0;
}

/* Check if host is an IP address, returns 1 if yes and fills ip_out */
static int is_ip_address(const char* host, uint8_t* ip_out) {
    int dots = 0;
    const char* p = host;
    while (*p) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return 0;
        p++;
    }

    if (dots != 3) return 0;

    /* Parse IP address */
    p = host;
    for (int i = 0; i < 4; i++) {
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        ip_out[i] = val;
        if (*p == '.') p++;
    }
    return 1;
}

/* Legacy resolve - now just checks for IP format */
int http_resolve_host(const char* host, uint8_t* ip_out) {
    if (is_ip_address(host, ip_out)) {
        return 0;
    }
    return -1;  /* Need async DNS */
}

/* Build HTTP request string */
static int build_request(http_request_t* req, char* buf, int buf_size,
                         const char* body, int body_len) {
    int pos = 0;
    const char* method_str = "GET";
    if (req->method == HTTP_POST) method_str = "POST";
    else if (req->method == HTTP_PUT) method_str = "PUT";
    else if (req->method == HTTP_DELETE) method_str = "DELETE";

    /* Request line */
    while (*method_str && pos < buf_size - 1) buf[pos++] = *method_str++;
    buf[pos++] = ' ';
    const char* path = req->url.path;
    while (*path && pos < buf_size - 1) buf[pos++] = *path++;
    buf[pos++] = ' ';
    const char* ver = "HTTP/1.1\r\n";
    while (*ver && pos < buf_size - 1) buf[pos++] = *ver++;

    /* Host header */
    const char* h = "Host: ";
    while (*h && pos < buf_size - 1) buf[pos++] = *h++;
    h = req->url.host;
    while (*h && pos < buf_size - 1) buf[pos++] = *h++;
    buf[pos++] = '\r'; buf[pos++] = '\n';

    /* User-Agent */
    h = "User-Agent: TinyOS/1.0\r\n";
    while (*h && pos < buf_size - 1) buf[pos++] = *h++;

    /* Connection */
    h = "Connection: close\r\n";
    while (*h && pos < buf_size - 1) buf[pos++] = *h++;

    /* Content-Length for POST/PUT */
    if (body && body_len > 0) {
        h = "Content-Type: text/plain\r\n";
        while (*h && pos < buf_size - 1) buf[pos++] = *h++;

        h = "Content-Length: ";
        while (*h && pos < buf_size - 1) buf[pos++] = *h++;

        /* Write length */
        char len_buf[12];
        int n = body_len;
        int i = 11;
        len_buf[i--] = 0;
        do {
            len_buf[i--] = '0' + (n % 10);
            n /= 10;
        } while (n > 0 && i >= 0);
        h = &len_buf[i + 1];
        while (*h && pos < buf_size - 1) buf[pos++] = *h++;
        buf[pos++] = '\r'; buf[pos++] = '\n';
    }

    /* End of headers */
    buf[pos++] = '\r'; buf[pos++] = '\n';

    /* Body */
    if (body && body_len > 0) {
        for (int i = 0; i < body_len && pos < buf_size - 1; i++) {
            buf[pos++] = body[i];
        }
    }

    buf[pos] = 0;
    return pos;
}

/* Parse HTTP response headers */
static int parse_response(http_request_t* req, const char* data, int len) {
    http_response_t* resp = &req->response;

    /* Find end of headers */
    const char* header_end = NULL;
    for (int i = 0; i < len - 3; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            header_end = data + i + 4;
            break;
        }
    }

    if (!header_end) {
        return 0;  /* Headers not complete */
    }

    req->header_complete = 1;
    req->body_start = (char*)header_end;

    /* Parse status line */
    const char* p = data;
    /* Skip "HTTP/1.x " */
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    /* Parse status code */
    resp->status_code = 0;
    while (*p >= '0' && *p <= '9') {
        resp->status_code = resp->status_code * 10 + (*p - '0');
        p++;
    }

    /* Copy headers */
    int hdr_len = header_end - data - 4;
    if (hdr_len > HTTP_MAX_HEADERS - 1) hdr_len = HTTP_MAX_HEADERS - 1;
    memcpy(resp->headers, data, hdr_len);
    resp->headers[hdr_len] = 0;

    /* Parse Content-Length */
    resp->content_length = -1;
    resp->chunked = 0;
    p = resp->headers;
    while (*p) {
        /* Find header name */
        const char* line_start = p;
        while (*p && *p != ':' && *p != '\r') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; if (*p) p++; continue; }

        int name_len = p - line_start;
        p++;  /* Skip ':' */
        while (*p == ' ') p++;  /* Skip whitespace */

        const char* value_start = p;
        while (*p && *p != '\r') p++;

        /* Check for Content-Length */
        if (name_len == 14) {
            int match = 1;
            const char* cl = "content-length";
            for (int i = 0; i < 14; i++) {
                if (to_lower(line_start[i]) != cl[i]) { match = 0; break; }
            }
            if (match) {
                resp->content_length = 0;
                const char* v = value_start;
                while (*v >= '0' && *v <= '9') {
                    resp->content_length = resp->content_length * 10 + (*v - '0');
                    v++;
                }
            }
        }

        /* Check for Transfer-Encoding: chunked */
        if (name_len == 17) {
            int match = 1;
            const char* te = "transfer-encoding";
            for (int i = 0; i < 17; i++) {
                if (to_lower(line_start[i]) != te[i]) { match = 0; break; }
            }
            if (match && str_ncmp(value_start, "chunked", 7) == 0) {
                resp->chunked = 1;
            }
        }

        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    return 1;
}

int http_request_start(http_request_t* req, int method, const char* url,
                       const char* body, int body_len) {
    memset(req, 0, sizeof(http_request_t));
    req->state = HTTP_STATE_IDLE;
    req->method = method;
    req->tcp_conn = -1;
    req->response.content_length = -1;  /* -1 means unknown */

    if (http_parse_url(url, &req->url) != 0) {
        req->state = HTTP_STATE_ERROR;
        return -1;
    }

    if (req->url.is_https) {
        req->state = HTTP_STATE_ERROR;
        return -1;
    }

    /* Check if host is IP address */
    if (is_ip_address(req->url.host, req->resolved_ip)) {
        /* Already have IP, go straight to connecting */
        req->tcp_conn = tcp_connect(req->resolved_ip, req->url.port);
        if (req->tcp_conn < 0) {
            req->state = HTTP_STATE_ERROR;
            return -1;
        }
        req->state = HTTP_STATE_CONNECTING;
    } else {
        /* Need DNS resolution */
        dns_resolve_start(&req->dns_query, req->url.host);
        req->state = HTTP_STATE_DNS;
    }

    /* Store body info for later (simplified - just use body/body_len params) */
    (void)body;
    (void)body_len;

    return 0;
}

int http_request_poll(http_request_t* req) {
    if (req->state == HTTP_STATE_DONE || req->state == HTTP_STATE_ERROR) {
        return req->state;
    }

    /* Handle DNS state */
    if (req->state == HTTP_STATE_DNS) {
        int dns_state = dns_resolve_poll(&req->dns_query);
        if (dns_state == DNS_STATE_DONE) {
            /* Got IP, start TCP connection */
            memcpy(req->resolved_ip, req->dns_query.result_ip, 4);
            req->tcp_conn = tcp_connect(req->resolved_ip, req->url.port);
            if (req->tcp_conn < 0) {
                req->state = HTTP_STATE_ERROR;
            } else {
                req->state = HTTP_STATE_CONNECTING;
            }
        } else if (dns_state == DNS_STATE_ERROR) {
            req->state = HTTP_STATE_ERROR;
        }
        return req->state;
    }

    int tcp_state = tcp_get_state(req->tcp_conn);

    switch (req->state) {
        case HTTP_STATE_CONNECTING:
            if (tcp_state == TCP_ESTABLISHED) {
                /* Build and send request */
                char request_buf[1024];
                int req_len = build_request(req, request_buf, sizeof(request_buf),
                                            NULL, 0);
                tcp_send(req->tcp_conn, request_buf, req_len);
                req->state = HTTP_STATE_HEADERS;
            } else if (tcp_state == TCP_CLOSED) {
                req->state = HTTP_STATE_ERROR;
            }
            break;

        case HTTP_STATE_HEADERS:
        case HTTP_STATE_BODY:
            if (tcp_data_available(req->tcp_conn)) {
                char buf[1024];
                int len = tcp_recv(req->tcp_conn, buf, sizeof(buf));

                if (!req->header_complete) {
                    /* Accumulate in body until headers complete */
                    int space = HTTP_MAX_BODY - req->response.body_len;
                    int to_copy = len;
                    if (to_copy > space) to_copy = space;
                    memcpy(req->response.body + req->response.body_len, buf, to_copy);
                    req->response.body_len += to_copy;
                    req->response.body[req->response.body_len] = 0;

                    if (parse_response(req, req->response.body, req->response.body_len)) {
                        /* Headers complete, move body data */
                        int body_offset = req->body_start - req->response.body;
                        int body_len = req->response.body_len - body_offset;
                        memmove(req->response.body, req->body_start, body_len);
                        req->response.body_len = body_len;
                        req->response.body[body_len] = 0;
                        req->state = HTTP_STATE_BODY;
                    }
                } else {
                    /* Append body data */
                    int space = HTTP_MAX_BODY - req->response.body_len - 1;
                    int to_copy = len;
                    if (to_copy > space) to_copy = space;
                    memcpy(req->response.body + req->response.body_len, buf, to_copy);
                    req->response.body_len += to_copy;
                    req->response.body[req->response.body_len] = 0;
                }
            }

            /* Check if done */
            if (tcp_state == TCP_CLOSED || tcp_state == TCP_CLOSE_WAIT) {
                req->state = HTTP_STATE_DONE;
            } else if (req->response.content_length >= 0 &&
                       req->response.body_len >= req->response.content_length) {
                req->state = HTTP_STATE_DONE;
                tcp_close(req->tcp_conn);
            }
            break;

        default:
            break;
    }

    return req->state;
}

int http_get_state(http_request_t* req) {
    return req->state;
}

void http_request_close(http_request_t* req) {
    if (req->tcp_conn >= 0) {
        tcp_close(req->tcp_conn);
        req->tcp_conn = -1;
    }
    req->state = HTTP_STATE_IDLE;
}

/* Blocking HTTP GET */
int http_get(const char* url, http_response_t* response) {
    http_request_t req;

    if (http_request_start(&req, HTTP_GET, url, NULL, 0) != 0) {
        return -1;
    }

    /* Poll until done (with timeout) */
    int timeout = 50000;
    while (timeout > 0) {
        tcp_poll();
        net_poll();

        int state = http_request_poll(&req);
        if (state == HTTP_STATE_DONE) {
            memcpy(response, &req.response, sizeof(http_response_t));
            http_request_close(&req);
            return 0;
        } else if (state == HTTP_STATE_ERROR) {
            http_request_close(&req);
            return -1;
        }

        /* Small delay */
        for (volatile int i = 0; i < 1000; i++);
        timeout--;
    }

    http_request_close(&req);
    return -1;  /* Timeout */
}

/* Blocking HTTP POST */
int http_post(const char* url, const char* body, int body_len,
              http_response_t* response) {
    http_request_t req;

    if (http_request_start(&req, HTTP_POST, url, body, body_len) != 0) {
        return -1;
    }

    /* Poll until done */
    int timeout = 50000;
    while (timeout > 0) {
        tcp_poll();
        net_poll();

        int state = http_request_poll(&req);
        if (state == HTTP_STATE_DONE) {
            memcpy(response, &req.response, sizeof(http_response_t));
            http_request_close(&req);
            return 0;
        } else if (state == HTTP_STATE_ERROR) {
            http_request_close(&req);
            return -1;
        }

        for (volatile int i = 0; i < 1000; i++);
        timeout--;
    }

    http_request_close(&req);
    return -1;
}
