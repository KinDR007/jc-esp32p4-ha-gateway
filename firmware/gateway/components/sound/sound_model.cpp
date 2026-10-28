#include "sound_model.h"

#include <cmath>
#include <cstring>

#include "dl_model_base.hpp"
#include "esp_log.h"
#include "esp_partition.h"
#include "sound_mel.h"

static const char *TAG = "sound_model";
static const char *PARTITION = "model";

static dl::Model *s_model;
static dl::TensorBase *s_input, *s_logits, *s_embedding;

uint32_t sound_model_stored_size(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION);
    uint32_t header[4];
    if (!part || esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK || memcmp(header, "EDL2", 4) != 0 ||
        header[2] + 16 > part->size) {
        return 0;
    }
    return header[2] + 16;
}

esp_err_t sound_model_load(void)
{
    if (s_model) {
        return ESP_OK;
    }
    if (!sound_model_stored_size()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Parameters are copied to PSRAM: faster than reading them through the flash cache. */
    s_model = new dl::Model(PARTITION, fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0, dl::MEMORY_MANAGER_GREEDY, nullptr,
                            true);
    if (!s_model) {
        return ESP_ERR_NO_MEM;
    }
    s_input = s_model->get_input();
    for (auto &out : s_model->get_outputs()) {
        int n = 1;
        for (int d : out.second->shape) {
            n *= d;
        }
        if (n == SOUND_CLASSES) {
            s_logits = out.second;
        } else if (n == SOUND_EMBEDDING) {
            s_embedding = out.second;
        }
    }
    int in_size = 1;
    for (int d : s_input->shape) {
        in_size *= d;
    }
    if (!s_logits || in_size != MEL_PATCH_FRAMES * MEL_BANDS || s_input->dtype != dl::DATA_TYPE_INT8) {
        ESP_LOGE(TAG, "unexpected model I/O (input %d values, dtype %d)", in_size, s_input->dtype);
        delete s_model;
        s_model = nullptr;
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "model loaded, input exponent %d", s_input->get_exponent());
    return ESP_OK;
}

bool sound_model_ready(void)
{
    return s_model != nullptr;
}

static void dequantize(dl::TensorBase *t, float *out, int n)
{
    float scale = ldexpf(1.0f, t->get_exponent());
    if (t->dtype == dl::DATA_TYPE_INT8) {
        const int8_t *q = static_cast<const int8_t *>(t->data);
        for (int i = 0; i < n; i++) {
            out[i] = q[i] * scale;
        }
    } else if (t->dtype == dl::DATA_TYPE_INT16) {
        const int16_t *q = static_cast<const int16_t *>(t->data);
        for (int i = 0; i < n; i++) {
            out[i] = q[i] * scale;
        }
    } else {
        memcpy(out, t->data, n * sizeof(float));
    }
}

esp_err_t sound_model_run(const float *patch, float *scores, float *embedding)
{
    if (!s_model) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Input layout [1, 96, 64, 1] (NHWC) equals the row-major patch. */
    float inv_scale = ldexpf(1.0f, -s_input->get_exponent());
    int8_t *in = static_cast<int8_t *>(s_input->data);
    for (int i = 0; i < MEL_PATCH_FRAMES * MEL_BANDS; i++) {
        float v = roundf(patch[i] * inv_scale);
        in[i] = static_cast<int8_t>(v > 127.0f ? 127.0f : v < -128.0f ? -128.0f : v);
    }
    s_model->run();
    dequantize(s_logits, scores, SOUND_CLASSES);
    for (int i = 0; i < SOUND_CLASSES; i++) {
        scores[i] = 1.0f / (1.0f + expf(-scores[i]));
    }
    if (embedding && s_embedding) {
        dequantize(s_embedding, embedding, SOUND_EMBEDDING);
    }
    return ESP_OK;
}
