#ifndef TOMOE_AUDIO_H
#define TOMOE_AUDIO_H

struct tomoe_audio;

struct tomoe_audio *tomoe_audio_open(int *error);

int tomoe_audio_poll(struct tomoe_audio *audio);

const char *tomoe_audio_snapshot(struct tomoe_audio *audio);

int tomoe_audio_fd(const struct tomoe_audio *audio);

unsigned tomoe_audio_mask(const struct tomoe_audio *audio);

int tomoe_audio_timeout(const struct tomoe_audio *audio, int max_ms);

void tomoe_audio_close(struct tomoe_audio *audio);

#endif
