#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "usb.h"
#include "usblink.h"

struct packet {
    uint16_t constant;
    struct { uint16_t addr, service; } src;
    struct { uint16_t addr, service; } dst;
    uint16_t data_check;
    uint8_t data_size;
    uint8_t ack;
    uint8_t seqno;
    uint8_t hdr_check;
    uint8_t data[255];
};

#define CONSTANT  BSWAP16(0x54FD)
#define SRC_ADDR  BSWAP16(0x6400)
#define DST_ADDR  BSWAP16(0x6401)

uint16_t usblink_data_checksum(struct packet *packet) {
    uint16_t check = 0;
    int i, size = packet->data_size;
    for (i = 0; i < size; i++) {
        uint16_t tmp = check << 12 ^ check << 8;
        check = (packet->data[i] << 8 | check >> 8)
                ^ tmp ^ tmp >> 5 ^ tmp >> 12;
    }
    return BSWAP16(check);
}

uint8_t usblink_header_checksum(struct packet *packet) {
    uint8_t check = 0;
    int i;
    for (i = 0; i < 15; i++) check += ((uint8_t *)packet)[i];
    return check;
}

static void dump_packet(char *type, void *data, uint32_t size) {
    if (log_enabled[LOG_USB]) {
        uint32_t i;
        logprintf(LOG_USB, "%s", type);
        for (i = 0; i < size && i < 24; i++)
            logprintf(LOG_USB, " %02x", ((uint8_t *)data)[i]);
        if (size > 24)
            logprintf(LOG_USB, "...");
        logprintf(LOG_USB, "\n");
    }
}

struct packet usblink_send_buffer;
void usblink_send_packet() {
    extern void usblink_start_send();
    usblink_send_buffer.constant   = CONSTANT;
    usblink_send_buffer.src.addr   = SRC_ADDR;
    usblink_send_buffer.dst.addr   = DST_ADDR;
    usblink_send_buffer.data_check = usblink_data_checksum(&usblink_send_buffer);
    usblink_send_buffer.hdr_check  = usblink_header_checksum(&usblink_send_buffer);
    dump_packet("send", &usblink_send_buffer, 16 + usblink_send_buffer.data_size);
    usblink_start_send();
}

uint8_t prev_seqno;
uint8_t next_seqno() {
    prev_seqno = (prev_seqno == 0xFF) ? 0x01 : prev_seqno + 1;
    return prev_seqno;
}

FILE *put_file;
uint32_t put_file_size;
uint16_t put_file_port;
enum {
    SENDING_03         = 1,
    RECVING_04         = 2,
    ACKING_04_or_FF_00 = 3,
    SENDING_05         = 4,
    RECVING_FF_00      = 5,
    DONE               = 6,
    EXPECT_FF_00       = 16, // Sent to us after first OS data packet
} put_file_state;
void put_file_next(struct packet *in) {
    struct packet *out = &usblink_send_buffer;
    switch (put_file_state & 15) {
        case SENDING_03:
            if (!in || in->ack != 0x0A) goto fail;
            put_file_state++;
            break;
        case RECVING_04:
            if (!in || in->data_size != 1 || in->data[0] != 0x04) {
                emuprintf("File send error: Didn't get 04\n");
                goto fail;
            }
            put_file_state++;
            break;
        case ACKING_04_or_FF_00:
            if (in) goto fail;
            put_file_state++;
send_data:
            if (prev_seqno == 1)
            {
                gui_status_printf("Sending file: %u bytes left", put_file_size);
                throttle_timer_off();
            }
            if (put_file_size > 0) {
                /* Send data (05) */
                uint32_t len = put_file_size;
                if (len > 253)
                    len = 253;
                put_file_size -= len;
                out->src.service = BSWAP16(0x8001);
                out->dst.service = put_file_port;
                out->data_size = 1 + len;
                out->ack = 0;
                out->seqno = next_seqno();
                out->data[0] = 0x05;
                fread(&out->data[1], 1, len, put_file);
                usblink_send_packet();
                break;
            }
            gui_status_printf("Send complete");
            throttle_timer_on();
            put_file_state = DONE;
            break;
        case SENDING_05:
            if (!in || in->ack != 0x0A) goto fail;
            if (put_file_state & EXPECT_FF_00) {
                put_file_state++;
                break;
            }
            goto send_data;
        case RECVING_FF_00: /* Got FF 00: OS header is valid */
            if (!in || in->data_size != 2 || in->data[0] != 0xFF || in->data[1]) {
                emuprintf("File send error: Didn't get FF 00\n");
                goto fail;
            }
            put_file_state = ACKING_04_or_FF_00;
            break;
fail:
            emuprintf("Send failed\n");
            usblink_connected = false;
            gui_usblink_changed(false);
        case DONE:
            put_file_state = 0;
            fclose(put_file);
            put_file = NULL;
            break;
    }
}

void usblink_sent_packet() {
    if (usblink_send_buffer.ack) {
        /* Received packet has been acked */
        uint16_t service = usblink_send_buffer.dst.service;
        if (service == BSWAP16(0x4060) || service == BSWAP16(0x4080))
            put_file_next(NULL);
    }
}

