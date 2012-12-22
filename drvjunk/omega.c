#include <libusb-1.0/libusb.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <malloc.h>
#include <memory.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

// interface 1 altsetting 1 endpoint 01 (out) bits 16 channels 2 mps 192
// interface 2 altsetting 1 endpoint 83 (in)  bits 16 channels 2 mps 192


static struct libusb_context *usbctx;
static int multimix_timeout = 1000;

int epOUT = 0x01, epIN = 0x83;

void init_usb()
{
    libusb_init(&usbctx);
    libusb_set_debug(usbctx, 3);
}

struct libusb_device_handle *open_multimix()
{
    struct libusb_device_handle *handle;
#if 0
    handle = libusb_open_device_with_vid_pid(usbctx, 0x1210, 0x0002);
    if (!handle)
    {
        printf("Lexicon Omega not found.\n");
        return NULL;
    }
    libusb_reset_device(handle);
    libusb_close(handle);
    sleep(1);
#endif
    handle = libusb_open_device_with_vid_pid(usbctx, 0x1210, 0x0002);
    if (!handle)
    {
        printf("Lexicon Omega not found after reset.\n");
        return NULL;
    }
    if (libusb_set_configuration(handle, 1))
    {
        libusb_close(handle);
        return NULL;
    }
    
    if (libusb_claim_interface(handle, 1))
        goto error;
    if (libusb_claim_interface(handle, 2))
        goto error;
    if (libusb_claim_interface(handle, 4))
        goto error;
    if (libusb_claim_interface(handle, 5))
        goto error;
    if (libusb_claim_interface(handle, 7))
        goto error;
    if (libusb_set_interface_alt_setting(handle, 1, 1))
        goto error;
    if (libusb_set_interface_alt_setting(handle, 2, 1))
        goto error;
    if (libusb_set_interface_alt_setting(handle, 4, 1))
        goto error;
    if (libusb_set_interface_alt_setting(handle, 5, 1))
        goto error;
    
    return handle;
    
error:
    libusb_close(handle);
    return NULL;
}

#define EP_CONTROL_UNDEFINED 0
#define SAMPLING_FREQ_CONTROL 1
#define PITCH_CONTROL 2

#define SET_CUR 0x01
#define GET_CUR 0x81
#define SET_MIN 0x02
#define GET_MIN 0x82
#define SET_MAX 0x03
#define GET_MAX 0x83
#define SET_RES 0x04
#define GET_RES 0x84
#define SET_MEM 0x05
#define GET_MEM 0x85
#define GET_STAT 0xFF

#if 0
int configure_omega(struct libusb_device_handle *h)
{
    uint8_t buffer[3] = {0,0,0};
    uint8_t controlSelector = SAMPLING_FREQ_CONTROL;
    uint8_t value = 0;
    uint8_t intf_or_endpt = 0x83;
    uint8_t entity_id = 0;
    int res = libusb_control_transfer(h, 0x40, 0x49, 0, 0, NULL, 0, multimix_timeout);
    printf("res = %d\n", res);
    res = libusb_control_transfer(h, 0xA2, GET_CUR, 256, intf_or_endpt, buffer, sizeof(buffer), multimix_timeout);
    if (res < 0)
    {
        printf("res = %d errno = %d\n", res, errno);
        return -1;
    }
    printf("min_sample_rate = %d\n", buffer[0] + 256 * buffer[1] + 65536 * buffer[2]);
    return 0;
}
#endif

int configure_omega(struct libusb_device_handle *h, int sample_rate)
{
    uint8_t freq_data[3];
    freq_data[0] = sample_rate & 0xFF;
    freq_data[1] = (sample_rate & 0xFF00) >> 8;
    freq_data[2] = (sample_rate & 0xFF0000) >> 16;
    if (libusb_control_transfer(h, 0x22, 0x01, 256, epOUT, freq_data, 3, multimix_timeout) != 3)
        return -1;
    if (libusb_control_transfer(h, 0x22, 0x01, 256, epIN, freq_data, 3, multimix_timeout) != 3)
        return -1;
//    libusb_control_transfer(dev, 0x22, 0x01, 
    return 0;
}

#if 0

int get_multimix_config(struct libusb_device_handle *h)
{
    uint8_t bufsize;
    uint8_t freq_data[3];
    if (libusb_control_transfer(h, 0xC0, 0x49, 0, 0, &bufsize, 1, multimix_timeout) != 1)
        return -1;
    printf("Bufsize = %d\n", (int)bufsize);
    if (libusb_control_transfer(h, 0xA2, 0x81, 256, 0x02, freq_data, 3, multimix_timeout) != 3)
        return -1;
    printf("Freq1 = %d\n", freq_data[0] | (freq_data[1] << 8) | (freq_data[2] << 16));
    if (libusb_control_transfer(h, 0xA2, 0x81, 256, 0x86, freq_data, 3, multimix_timeout) != 3)
        return -1;
    printf("Freq2 = %d\n", freq_data[0] | (freq_data[1] << 8) | (freq_data[2] << 16));
    return 0;
}

#endif

// 192 bytes = 1ms@48000 (48 samples)

#define NUM_PLAY_BUFS 2
#define PLAY_PACKET_COUNT 2
#define PLAY_PACKET_SIZE 192

static float phase = 0;
static int phase2 = 0;

static int desync = 0;
static int samples_sent = 0;

