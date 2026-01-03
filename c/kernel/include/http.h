/*
 * TinyOS HTTP Client
 */
#ifndef HTTP_H
#define HTTP_H

#include "types.h"
#include "net.h"

/* HTTP methods */
#define HTTP_GET     0
#define HTTP_POST    1
#define HTTP_PUT     2
#define HTTP_DELETE  3

/* HTTP status */
#define HTTP_STATE_IDLE       0
#define HTTP_STATE_DNS        1  /* DNS resolution */
#define HTTP_STATE_CONNECTING 2
#define HTTP_STATE_SENDING    3
#define HTTP_STATE_HEADERS    4
#define HTTP_STATE_BODY       5
#define HTTP_STATE_DONE       6
#define HTTP_STATE_ERROR      7

/* Maximum sizes */
#define HTTP_MAX_HOST    64
#define HTTP_MAX_PATH    128
#define HTTP_MAX_HEADERS 512
#define HTTP_MAX_BODY    4096

/* Parsed URL */
typedef struct {
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    uint16_t port;
    int is_https;
} http_url_t;

/* HTTP response */
typedef struct {
    int status_code;
    char headers[HTTP_MAX_HEADERS];
    char body[HTTP_MAX_BODY];
    int body_len;
    int content_length;
    int chunked;
} http_response_t;

/* HTTP request handle */
typedef struct {
    int state;
    int tcp_conn;
    int method;
    http_url_t url;
    http_response_t response;
    int header_complete;
    char* body_start;
    dns_query_t dns_query;  /* For async DNS resolution */
    uint8_t resolved_ip[4]; /* Resolved IP address */
} http_request_t;

/* Initialize HTTP client */
void http_init(void);

/* Parse a URL */
int http_parse_url(const char* url, http_url_t* parsed);

/* Start an HTTP request (non-blocking) */
int http_request_start(http_request_t* req, int method, const char* url,
                       const char* body, int body_len);

/* Poll HTTP request (call repeatedly until done) */
int http_request_poll(http_request_t* req);

/* Get request state */
int http_get_state(http_request_t* req);

/* Close/cleanup request */
void http_request_close(http_request_t* req);

/* Simple blocking GET request */
int http_get(const char* url, http_response_t* response);

/* Simple blocking POST request */
int http_post(const char* url, const char* body, int body_len,
              http_response_t* response);

/* DNS resolution (simple - returns gateway for now) */
int http_resolve_host(const char* host, uint8_t* ip_out);

#endif
