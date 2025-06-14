/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2025 The Playdroid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include <cros_gralloc/cros_gralloc_handle.h>
#include <gralloc_handle.h>

#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>

#include "playsocket.h"

const char *SOCKET_PATH = "/run/playdroid_socket";

enum {
    GRALLOC_ANDROID,
    GRALLOC_GBM,
    GRALLOC_CROS,
    GRALLOC_DEFAULT
};

struct playdroid_hwc_composer_device_1 {
    hwc_composer_device_1_t base; // constant after init
    const hwc_procs_t *procs;     // constant after init
    int32_t vsync_period_ns;      // constant after init
    int gtype;

    int sock;
    int width;
    int height;
    int refresh;
    struct MessageData message;

    pthread_mutex_t vsync_lock;
    bool vsync_callback_enabled; // protected by this->vsync_lock
};

static int hwc_prepare(hwc_composer_device_1_t* dev,
                       size_t numDisplays, hwc_display_contents_1_t** displays) {
    struct playdroid_hwc_composer_device_1 *pdev = (struct playdroid_hwc_composer_device_1 *)dev;

    if (!numDisplays || !displays) return 0;

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];

    if (!contents) return 0;

    if (displays && (contents->flags & HWC_GEOMETRY_CHANGED)) {
        for (size_t i = 0; i < contents->numHwLayers; i++) {
            if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
                continue;
            if (contents->hwLayers[i].flags & HWC_SKIP_LAYER)
                continue;

            if (contents->hwLayers[i].compositionType == HWC_OVERLAY)
                contents->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
        }
    }

    return 0;
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
    struct playdroid_hwc_composer_device_1* pdev = (struct playdroid_hwc_composer_device_1*)dev;

    if (!numDisplays || !displays) {
        return 0;
    }

    hwc_display_contents_1_t *contents = displays[HWC_DISPLAY_PRIMARY];
    hwc_layer_1_t *fb_target_layer;

    for (size_t l = 0; l < contents->numHwLayers; l++) {
        size_t layer = l;
        hwc_layer_1_t *fb_layer = &contents->hwLayers[layer];

        if (fb_layer->compositionType != HWC_FRAMEBUFFER_TARGET) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (!fb_layer->handle) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        fb_target_layer = fb_layer;
    }

    if (pdev->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *)fb_target_layer->handle;
        pdev->message.type = MSG_HAVE_BUFFER;
        pdev->message.format = drm_handle->format;
        pdev->message.modifiers = drm_handle->modifier;
        pdev->message.stride = drm_handle->stride;
        pdev->message.offset = 0; // offset is not used in this case

        send_message(pdev->sock, drm_handle->prime_fd, MSG_TYPE_FD, &pdev->message);
    } else if (pdev->gtype == GRALLOC_CROS) {
        const struct cros_gralloc_handle *cros_handle = (const struct cros_gralloc_handle *)fb_target_layer->handle;
        pdev->message.type = MSG_HAVE_BUFFER;
        pdev->message.format = cros_handle->format;
        pdev->message.modifiers = cros_handle->format_modifier;
        pdev->message.stride = cros_handle->strides[0];
        pdev->message.offset = cros_handle->offsets[0];

        send_message(pdev->sock, cros_handle->fds[0], MSG_TYPE_FD, &pdev->message);
    }

    return 0;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value) {
    struct playdroid_hwc_composer_device_1* pdev =
            (struct playdroid_hwc_composer_device_1*)dev;

    switch (what) {
        case HWC_VSYNC_PERIOD:
            value[0] = pdev->vsync_period_ns;
            break;
        default:
            // unsupported query
            ALOGE("%s badness unsupported query what=%d", __FUNCTION__, what);
            return -EINVAL;
    }
    return 0;
}

static int hwc_event_control(struct hwc_composer_device_1* dev, int dpy __unused,
                             int event, int enabled) {
    struct playdroid_hwc_composer_device_1* pdev =
            (struct playdroid_hwc_composer_device_1*)dev;
    int ret = -EINVAL;

    // enabled can only be 0 or 1
    if (!(enabled & ~1)) {
        if (event == HWC_EVENT_VSYNC) {
            pthread_mutex_lock(&pdev->vsync_lock);
            pdev->vsync_callback_enabled = enabled;
            pthread_mutex_unlock(&pdev->vsync_lock);
            ret = 0;
        }
    }
    return ret;
}

static int hwc_blank(struct hwc_composer_device_1* dev __unused, int disp __unused,
                     int blank __unused) {
    return 0;
}

static void hwc_dump(hwc_composer_device_1* dev __unused, char* buff __unused,
                     int buff_len __unused) {
    // This is run when running dumpsys.
    // No-op for now.
}


static int hwc_get_display_configs(struct hwc_composer_device_1* dev __unused,
                                   int disp, uint32_t* configs, size_t* numConfigs) {
    if (*numConfigs == 0) {
        return 0;
    }

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    return -EINVAL;
}


