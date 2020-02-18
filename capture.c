#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <gmodule.h>
#include <libsigrok/libsigrok.h>
#include "capture.h"

#define UNUSED(x) (void)(x)


typedef struct state {
    struct sr_context *context;
    struct sr_dev_driver *driver;
    struct sr_dev_inst *device;
    struct sr_session *session;
    unsigned int num_channels;
    uint64_t prev;
    uint64_t idx;
    bool running;
} state_t;

static struct state state;

static const uint64_t SAMPLERATE = 50000000;

#define SR_ERROR_CHECK(x) do {                                          \
        int __err_rc = (x);                                             \
        if (__err_rc != SR_OK) {                                        \
        fprintf(stderr, "\033[1;31m");                                  \
            fprintf(stderr, "Sigrok error %s @ %s:%d.\n",               \
                    sr_strerror_name(__err_rc), __FILE__, __LINE__);    \
        fprintf(stderr, "\033[0m");                                     \
        }                                                               \
        exit(1);                                                        \
    } while(0)


static int assert_sr(int ret, const char *string) {
    if (ret != SR_OK) {
    fprintf(stderr, "\033[1;31m");
        fprintf(stderr, "Error %s (%s): %s.\n", string, sr_strerror_name(ret),
                sr_strerror(ret));
    fprintf(stderr, "\033[0m");
        exit(1);
    }
    return ret;
}


static const char *configkey_tostring(int option) {
    const char *option_name;
    switch (option) {
        case SR_CONF_LOGIC_ANALYZER:
            option_name = "Logic Analyzer"; break;
        case SR_CONF_OSCILLOSCOPE:
            option_name = "Oscilloscope"; break;
        case SR_CONF_CONN:
            option_name = "Connection"; break;
        case SR_CONF_SAMPLERATE:
            option_name = "Sample Rate"; break;
        case SR_CONF_VDIV:
            option_name = "Volts/Div"; break;
        case SR_CONF_COUPLING:
            option_name = "Coupling"; break;
        case SR_CONF_NUM_VDIV:
            option_name = "Number of Vertical Divisions"; break;
        case SR_CONF_LIMIT_MSEC:
            option_name = "Sample Time Limit (ms)"; break;
        case SR_CONF_LIMIT_SAMPLES:
            option_name = "Sample Number Limit"; break;
        case SR_CONF_LIMIT_FRAMES:
            option_name = "Frames Number Limit"; break;
        case SR_CONF_CONTINUOUS:
            option_name = "Continuous"; break;
        default: option_name = NULL;
    }
    return option_name;
}


static void enumerate_device_options(const char *name, struct sr_dev_driver *driver,
        struct sr_dev_inst *dev, struct sr_channel_group *chgroup) {
    GVariant *gvar;
    int res;
    GArray *options_list;

    options_list= sr_dev_options(driver, dev, chgroup);

    if (options_list == NULL) {
        fprintf(stderr, "\033[1;31m");
        fprintf(stderr, "Error getting options list from %s!\n", name);
        fprintf(stderr, "\033[0m");
        exit(1);
    }

    for (guint i = 0; i < options_list->len; i++) {
        uint32_t option = g_array_index(options_list, uint32_t, i);
        const char *option_name = configkey_tostring(option);

        fprintf(stderr, "\033[1;32m");
        if (option_name == NULL) {
            fprintf(stderr, "%s option %u available\n", name, option);
        } else {
            fprintf(stderr, "%s option %u available: %s\n", name, option, option_name);
        }
        fprintf(stderr, "\033[0m");

        res = sr_config_get(driver, dev, chgroup, option, &gvar);
        if (res == SR_OK) {
            gchar *value = g_variant_print(gvar, TRUE);
            fprintf(stderr, "\033[1;32m");
            fprintf(stderr, "value is %s\n", value);
            fprintf(stderr, "\033[0m");
            free(value);
        }

        res = sr_config_list(driver, dev, chgroup, option, &gvar);
        if (res == SR_OK) {
            gchar *value = g_variant_print(gvar, TRUE);
            fprintf(stderr, "\033[1;32m");
            fprintf(stderr, "list values are %s\n", value);
            fprintf(stderr, "\033[0m");
            free(value);
        }

        fprintf(stderr, "\n");
    }
    g_array_free(options_list, TRUE);
}


