#pragma once
/* Consume log arguments so host -Werror catches real unused code. */
void mock_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