static int32_t hwc_attribute(struct playdroid_hwc_composer_device_1* pdev,
                             const uint32_t attribute) {
    char property[PROPERTY_VALUE_MAX];
    int density = 180;

    switch(attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            return pdev->vsync_period_ns;
        case HWC_DISPLAY_WIDTH: {
            return pdev->width;
        }
        case HWC_DISPLAY_HEIGHT: {
            return pdev->height;
        }
        case HWC_DISPLAY_DPI_X:
        case HWC_DISPLAY_DPI_Y:
            if (property_get("ro.sf.lcd_density", property, nullptr) > 0)
                density = atoi(property);
            return density * 1000;
        case HWC_DISPLAY_COLOR_TRANSFORM:
            return HAL_COLOR_TRANSFORM_IDENTITY;
        default:
            ALOGE("unknown display attribute %u", attribute);
            return -EINVAL;
    }
}

static int hwc_get_display_attributes(struct hwc_composer_device_1* dev __unused,
                                      int disp, uint32_t config __unused,
                                      const uint32_t* attributes, int32_t* values) {
    struct playdroid_hwc_composer_device_1* pdev = (struct playdroid_hwc_composer_device_1*)dev;
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY) {
            values[i] = hwc_attribute(pdev, attributes[i]);
            if (values[i] == -EINVAL) {
                return -EINVAL;
            }
        } else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

static int hwc_close(hw_device_t* dev) {
    struct playdroid_hwc_composer_device_1* pdev = (struct playdroid_hwc_composer_device_1*)dev;

    if (pdev->sock >= 0) {
        close(pdev->sock);
    }
    pthread_mutex_destroy(&pdev->vsync_lock);
    
    delete dev;
    return 0;
}

int get_gralloc_type(const char *gralloc) {
    if (strcmp(gralloc, "default") == 0) {
        return GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GRALLOC_GBM;
    } else if (str_starts_with(gralloc, "minigbm_") == 0) {
        return GRALLOC_CROS;
    } else {
        return GRALLOC_ANDROID;
    }
}

static void hwc_register_procs(struct hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
    struct playdroid_hwc_composer_device_1* pdev = (struct playdroid_hwc_composer_device_1*)dev;
    pdev->procs = procs;
}

static int hwc_open(const struct hw_module_t* module, const char* name,
                    struct hw_device_t** device) {
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("%s called with bad name %s", __FUNCTION__, name);
        return -EINVAL;
    }

    playdroid_hwc_composer_device_1 *pdev = new playdroid_hwc_composer_device_1();
    if (!pdev) {
        ALOGE("%s failed to allocate dev", __FUNCTION__);
        return -ENOMEM;
    }

    pdev->base.common.tag = HARDWARE_DEVICE_TAG;
    pdev->base.common.version = HWC_DEVICE_API_VERSION_1_1;
    pdev->base.common.module = const_cast<hw_module_t *>(module);
    pdev->base.common.close = hwc_close;

    pdev->base.prepare = hwc_prepare;
    pdev->base.set = hwc_set;
    pdev->base.eventControl = hwc_event_control;
    pdev->base.blank = hwc_blank;
    pdev->base.query = hwc_query;
    pdev->base.registerProcs = hwc_register_procs;
    pdev->base.dump = hwc_dump;
    pdev->base.getDisplayConfigs = hwc_get_display_configs;
    pdev->base.getDisplayAttributes = hwc_get_display_attributes;

    pdev->vsync_period_ns = 1000*1000*1000/60; // vsync is 60 hz

    pthread_mutex_init(&pdev->vsync_lock, NULL);
    pdev->vsync_callback_enabled = true;

    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        pdev->gtype = get_gralloc_type(property);
    }

    pdev->sock = connect_socket(SOCKET_PATH);

    pdev->message.type = MSG_HELLO;
    ret = send_message(pdev->sock, -1, MSG_TYPE_DATA, &pdev->message);

    pdev->message.type = MSG_ASK_FOR_RESOLUTION;
    ret = send_message(pdev->sock, -1, MSG_TYPE_DATA_NEEDS_REPLY, &pdev->message);

    MessageType type;
    ret = recv_message(pdev->sock, NULL, &pdev->message, &type);
    if (type != MSG_TYPE_DATA_REPLY || pdev->message.type != MSG_HAVE_RESOLUTION) {
        ALOGE("Expected resolution reply, got type %d, message type %d",
             type, pdev->message.type);
        return -EINVAL;
    }
    if (ret < 0) {
        ALOGE("Failed to receive resolution reply: %s", strerror(errno));
        return ret;
    }
    pdev->width = pdev->message.width;
    pdev->height = pdev->message.height;
    pdev->refresh = pdev->message.refresh_rate;
    if (pdev->width <= 0 || pdev->height <= 0 || pdev->refresh <= 0) {
        ALOGE("Invalid resolution received: %dx%d @ %dHz",
             pdev->width, pdev->height, pdev->refresh);
        return -EINVAL;
    }
    ALOGI("Received resolution: %dx%d @ %dHz", pdev->width, pdev->height, pdev->refresh);

    if (pdev->refresh > 1000 && pdev->refresh < 1000000)
        pdev->vsync_period_ns = 1000 * 1000 * 1000 / (pdev->refresh / 1000);

    *device = &pdev->base.common;

    return ret;
}

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "playdroid hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
    }
};