static struct sr_channel** get_device_channels(struct sr_dev_inst *dev, unsigned int *num) {
    GSList *ch_list;
    struct sr_channel **channels;
    if ((ch_list = sr_dev_inst_channels_get(dev)) == NULL) {
        fprintf(stderr, "Error enumerating channels\n");
        exit(1);
    }

    guint ch_count = g_slist_length(ch_list);
    fprintf(stderr, "\033[1;32m");
    fprintf(stderr, "Found %d channels\n", ch_count);
    fprintf(stderr, "\033[0m");

    channels = malloc((1 + ch_count) * sizeof(struct sr_channel *));
    if (channels == NULL) {
        perror("malloc");
        exit(1);
    }
    for (guint i = 0; i < ch_count && ch_list != NULL; ch_list = ch_list->next) {
        struct sr_channel *channel;
        channel = ch_list->data;
        channels[i++] = channel;
    }
    channels[ch_count] = NULL;
    num[0] = ch_count;
    return channels;
}


static struct sr_dev_driver *get_driver(const char *driver_name,
        struct sr_context *sr_ctx) {
    struct sr_dev_driver** drivers;
    struct sr_dev_driver* driver = NULL;
    drivers = sr_driver_list(sr_ctx);
    if (drivers == NULL) {
        fprintf(stderr, "No drivers found!\n");
        exit(1);
        return NULL;
    }

    for (int i = 0; drivers[i] != NULL; i++) {
        driver = drivers[i];
        if (0 == strcmp(driver->name, driver_name)) {
            int r = sr_driver_init(sr_ctx, driver);
            if (r != SR_OK) {
                fprintf(stderr, "Error initializing driver!\n");
                exit(1);
                return NULL;
            }
            
            return driver;
        }
    }
    fprintf(stderr, "%s driver not found!\n", driver_name);
    exit(1);
    return NULL;
}


static struct sr_dev_inst *get_device(struct sr_dev_driver *driver) {
    GSList* dev_list = sr_driver_scan(driver, NULL);
    if (dev_list == NULL) {
        fprintf(stderr, "No devices found\n");
        exit(1);
        return NULL;
    }

    struct sr_dev_inst *dev;
    dev = dev_list->data;

    g_slist_free(dev_list);
    return dev;
}


static void on_session_stopped(void *data) {
    struct state *s = data;
    UNUSED(s);
    fprintf(stderr, "\033[1;36m");
    fprintf(stderr, "session stopped\n");
    fprintf(stderr, "\033[0m");
}


#define ON_LOGIC_FRAME(FUNCTION_NAME, DATA_T)                                 \
static void FUNCTION_NAME(struct state *s, const DATA_T *data,                \
        uint64_t length, DATA_T mask) {                                       \
    uint64_t diffs = 0;                                                       \
    DATA_T prev = (DATA_T) s->prev;                                           \
    uint64_t idx = s->idx;                                                    \
    for (uint64_t i = 1; i < length / 2; i++) {                               \
        DATA_T unit = data[i] & mask;                                         \
        idx++;                                                                \
        if (unit != prev) {                                                   \
            on_capture_change(idx, prev, unit);                               \
            diffs++;                                                          \
        }                                                                     \
        prev = unit;                                                          \
    }                                                                         \
    s->prev = prev;                                                           \
    s->idx = idx;                                                             \
                                                                              \
    if (diffs != 0) {                                                         \
        fprintf(stderr, "\033[1;34m");                                        \
        fprintf(stderr, "--- diffs: %lu.\n", diffs);                          \
        fprintf(stderr, "\033[0m");                                           \
    }                                                                         \
                                                                              \
}                                                                             \

ON_LOGIC_FRAME(on_logic_frame_8, uint8_t);
ON_LOGIC_FRAME(on_logic_frame_16, uint16_t);
ON_LOGIC_FRAME(on_logic_frame_32, uint32_t);

