// Simulierter Task-Watchdog: loest bei Ablauf einen simulierten Neustart aus
// (Prozess startet sich neu, Reset-Grund ESP_RST_TASK_WDT).
#pragma once
#include "Arduino.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;

esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool panic);
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* cfg);
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* cfg);
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_delete(TaskHandle_t task);
esp_err_t esp_task_wdt_reset();
esp_err_t esp_task_wdt_deinit();
