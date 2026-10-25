#ifndef LE_ESPHOME_PLAYBACK_H
#define LE_ESPHOME_PLAYBACK_H
#include <stdint.h>
#include <stddef.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include "radio_resample.h"
#include "../../third-party/minimp3/minimp3.h"
#define ESP_PLAY_BODY_MAX (2U*1024U*1024U)
#define ESP_PLAY_STREAM_MAX (24U*1024U*1024U)
#define ESP_PLAY_STREAM_BUFFER (64U*1024U)
/* Single finite playback slot. tick does bounded nonblocking work. */
struct esp_playback {
 int state,fd,bus_fd,tls,wav,header_done,result,dns_fd,chunked,stream_wav,stream_kind,stream_done,stream_blocked;
 unsigned port;char host[254],path[1024],bus[256],request[1536];size_t request_len,request_sent;
 unsigned char buffer[ESP_PLAY_BODY_MAX+8192];size_t received,body_start,body_len,expected,position;
 unsigned char wire[8192],stream_pcm[ESP_PLAY_STREAM_BUFFER],wav_head[1024];
 unsigned char wav_tail_header[8];size_t wav_tail_len,wav_tail_left;
 size_t wire_len,wire_pos,stream_used,stream_pos,wav_head_len,wav_chunk_start,wav_chunk_end,wav_data_received,wire_left,wire_line_len;
 char wire_line[128];int wire_state,wav_fmt,wav_data;uint32_t wav_riff_size,wav_data_size;
 size_t stream_body_seen,stream_wire_seen;uint64_t stream_idle;
 unsigned char dns_query[300];size_t dns_query_len;uint16_t dns_id;
 int rate,channels,source_rate;uint64_t input_frames;size_t pcm_offset,pcm_bytes;uint64_t deadline,origin,drain_until,frames;
 int16_t output[8192*2];size_t output_len,output_sent;
 mp3dec_t decoder;struct le_radio_resampler resampler;
 mbedtls_ssl_context ssl;mbedtls_ssl_config config;mbedtls_x509_crt ca;mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
};
void esp_playback_init(struct esp_playback *);
void esp_playback_close(struct esp_playback *);
int esp_playback_start(struct esp_playback *,const char *,const char *,const char *,uint64_t);
int esp_playback_start_stream(struct esp_playback *,const char *,const char *,const char *,uint64_t);
/* 0 active, 1 drained successfully, -1 failed. */
int esp_playback_tick(struct esp_playback *,uint64_t);
#endif