static int srate = 44100;

static int calc_packet_lengths(struct libusb_transfer *t, int packets)
{
    int tsize = 0;
    int i;
    // printf("sync_freq = %d\n", sync_freq);
    // printf("desync %d\n", desync);
    int lag = desync / 1000;
    int nsamps = srate/1000;
    if (lag >= packets && nsamps < PLAY_PACKET_SIZE/4) {
        nsamps++;
        lag -= packets;
    }
    for (i = 0; i < packets; i++)
    {
        int v = (nsamps) * 4;
        t->iso_packet_desc[i].length = v;
        tsize += v;
    }
    // printf("tsize = %d\n", tsize);
    return tsize;
}

void play_callback(struct libusb_transfer *transfer)
{
    int i;
    //printf("Play Callback! %d %p status %d\n", (int)transfer->length, transfer->buffer, (int)transfer->status);
    transfer->length = calc_packet_lengths(transfer, transfer->num_iso_packets);
    desync += transfer->num_iso_packets * srate;
    desync -= transfer->length / 4 * 1000;
    int16_t *data = (int16_t*)transfer->buffer;
    for (i = 0; i < transfer->length / 4; i ++)
    {
        float v = 16000 * sin(phase);
        //phase += (phase2 & 4096) ? 0.02 : 0.04;
        phase += (phase2 & 16384) ? 0.04: 0.02;
        //phase += 0.2 * cos(phase2 / 16384.0);
        phase2++;
        if (phase > 2 * M_PI)
            phase -= 2 * M_PI;
        int vi = (int)v;
        data[i * 2] = vi;
        data[i * 2 + 1] = vi;
    }
    libusb_submit_transfer(transfer);
}

void play_stuff(struct libusb_device_handle *h, int index)
{
    struct libusb_transfer *t;
    int i;
    int packets = PLAY_PACKET_COUNT;
    t = libusb_alloc_transfer(packets);
    int tsize = srate * 4 / 1000;
    uint8_t *buf = (uint8_t *)malloc(PLAY_PACKET_SIZE*packets);
    //int ssf = 0;
    
    for (i = 0; i < tsize * packets; i++)
        buf[i] = 0;

    libusb_fill_iso_transfer(t, h, epOUT, buf, tsize * packets, packets, play_callback, (void *)index, 20000);
    libusb_set_iso_packet_lengths(t, tsize);
    libusb_submit_transfer(t);
}

#define NUM_RECORD_BUFS 2
#define NUM_REC_PACKETS 2
#define REC_PACKET_SIZE 192

static uint8_t *record_buffers[NUM_RECORD_BUFS];
// struct libusb_transfer *record_transfers[NUM_RECORD_BUFS];

float filt = 0;

void record_callback(struct libusb_transfer *transfer)
{
    int i;
    // printf("Record callback! %p index %d len %d\n", transfer, (int)transfer->user_data, transfer->length);
    
    float avg = 0;
    int16_t *bufz = (int16_t*)transfer->buffer;
    int items = transfer->length / 4;
    for (i = 0; i < items; i ++)
    {
        int16_t *buf = &bufz[i * 2];
        avg += fabs(buf[0]) + fabs(buf[1]);
    }
    if (avg)
        printf("%10.6f dB\r", 6 * log(avg / 32767 / items) / log(2.0));
    libusb_submit_transfer(transfer);
}

void record_stuff(struct libusb_device_handle *h, int index)
{
    // 0x02
    struct libusb_transfer *t;
    
    record_buffers[index] = (uint8_t*)malloc(NUM_REC_PACKETS * REC_PACKET_SIZE);
    memset(record_buffers[index], 0, NUM_REC_PACKETS * REC_PACKET_SIZE);
    
    t = libusb_alloc_transfer(NUM_REC_PACKETS);    
    libusb_fill_iso_transfer(t, h, epIN, record_buffers[index], NUM_REC_PACKETS * REC_PACKET_SIZE, NUM_REC_PACKETS, record_callback, (void *)index, 1000);
    libusb_set_iso_packet_lengths(t, REC_PACKET_SIZE);
    if (libusb_submit_transfer(t))
        goto error;
    return;
error:
    printf("Record setup failed for index=%d\n", index);
}

void usb_main_loop()
{
    struct sched_param p;
    p.sched_priority = 10;
    sched_setscheduler(0, SCHED_FIFO, &p);
    while(1) {
	struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 100
        };
	libusb_handle_events_timeout(usbctx, &tv);
    }
}

int main(int argc, char *argv[])
{
    struct libusb_device_handle *h;
    int i;
    
    init_usb();
    h = open_multimix();
    if (!h)
    {
        printf("Alesis MultiMix 8 could not be opened.\n");
        return 2;
    }
    
    // 10: 4 3 3 - 16 bit
    // 30: 2 2 1 2 2 2 1 - 24 bit
    // 50: 4 3 3 
    // 70: 2 2 1 2 2 2 1
    printf("Error = %d\n", configure_omega(h, srate));
    for (i = 0; i < NUM_RECORD_BUFS; i++)
        record_stuff(h, i);
    //for (i = 0; i < NUM_SYNC_BUFS; i++)
    //    sync_stuff(h, i);
    for (i = 0; i < NUM_PLAY_BUFS; i++)
        play_stuff(h, i);
    //play_stuff(h, 0);
    usb_main_loop();
    return 0;
}