static void on_session_datafeed(const struct sr_dev_inst *dev,
                         const struct sr_datafeed_packet *packet, void *data) {
    UNUSED(dev);
    uint16_t type = packet->type;
    struct state *s = data;

    switch (type) {

        case SR_DF_HEADER: {
            const struct sr_datafeed_header *payload;
            payload = packet->payload;
            fprintf(stderr, "\033[1;33m");
            fprintf(stderr, "Received datafeed header.\n");
            fprintf(stderr, "\033[0m");
            UNUSED(payload);
        } break;

        case SR_DF_ANALOG: {
            const struct sr_datafeed_analog *payload;
            payload = packet->payload;
            double *payload_data = payload->data;
            UNUSED(payload_data);
            unsigned int payload_count = payload->num_samples;
            fprintf(stderr, "\033[1;33m");
            fprintf(stderr, "Received %d analog samples.\n", payload_count);
            fprintf(stderr, "\033[0m");
        } break;

        case SR_DF_LOGIC: {
            const struct sr_datafeed_logic *payload;
            payload = packet->payload;
            uint64_t length = payload->length;
            uint16_t unitsize = payload->unitsize;
            if (unitsize == 1) {
                uint8_t *data = (uint8_t *) payload->data;
                on_logic_frame_8(s, data, length, 0xff);
            } else if (unitsize == 2) {
                uint16_t *data = (uint16_t *) payload->data;
                on_logic_frame_16(s, data, length, 0xffff);
            } else if (unitsize == 4) {
                uint32_t *data = (uint32_t *) payload->data;
                on_logic_frame_32(s, data, length, 0xffffffff);
            } else {
                fprintf(stderr, "\033[1;33m");
                fprintf(stderr, "Received datafeed size %u.\n", unitsize);
                fprintf(stderr, "\033[0m");
            }

        } break;

        case SR_DF_END: {
            fprintf(stderr, "\033[1;33m");
            fprintf(stderr, "Received datafeed end.\n");
        } break;

        default:
            fprintf(stderr, "unknown datafeed type %d\n", type);

    }
}


bool capture_stop() {
    struct state *s = &state;
    if (s->running) {
        s->running = false;
        fprintf(stderr, "Trying to shut down session cleanly...\n");
        assert_sr(sr_session_stop(s->session), "stopping session");
        return true;
    } else {
        return false;
    }
}


void capture_init() {
    int ret;
    struct sr_channel **channels;
    struct state *s = &state;
    GVariant *gvar;

    s->context = NULL;
    s->session = NULL;
    s->driver = NULL;
    s->device = NULL;

    assert_sr(sr_init(&s->context), "initializing libsigrok");

    //s->driver = get_driver("fx2lafw", s->context);
    s->driver = get_driver("saleae-logic-pro", s->context);
    enumerate_device_options("Driver", s->driver, NULL, NULL);

    s->device = get_device(s->driver);
    enumerate_device_options("Device", s->driver, s->device, NULL);
    assert_sr(sr_dev_open(s->device), "opening device");

    channels = get_device_channels(s->device, &s->num_channels);
    for (unsigned int i = 0; i < s->num_channels; i++) {
        assert_sr(sr_dev_channel_enable(channels[i], true), "enabling channel");
    }

    gvar = g_variant_new_uint64(SAMPLERATE);
    ret = sr_config_set(s->device, NULL, SR_CONF_SAMPLERATE, gvar);
    assert_sr(ret, "setting samplerate");
    g_variant_unref(gvar);

    assert_sr(sr_session_new(s->context, &s->session), "creating session");
    assert_sr(sr_session_dev_add(s->session, s->device),
            "adding device to session");

    ret = sr_session_datafeed_callback_add(s->session, on_session_datafeed, s);
    assert_sr(ret, "adding callback for session datafeed");

    ret = sr_session_stopped_callback_set(s->session, on_session_stopped, s);
    assert_sr(ret, "setting callback for session stopped");

    s->running = false;
}


void capture_run() {
    struct state *s = &state;

    s->running = true;
    fprintf(stderr, "Session starting.\n");
    assert_sr(sr_session_start(s->session), "starting session");
    assert_sr(sr_session_run(s->session), "running session");
    s->running = false;
    fprintf(stderr, "Sigrok session finished.\n");
}


void capture_cleanup() {
    struct state *s = &state;

    fprintf(stderr, "Sigrok shutting down...\n");
    assert_sr(sr_session_destroy(s->session), "destroying session");
    assert_sr(sr_dev_close(s->device), "closing device");
    assert_sr(sr_exit(s->context), "shutting down libsigrok");
    fprintf(stderr, "Sigrok successfully closed\n");
}


