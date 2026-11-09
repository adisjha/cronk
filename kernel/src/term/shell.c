#include "shell.h"
#include "sched/benchmark.h"
#include "program_view.h"
#include "storage/files.h"
#include "sched/scheduler.h"
#include "memory_view.h"
#include "clock_view.h"
#include "terminal.h"
#include "drivers/keyboard/keyboard.h"
#include <stddef.h>

#define INPUT_MAX 256

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
        terminal_write(fb, "help   clear   programs   mem   time   run FILE (try run hello.bin)\n"
                           "runbg FILE   start a disk program in the background\n"
                           "ps           list tasks and scheduled slices\n"
                           "spawn        start a background worker\n"
                           "kill ID      stop a background worker\n"
                           "uptime       seconds since scheduler start\n"
                           "bench        compare polling and sleeping on one CPU\n"
                           "echo TEXT    print text\n"
                           "ls   cat NAME   write NAME TEXT   rm NAME\n");
    } else if (streq(cmd, "ls")) {
        files_command(fb, FILE_LIST, cmd);
    } else if (starts_with(cmd, "cat ")) {
        files_command(fb, FILE_READ, cmd + 4);
    } else if (starts_with(cmd, "write ")) {
        files_command(fb, FILE_WRITE, cmd + 6);
    } else if (starts_with(cmd, "rm ")) {
        files_command(fb, FILE_REMOVE, cmd + 3);
    } else if (streq(cmd, "ps")) {
        terminal_write(fb, "ID   TASK   SLICES   LAST OUTPUT\n");
        for (unsigned i = 0; i < TASK_SLOTS; ++i) {
            struct task_info info;
            if (!scheduler_info(i, &info)) continue;
            terminal_write_uint(fb, info.id);
            terminal_write(fb, "   ");
            terminal_write(fb, info.name);
            terminal_write(fb, "   ");
            terminal_write_uint(fb, info.slices);
            terminal_write(fb, "   ");
            terminal_write(fb, info.output);
            terminal_putchar(fb, '\n');
        }
    } else if (streq(cmd, "spawn")) {
        unsigned id = scheduler_spawn("worker", worker, NULL, NULL);
        if (id) {
            terminal_write(fb, "started worker ");
            terminal_write_uint(fb, id);
            terminal_putchar(fb, '\n');
        } else terminal_write(fb, "task table full\n");
    } else if (streq(cmd, "bench")) {
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
    else if (streq(cmd, "time") || streq(cmd, "clock")) { clock_view(fb); }
    else if (streq(cmd, "programs") || streq(cmd, "top")) { program_view(fb); }
    else if (streq(cmd, "mem")) { memory_view(fb); }
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
    char input_buf[INPUT_MAX];
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
        terminal_erase_cursor(fb);
        if (c == KEY_F1 || c == KEY_F2 || c == KEY_F3) {
            if (c == KEY_F1) program_view(fb);
            else if (c == KEY_F3) clock_view(fb);
            else memory_view(fb);
            terminal_write(fb, "> ");
            input_buf[input_len] = 0;
            terminal_write(fb, input_buf);
        }
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
        else {
            if (input_len < INPUT_MAX - 1){
                input_buf[input_len++] = c;
                terminal_putchar(fb, c);
            }
        }
        
        terminal_draw_cursor(fb);
    }
}
