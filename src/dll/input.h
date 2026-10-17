// input.h: lokaler Controller im Netzmodus (XInput, DirectInput, Tastatur), siehe input.c
#pragma once
#include "net.h"

void input_init(void *real_xinput_getstate, void (*logger)(const char *fmt, ...));
void input_read(NETPAD *out);  // aktueller Zustand im Xbox-Format
