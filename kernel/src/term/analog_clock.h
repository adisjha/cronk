#pragma once
#include "arch/clock.h"
#include <limine.h>

struct clock_face { int x, y, radius; };
void clock_face_draw(struct limine_framebuffer *fb, struct clock_face face);
void clock_hands_draw(struct limine_framebuffer *fb, struct clock_face face,
                      const struct clock_sample *time, bool erase);
