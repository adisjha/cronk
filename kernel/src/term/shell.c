#include "gfx/framebuffer.h"
#include "shell.h"
#include "format.h"
#include "mm/pages.h"
#include "arch/cpu.h"
#include "editor.h"
#include "sched/benchmark.h"
#include "sched/hardware_benchmark.h"
#include "apps.h"
#include "storage/files.h"
#include "sched/scheduler.h"
#include "terminal.h"
#include "drivers/keyboard/keyboard.h"
#include <stddef.h>

#define INPUT_MAX (FILE_TEXT_MAX + 128)

static int streq(const char *a, const char *b) {
    while (*a && *b){
        if (*a != *b){return 0;}
        a++; b++;
    }
    return *a == *b;
}

static int starts_with(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static void worker(void *unused) {
    (void)unused;
    // The demo wakes once per second.
    for (;;) scheduler_sleep(100);
}

static void busy_worker(void *unused) {
    (void)unused;
    unsigned value=1;
    for(;;) {
        for(unsigned i=0;i<1000000;++i){value^=value<<13;value^=value>>17;value^=value<<5;}
        char text[32];format_uint(text,value);scheduler_output(NULL,text);
    }
}

static unsigned parse_id(const char *text) {
    unsigned id = 0;
    while (*text) {
        unsigned digit = (unsigned)(*text++ - '0');
        if (digit > 9 || id > (UINT32_MAX - digit) / 10) return 0;
        id = id * 10 + digit;
    }
    return id;
}

static void shell_execute(struct limine_framebuffer *fb, char *cmd){
    if (streq(cmd, "help")) {
        app_open(fb, APP_HELP);
    } else if (streq(cmd, "apps")) {
        app_open(fb, APP_MENU);
    } else if (streq(cmd, "files")) {
        app_open(fb, APP_FILES);
    } else if (streq(cmd, "calc")) {
        app_open(fb, APP_CALC);
    } else if (streq(cmd, "scale") || starts_with(cmd, "scale ")) {
        if (cmd[5]) {
            unsigned value = parse_id(cmd + 6);
            if (!ui_set_scale(fb, value)) {
                terminal_write(fb, "choose 1-4 with at least 640x360 logical pixels\n");
                return;
            }
            terminal_clear(fb);
        }
        terminal_write_uint(fb, fb->width);
        terminal_putchar(fb, 'x');
        terminal_write_uint(fb, fb->height);
        terminal_write(fb, "   UI scale ");
        terminal_write_uint(fb, ui_scale());
        terminal_write(fb, "x\n");
    } else if (streq(cmd, "pwd")) {
        files_command(fb, FILE_PWD, cmd + 3);
    } else if (streq(cmd, "cd") || starts_with(cmd, "cd ")) {
        files_command(fb, FILE_CD, cmd + 2);
    } else if (starts_with(cmd, "mv ")) {
        files_command(fb, FILE_MOVE, cmd + 3);
    } else if (starts_with(cmd, "cp ")) {
        files_command(fb, FILE_COPY, cmd + 3);
    } else if (starts_with(cmd, "mkdir ")) {
        files_command(fb, FILE_MKDIR, cmd + 6);
    } else if (starts_with(cmd, "rmdir ")) {
        files_command(fb, FILE_RMDIR, cmd + 6);
    } else if (streq(cmd, "ls") || starts_with(cmd, "ls ")) {
        files_command(fb, FILE_LIST, cmd + 2);
    } else if (starts_with(cmd, "cat ")) {
        files_command(fb, FILE_READ, cmd + 4);
    } else if (streq(cmd, "edit") || starts_with(cmd, "edit ")) {
        desktop_edit(fb, cmd + 4);
    } else if (starts_with(cmd, "write ")) {
        files_command(fb, FILE_WRITE, cmd + 6);
    } else if (starts_with(cmd, "rm ")) {
        files_command(fb, FILE_REMOVE, cmd + 3);
    } else if (streq(cmd, "ps")) {
        terminal_write(fb, "ID   LEVEL  TASK   SLICES   RAM KiB   RUNTIME   CORE   LAST OUTPUT\n");
        for (unsigned i = 0; i < TASK_SLOTS; ++i) {
            struct task_info info;
            if (!scheduler_info(i, &info)) continue;
            terminal_write_uint(fb, info.id);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, info.ring);
            terminal_write(fb, "     ");
            terminal_write(fb, info.name);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, info.slices);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, (info.ram_bytes + 1023) / 1024);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, info.runtime);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, info.cpu);
            terminal_write(fb, "   ");
            terminal_write(fb, info.output);
            terminal_putchar(fb, '\n');
        }
    } else if (streq(cmd, "spawn") || streq(cmd,"spawn busy")) {
        bool busy=streq(cmd,"spawn busy");
        unsigned id = scheduler_spawn(busy?"cpu worker":"worker", busy?busy_worker:worker, NULL, NULL);
        if (id) {
            terminal_write(fb, busy?"started cpu worker ":"started worker ");
            terminal_write_uint(fb, id);
            terminal_putchar(fb, '\n');
        } else terminal_write(fb, "task table full\n");
    } else if (streq(cmd, "bench")) {
        hardware_benchmark(fb, BENCH_ALL);
    } else if (streq(cmd, "bench cpu")) {
        hardware_benchmark(fb, BENCH_CPU);
    } else if (streq(cmd, "bench ram")) {
        hardware_benchmark(fb, BENCH_RAM);
    } else if (starts_with(cmd, "bench ram ")) {
        hardware_ram_benchmark(fb, cmd + 10);
    } else if (streq(cmd, "bench caches") || streq(cmd, "bench cache")) {
        hardware_benchmark(fb, BENCH_CACHES);
    } else if (streq(cmd, "bench disk")) {
        hardware_benchmark(fb, BENCH_DISK);
    } else if (streq(cmd, "bench parallel")) {
        scheduler_parallel_benchmark(fb);
    } else if (streq(cmd, "bench sched")) {
        scheduler_benchmark(fb);
    } else if (streq(cmd, "uptime")) {
        terminal_write_uint(fb, scheduler_seconds());
        terminal_write(fb, " seconds\n");
    } else if (starts_with(cmd, "kill ")) {
        terminal_write(fb, scheduler_stop(parse_id(cmd + 5)) ? "stopped\n" : "unknown or protected task\n");
    } else if (streq(cmd, "echo") || starts_with(cmd, "echo ")) {
        terminal_write(fb, cmd[4] ? cmd + 5 : "");
        terminal_putchar(fb, '\n');
    }
    else if (streq(cmd, "time") || streq(cmd, "clock")) { app_open(fb, APP_CLOCK); }
    else if (streq(cmd, "programs") || streq(cmd, "top")) { app_open(fb, APP_PROGRAMS); }
    else if (streq(cmd, "mem")) { app_open(fb, APP_MEMORY); }
    else if (starts_with(cmd, "runbg ")) { files_command(fb, FILE_RUNBG, cmd + 6); }
    else if (starts_with(cmd, "run ")) { files_command(fb, FILE_RUN, cmd + 4); }
    else if (streq(cmd, "clear")){terminal_clear(fb);}
    else if (cmd[0] == 0){}
    else{
        terminal_write(fb, "unknown command: ");
        terminal_write(fb, cmd);
        terminal_putchar(fb, '\n');
    }
}