void usblink_received_packet(uint8_t *data, uint32_t size) {
    dump_packet("recv", data, size);
    struct packet *in = (struct packet *)data;
    struct packet *out = &usblink_send_buffer;

    if (in->dst.service == BSWAP16(0x8001))
        put_file_next(in);

    if (in->src.service == BSWAP16(0x4003)) { /* Address request */
        gui_status_printf("usblink connected.");
        usblink_connected = true;
        gui_usblink_changed(true);
        out->src.service = BSWAP16(0x4003);
        out->dst.service = BSWAP16(0x4003);
        out->data_size = 4;
        out->ack = 0;
        out->seqno = 1;
        uint16_t tmp = DST_ADDR;
        memcpy(&out->data[0], &tmp, sizeof(tmp)); // *(uint16_t *)&out->data[0] = DST_ADDR;
        tmp = BSWAP16(0xFF00);
        memcpy(&out->data[2], &tmp, sizeof(tmp)); // *(uint16_t *)&out->data[2] = BSWAP16(0xFF00);
        usblink_send_packet();
    } else if (!in->ack) {
        /* Send an ACK */
        out->src.service = BSWAP16(0x00FF);
        out->dst.service = in->src.service;
        out->data_size = 2;
        out->ack = 0x0A;
        out->seqno = in->seqno;
        memcpy(&out->data[0], &in->dst.service, sizeof(in->dst.service)); // *(uint16_t *)&out->data[0] = in->dst.service;
        usblink_send_packet();
    }
}

bool usblink_put_file(const char *filepath, const char *folder) {
    const char *filename = filepath;
    const char *p;
    for (p = filepath; *p; p++)
        if (*p == ':' || *p == '/' || *p == '\\')
            filename = p + 1;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        gui_perror(filepath);
        return 0;
    }
    if (put_file)
        fclose(put_file);
    put_file = f;
    fseek(f, 0, SEEK_END);
    put_file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    put_file_state = 1;

    /* Send the first packet */
    struct packet *out = &usblink_send_buffer;
    out->src.service = BSWAP16(0x8001);
    out->dst.service = put_file_port = BSWAP16(0x4060);
    out->ack = 0;
    out->seqno = next_seqno();
    uint8_t *data = out->data;
    *data++ = 3;
    *data++ = 1;
    data += sprintf((char *)data, "/%s/%s", folder, filename) + 1;
    *(uint32_t *)data = BSWAP32(put_file_size); data += 4;
    out->data_size = data - out->data;
    usblink_send_packet();
    return 1;
}

void usblink_send_os(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f)
        return;
    if (put_file)
        fclose(put_file);
    put_file = f;
    fseek(f, 0, SEEK_END);
    put_file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    put_file_state = 1 | 16;

    /* Send the first packet */
    struct packet *out = &usblink_send_buffer;
    out->src.service = BSWAP16(0x8001);
    out->dst.service = put_file_port = BSWAP16(0x4080);
    out->ack = 0;
    out->seqno = next_seqno();
    uint8_t *data = out->data;
    *data++ = 3;
    *(uint32_t *)data = BSWAP32(put_file_size); data += 4;
    out->data_size = data - out->data;
    usblink_send_packet();
}

bool usblink_sending, usblink_connected = false;
int usblink_state;

extern void usb_bus_reset_on(void);
extern void usb_bus_reset_off(void);
extern void usb_receive_setup_packet(int endpoint, void *packet);
extern void usb_receive_packet(int endpoint, void *packet, uint32_t size);

struct usb_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

void usblink_reset() {
    if (put_file_state) {
        put_file_state = 0;
        fclose(put_file);
        put_file = NULL;
    }
    usblink_connected = false;
    gui_usblink_changed(usblink_connected);
    usblink_state = 0;
    usblink_sending = false;
}

void usblink_connect() {
    prev_seqno = 0;
    usblink_state = 1;
}

// no easy way to tell when it's ok to turn bus reset off,
// (putting the device into the default state) so do it on a timer :/
void usblink_timer() {
    switch (usblink_state) {
        case 1:
            usb_bus_reset_on();
            usblink_state++;
            break;
        case 2:
            usb_bus_reset_off();
            //printf("Sending SET_ADDRESS\n");
            struct usb_setup packet = { 0, 5, 1, 0, 0 };
            usb_receive_setup_packet(0, &packet);
            usblink_state++;
            break;
    }
}

void usblink_receive(int ep, void *buf, uint32_t size) {
    //printf("usblink_receive(%d,%p,%d)\n", ep, buf, size);
    if (ep == 0) {
        if (usblink_state == 3) {
            //printf("Sent SET_ADDRESS, sending SET_CONFIGURATION\n");
            struct usb_setup packet = { 0, 9, 1, 0, 0 };
            usb_receive_setup_packet(0, &packet);
            usblink_state = 0;
        }
    } else {
        if (size >= 16)
            usblink_received_packet(buf, size);
    }
}

void usblink_complete_send(int ep) {
    if (ep != 0 && usblink_sending) {
        uint32_t size = 16 + usblink_send_buffer.data_size;
        usb_receive_packet(ep, &usblink_send_buffer, size);
        usblink_sending = false;
        //printf("send complete\n");
        usblink_sent_packet();
    }
}

void usblink_start_send() {
    int ep;
    usblink_sending = true;
    // If there's already an endpoint waiting for data, just send immediately
    for (ep = 1; ep < 4; ep++) {
        if (usb.epsr & (1 << ep)) {
            usblink_complete_send(ep);
            return;
        }
    }
}
