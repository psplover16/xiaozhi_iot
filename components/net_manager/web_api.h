#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 註冊 /status 與 /api/control 控制路由 */
esp_err_t web_api_register_routes(httpd_handle_t httpd);

/* 共用靜態資源輸出（自動附帶 Content-Type 與 Cache-Control 標頭） */
esp_err_t web_api_send_static(httpd_req_t *req,
                              const uint8_t *start,
                              const uint8_t *end,
                              const char *content_type,
                              const char *cache_ctrl);

/* 共用 HTTP / JSON 輔助工具 */
void web_api_json_escape(char *dst, size_t dst_len, const char *src);
bool web_api_recv_post_body(httpd_req_t *req, char *out_body, size_t buf_len);
bool web_api_extract_form_field(const char *body, const char *key,
                                char *out_val, size_t out_len);

#ifdef __cplusplus
}
#endif