// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

#include "bt4.h"

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

// Time between advertising events. Each message stays up for 100 ms, so it
// goes out about twice on each of the three advertising channels.
#define ADV_INTERVAL_MS 50

static const char *TAG = "bt4";

static volatile bool s_synced;
static uint8_t s_own_addr_type;

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no Bluetooth address available");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "could not determine the address type");
        return;
    }
    s_synced = true;
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "Bluetooth stack reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); // returns only if the stack is stopped
    nimble_port_freertos_deinit();
}

// dBm to the controller's level index: -24 dBm is 0, each step is 3 dB.
static esp_power_level_t level_for(int dbm)
{
    return (esp_power_level_t)((dbm - BT4_POWER_MIN_DBM) / BT4_POWER_STEP_DB);
}

esp_err_t bt4_init(void)
{
    // The radio keeps its calibration data in NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Lowest power from the outset, before anything could be sent.
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, level_for(BT4_POWER_MIN_DBM));
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, level_for(BT4_POWER_MIN_DBM));

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool bt4_ready(void)
{
    return s_synced;
}

bool bt4_power_allowed(int dbm)
{
    return dbm >= BT4_POWER_MIN_DBM && dbm <= BT4_POWER_MAX_DBM && (dbm - BT4_POWER_MIN_DBM) % BT4_POWER_STEP_DB == 0;
}

esp_err_t bt4_start(int power_dbm, const uint8_t adv[BT4_ADV_LEN])
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bt4_power_allowed(power_dbm)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_power_level_t level = level_for(power_dbm);
    ESP_RETURN_ON_ERROR(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, level), TAG, "set power");
    // Do not transmit at a power we cannot confirm.
    if (esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV) != level) {
        ESP_LOGE(TAG, "radio did not confirm %d dBm", power_dbm);
        return ESP_FAIL;
    }

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    if (ble_gap_adv_set_data(adv, BT4_ADV_LEN) != 0) {
        return ESP_FAIL;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_NON,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS),
    };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising did not start, code %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising at %d dBm", power_dbm);
    return ESP_OK;
}

esp_err_t bt4_update(const uint8_t adv[BT4_ADV_LEN])
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    return ble_gap_adv_set_data(adv, BT4_ADV_LEN) == 0 ? ESP_OK : ESP_FAIL;
}

void bt4_stop(void)
{
    if (s_synced && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        ESP_LOGI(TAG, "advertising stopped");
    }
}
