/*
 * Tab5-Meshtastic v2 — persistent settings implementation.
 *
 * Keeps an in-memory cache loaded at init and writes through to NVS on change.
 * One NVS namespace; keys: "saved" (blob array) + "saved_n", "active" (blob) +
 * "active_ok".
 */

#include "settings.h"

#include <string.h>

#include "nvs.h"
#include "esp_log.h"

namespace {

const char* TAG = "settings";
const char* NS  = "tab5mesh";

nvs_handle_t   s_h          = 0;
saved_device_t s_saved[SETTINGS_MAX_SAVED];
uint32_t       s_saved_n    = 0;
saved_device_t s_active;
bool           s_active_ok  = false;

bool addr_eq(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

void persist(void)
{
    if (!s_h) return;
    nvs_set_blob(s_h, "saved", s_saved, s_saved_n * sizeof(saved_device_t));
    nvs_set_u32(s_h, "saved_n", s_saved_n);
    nvs_set_u8(s_h, "active_ok", s_active_ok ? 1 : 0);
    if (s_active_ok) nvs_set_blob(s_h, "active", &s_active, sizeof(s_active));
    esp_err_t err = nvs_commit(s_h);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs_commit: %s", esp_err_to_name(err));
}

}  // namespace

void settings_init(void)
{
    if (s_h) return;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &s_h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return; }

    uint32_t n = 0;
    if (nvs_get_u32(s_h, "saved_n", &n) == ESP_OK && n <= SETTINGS_MAX_SAVED) {
        size_t len = n * sizeof(saved_device_t);
        if (len && nvs_get_blob(s_h, "saved", s_saved, &len) == ESP_OK)
            s_saved_n = n;
    }

    uint8_t ok = 0;
    if (nvs_get_u8(s_h, "active_ok", &ok) == ESP_OK && ok) {
        size_t len = sizeof(s_active);
        if (nvs_get_blob(s_h, "active", &s_active, &len) == ESP_OK)
            s_active_ok = true;
    }
    ESP_LOGI(TAG, "loaded: %lu saved, active=%s",
             (unsigned long)s_saved_n, s_active_ok ? s_active.name : "(none)");
}

bool settings_get_active(saved_device_t* out)
{
    if (!s_active_ok) return false;
    *out = s_active;
    return true;
}

void settings_set_active(const saved_device_t* dev)
{
    if (s_active_ok && memcmp(&s_active, dev, sizeof(*dev)) == 0) return;  /* unchanged */

    s_active    = *dev;
    s_active_ok = true;

    /* upsert into the saved list */
    for (uint32_t i = 0; i < s_saved_n; i++) {
        if (addr_eq(s_saved[i].addr, dev->addr)) { s_saved[i] = *dev; persist(); return; }
    }
    if (s_saved_n < SETTINGS_MAX_SAVED) s_saved[s_saved_n++] = *dev;
    persist();
}

uint32_t settings_get_saved(saved_device_t* out, uint32_t max)
{
    uint32_t n = s_saved_n < max ? s_saved_n : max;
    memcpy(out, s_saved, n * sizeof(saved_device_t));
    return n;
}

bool settings_is_saved(const uint8_t addr[6], char* pin_out)
{
    for (uint32_t i = 0; i < s_saved_n; i++) {
        if (addr_eq(s_saved[i].addr, addr)) {
            if (pin_out) strcpy(pin_out, s_saved[i].pin);
            return true;
        }
    }
    return false;
}

void settings_forget(const uint8_t addr[6])
{
    for (uint32_t i = 0; i < s_saved_n; i++) {
        if (addr_eq(s_saved[i].addr, addr)) {
            for (uint32_t j = i; j + 1 < s_saved_n; j++) s_saved[j] = s_saved[j + 1];
            s_saved_n--;
            break;
        }
    }
    if (s_active_ok && addr_eq(s_active.addr, addr)) s_active_ok = false;
    persist();
}
