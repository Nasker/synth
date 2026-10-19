#include "usb_midi.h"
#include "synth.h"

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// USB-MIDI Code Index Numbers (low nibble of packet byte 0)
#define CIN_NOTE_OFF     0x8
#define CIN_NOTE_ON      0x9
#define CIN_CONTROL      0xB
#define CIN_PITCH_BEND   0xE

#define MIDI_XFER_SIZE   256
#define PITCH_BEND_RANGE 2.0f   // semitones

static const char *TAG = "usb_midi";

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    usb_transfer_t          *xfer;
    int                      intf_num;
    int                      alt_setting;
    volatile bool            seen;
} s = {0};

static void parse_midi_packet(const uint8_t *p)
{
    uint8_t cin = p[0] & 0x0F;
    uint8_t d1  = p[2];
    uint8_t d2  = p[3];

    switch (cin) {
    case CIN_NOTE_ON:
        if (d2 > 0) synth_note_on(d1, d2 / 127.f);
        else        synth_note_off(d1);
        break;
    case CIN_NOTE_OFF:
        synth_note_off(d1);
        break;
    case CIN_CONTROL: {
        float v = d2 / 127.f;
        if      (d1 == 1)  synth_params()->lfo_depth = v;   // mod wheel -> filter LFO
        else if (d1 == 7)  synth_params()->master    = v;   // channel volume
        else if (d1 == 71) synth_params()->reso      = v;   // "sound controller 2"
        else if (d1 == 74) synth_params()->cutoff    = v;   // brightness
        break;
    }
    case CIN_PITCH_BEND: {
        int bend = (d2 << 7) | d1;   // 14-bit, 8192 = center
        synth_pitch_bend((bend - 8192) / 8192.f * PITCH_BEND_RANGE);
        break;
    }
    default:
        break;
    }
}

static void in_xfer_cb(usb_transfer_t *xfer)
{
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes >= 4) {
        s.seen = true;
        const uint8_t *data = xfer->data_buffer;
        for (int i = 0; i + 4 <= xfer->actual_num_bytes; i += 4) {
            if (data[i] != 0) parse_midi_packet(&data[i]);
        }
        usb_host_transfer_submit(xfer);
    }
    // Any other status (abort on disconnect, stall, ...) just drops the transfer
}

static void try_open_device(uint8_t dev_addr)
{
    if (usb_host_device_open(s.client, dev_addr, &s.dev) != ESP_OK) {
        ESP_LOGW(TAG, "device open failed");
        return;
    }

    // Scan for a MIDI streaming interface: class 0x01 (audio), subclass 0x03
    const usb_config_desc_t *cfg;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(s.dev, &cfg));

    int offset = 0;
    int cur_intf = -1, cur_alt = -1;
    bool in_midi = false;
    uint8_t ep_in = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *id = (const usb_intf_desc_t *)d;
            cur_intf = id->bInterfaceNumber;
            cur_alt  = id->bAlternateSetting;
            in_midi  = (id->bInterfaceClass == 0x01 && id->bInterfaceSubClass == 0x03);
        } else if (in_midi && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ed = (const usb_ep_desc_t *)d;
            if (ed->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                ep_in = ed->bEndpointAddress;
                break;
            }
        }
    }

    if (!ep_in) {
        ESP_LOGI(TAG, "not a MIDI device");
        usb_host_device_close(s.client, s.dev);
        s.dev = NULL;
        return;
    }

    s.intf_num = cur_intf;
    s.alt_setting = cur_alt;
    ESP_ERROR_CHECK(usb_host_interface_claim(s.client, s.dev, s.intf_num, s.alt_setting));

    ESP_ERROR_CHECK(usb_host_transfer_alloc(MIDI_XFER_SIZE, 0, &s.xfer));
    s.xfer->device_handle    = s.dev;
    s.xfer->bEndpointAddress = ep_in;
    s.xfer->callback         = in_xfer_cb;
    s.xfer->num_bytes        = MIDI_XFER_SIZE;
    ESP_ERROR_CHECK(usb_host_transfer_submit(s.xfer));

    ESP_LOGI(TAG, "MIDI device ready: intf %d alt %d, IN ep 0x%02x", s.intf_num, s.alt_setting, ep_in);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "device connected, addr %d", msg->new_dev.address);
        try_open_device(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "device gone");
        if (s.dev) {
            usb_host_interface_release(s.client, s.dev, s.intf_num);
            usb_host_device_close(s.client, s.dev);
            s.dev = NULL;
        }
        if (s.xfer) {
            usb_host_transfer_free(s.xfer);
            s.xfer = NULL;
        }
        break;
    default:
        break;
    }
}

static void host_lib_task(void *arg)
{
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void client_task(void *arg)
{
    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &s.client));
    while (1) {
        usb_host_client_handle_events(s.client, portMAX_DELAY);
    }
}

void usb_midi_start(void)
{
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    xTaskCreatePinnedToCore(host_lib_task, "usb_lib", 3072, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, NULL, 10, NULL, 0);

    ESP_LOGI(TAG, "USB host installed, waiting for MIDI device...");
}

bool usb_midi_seen_activity(void)
{
    return s.seen;
}