void shell_run(struct limine_framebuffer *fb) {
    char *input_buf = pages_alloc((INPUT_MAX+4095)/4096,PAGE_BUFFER);
    if(!input_buf){terminal_write(fb,"unable to allocate shell input\n");for(;;)scheduler_sleep(100);}
    scheduler_charge_current_memory(((INPUT_MAX+4095)/4096)*PAGE_SIZE);
    size_t input_len = 0;
    terminal_write(fb, "> ");
    terminal_draw_cursor(fb);


    while(1){
        terminal_tick(fb, NULL);
        char c = keyboard_poll_char();

        if(!c){
            scheduler_wait_input();
            continue;
        }
        if (c == KEY_PAGE_UP || c == KEY_PAGE_DOWN) {
            terminal_history(fb, c == KEY_PAGE_UP ? 1 : -1);
            if (terminal_at_bottom()) terminal_draw_cursor(fb);
            continue;
        }
        if (!terminal_at_bottom()) terminal_history(fb, 0);
        terminal_erase_cursor(fb);
        if (c == KEY_APPS) { app_open(fb, APP_MENU); }

        else if (c=='\n') {
            input_buf[input_len] = 0;
            
            terminal_putchar(fb, '\n');
            shell_execute(fb, input_buf);
            
            input_len = 0;
            terminal_write(fb, "> " );
        }
        else if (c=='\b'){
            if (input_len > 0) {
                --input_len;
                input_buf[input_len] = 0;
                terminal_backspace(fb);
            }
        }
        else if (c >= ' ') {
            if (input_len < INPUT_MAX - 1){
                input_buf[input_len++] = c;
                terminal_putchar(fb, c);
            }
        }
        
        terminal_draw_cursor(fb);
    }
}
