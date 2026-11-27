/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_random.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "esp_blufi_api.h"
#include "blufi.h"

#include <aes/esp_aes.h>
#include "blufi/legacy_dh.h"
#include "system/md5.h"
#include "esp_crc.h"

/*
   The SEC_TYPE_xxx is for self-defined packet data type in the procedure of "BLUFI negotiate key"
   If user use other negotiation procedure to exchange(or generate) key, should redefine the type by yourself.
 */
#define SEC_TYPE_DH_PARAM_LEN   0x00
#define SEC_TYPE_DH_PARAM_DATA  0x01
#define SEC_TYPE_DH_P           0x02
#define SEC_TYPE_DH_G           0x03
#define SEC_TYPE_DH_PUBLIC      0x04


struct blufi_security {
#define DH_SELF_PUB_KEY_LEN     128
    uint8_t  self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN           128
    uint8_t  share_key[SHARE_KEY_LEN];
    size_t   share_len;
#define PSK_LEN                 16
    uint8_t  psk[PSK_LEN];
    uint8_t  *dh_param;
    int      dh_param_len;
    uint8_t  iv[16];
    blufi_crypto::DhContext dhm;
    esp_aes_context aes;
};
static struct blufi_security *blufi_sec;

static int myrand( void *rng_state, unsigned char *output, size_t len )
{
    esp_fill_random(output, len);
    return( 0 );
}

extern "C" void btc_blufi_report_error(esp_blufi_error_state_t state);

void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free)
{
    if (output_data == nullptr || output_len == nullptr || need_free == nullptr) return;
    *output_data = nullptr;
    *output_len = 0;
    *need_free = false;
    if (data == NULL || len < 3) {
        BLUFI_ERROR("BLUFI Invalid data format");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    int ret;
    uint8_t type = data[0];

    if (blufi_sec == NULL) {
        BLUFI_ERROR("BLUFI Security is not initialized");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    switch (type) {
    case SEC_TYPE_DH_PARAM_LEN:
        blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);
        if (blufi_sec->dh_param) {
            free(blufi_sec->dh_param);
            blufi_sec->dh_param = NULL;
        }
        if (blufi_sec->dh_param_len <= 0 || blufi_sec->dh_param_len > 390) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        blufi_sec->dh_param = (uint8_t *)malloc(blufi_sec->dh_param_len);
        if (blufi_sec->dh_param == NULL) {
            blufi_sec->dh_param_len = 0;  /* Reset length to avoid using unallocated memory */
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            BLUFI_ERROR("%s, malloc failed\n", __func__);
            return;
        }
        break;
    case SEC_TYPE_DH_PARAM_DATA:{
        if (blufi_sec->dh_param == NULL) {
            BLUFI_ERROR("%s, blufi_sec->dh_param == NULL\n", __func__);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        if (len < (blufi_sec->dh_param_len + 1)) {
            BLUFI_ERROR("%s, invalid dh param len\n", __func__);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        uint8_t *param = blufi_sec->dh_param;
        memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len);
        ret = blufi_crypto::ReadParams(&blufi_sec->dhm, &param, &param[blufi_sec->dh_param_len]);
        if (ret) {
            BLUFI_ERROR("%s read param failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
            return;
        }
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;

        const int dhm_len = blufi_crypto::GetLen(&blufi_sec->dhm);

        if (dhm_len > DH_SELF_PUB_KEY_LEN) {
            BLUFI_ERROR("%s dhm len not support %d\n", __func__, dhm_len);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        ret = blufi_crypto::MakePublic(&blufi_sec->dhm, dhm_len, blufi_sec->self_public_key, DH_SELF_PUB_KEY_LEN, myrand, NULL);
        if (ret) {
            BLUFI_ERROR("%s make public failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
            return;
        }

        ret = blufi_crypto::CalcSecret( &blufi_sec->dhm,
                blufi_sec->share_key,
                SHARE_KEY_LEN,
                &blufi_sec->share_len,
                myrand, NULL);
        if (ret) {
            BLUFI_ERROR("%s blufi_crypto::CalcSecret failed %d\n", __func__, ret);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        Md5::Digest(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);

        esp_aes_setkey(&blufi_sec->aes, blufi_sec->psk, PSK_LEN * 8);

        /* alloc output data */
        *output_data = &blufi_sec->self_public_key[0];
        *output_len = dhm_len;
        *need_free = false;

    }
        break;
    case SEC_TYPE_DH_P:
        break;
    case SEC_TYPE_DH_G:
        break;
    case SEC_TYPE_DH_PUBLIC:
        break;
    }
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   /* set iv8 as the iv0[0] */

    ret = esp_aes_crypt_cfb128(&blufi_sec->aes, ESP_AES_ENCRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    if (ret) {
        return -1;
    }

    return crypt_len;
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   /* set iv8 as the iv0[0] */

    ret = esp_aes_crypt_cfb128(&blufi_sec->aes, ESP_AES_DECRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    if (ret) {
        return -1;
    }

    return crypt_len;
}

uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len)
{
    /* This iv8 ignore, not used */
    return esp_crc16_be(0, data, len);
}

esp_err_t blufi_security_init(void)
{
    blufi_sec = new (std::nothrow) blufi_security{};
    if (blufi_sec == NULL) {
        return ESP_FAIL;
    }

    esp_aes_init(&blufi_sec->aes);

    memset(blufi_sec->iv, 0x0, sizeof(blufi_sec->iv));
    return 0;
}

void blufi_security_deinit(void)
{
    if (blufi_sec == NULL) {
        return;
    }
    if (blufi_sec->dh_param){
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;
    }

    esp_aes_free(&blufi_sec->aes);
    memset(blufi_sec->psk, 0, sizeof(blufi_sec->psk));
    memset(blufi_sec->share_key, 0, sizeof(blufi_sec->share_key));

    delete blufi_sec;
    blufi_sec = NULL;
}
