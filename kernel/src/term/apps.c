#include "apps.h"
#include "editor.h"
#include "sched/service.h"
#include "arch/cpu.h"
#include "mm/pages.h"
#include "terminal.h"
#include "program_view.h"
#include "memory_view.h"
#include "clock_view.h"
#include "drivers/keyboard/keyboard.h"
#include "gfx/framebuffer.h"
#include "storage/files.h"
#include "sched/scheduler.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static char next_key(struct limine_framebuffer *fb) {
    for (;;) {
        terminal_tick(fb, NULL);
        char key = keyboard_poll_char();
        if (key) return key;
        scheduler_wait_input();
    }
}

static void move_selection(char key, size_t *index, size_t count) {
    if (!count) return;
    if (key == KEY_DOWN) *index = (*index + 1) % count;
    if (key == KEY_UP) *index = (*index + count - 1) % count;
}

static size_t visible_rows(struct limine_framebuffer *fb) {
    size_t rows = ui_height(fb) / FONT_HEIGHT;
    return rows > 6 ? rows - 6 : 1;
}

static void menu_row(struct limine_framebuffer *fb, size_t row, const char *label, bool selected) {
    size_t y = 24 + (row + 4) * FONT_HEIGHT;
    size_t width = ui_width(fb) - 2 * FONT_WIDTH;
    if (width > 56 * FONT_WIDTH) width = 56 * FONT_WIDTH;
    uint32_t bg = selected ? 0x345e79 : 0;
    draw_rect(fb, FONT_WIDTH, y, width, FONT_HEIGHT, bg);
    draw_string(fb, label, 2 * FONT_WIDTH, y, 0xffffff, bg);
}

static const char *const commands[] = {
    "apps          Browse applications", "help          Search the command library",
    "programs/top  Live program manager", "mem           Browse memory",
    "time/clock    UTC clock", "calc          Integer calculator",
    "files         Browse disk files", "mv SRC DEST   Move or rename a file/folder",
    "cp SRC DEST   Copy a file",
    "ls [PATH]     List files and folders",
    "mkdir PATH    Create a folder", "rmdir PATH    Remove an empty folder",
    "cd [PATH]     Change folder (.. for parent)", "pwd           Show current path",
    "cat NAME      Read a text file", "write NAME TEXT   Save or replace a file",
    "edit PATH     Text editor (F2 save, Esc quit)",
    "rm NAME       Remove a file", "run FILE      Launch a disk program",
    "runbg FILE    Launch in background", "ps            List tasks and privilege levels",
    "kill ID       Stop a background task", "spawn         Start a demo worker", "spawn busy    Start a CPU worker for per-core graphs",
    "uptime        Seconds since scheduler start", "bench         CPU, RAM and disk benchmarks",
    "bench cpu/ram/disk   Run one hardware test", "bench parallel   Four CPU jobs across available cores", "bench sched   Run the scheduler benchmark",
    "bench ram 128 KiB  RAM test; bare sizes mean MiB", "bench caches  Cache geometry and size/speed sweep",
    "scale [1-4]   Show or change UI scale", "echo TEXT     Print text",
    "clear         Clear terminal and scrollback"
};

static bool matches(const char *text, const char *query) {
    if (!*query) return true;
    for (; *text; ++text) {
        size_t n = 0;
        while (query[n] && text[n]) {
            char c = text[n];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            char q = query[n];
            if (q >= 'A' && q <= 'Z') q += 'a' - 'A';
            if (c != q) break;
            ++n;
        }
        if (!query[n]) return true;
    }
    return false;
}

static void help(struct limine_framebuffer *fb) {
    char query[32] = {0};
    size_t length = 0, selected = 0;
    terminal_clear(fb);
    for (;;) {
        size_t indices[COUNT(commands)], count = 0;
        for (size_t i = 0; i < COUNT(commands); ++i)
            if (matches(commands[i], query)) indices[count++] = i;
        terminal_begin_refresh();
        terminal_write(fb, "Command library\nType to filter   Up/Down browse   Esc back\nSearch: ");
        terminal_write(fb, query);
        terminal_end_refresh(fb);
        size_t rows = visible_rows(fb), first = selected / rows * rows;
        for (size_t row = 0; row < rows && row < COUNT(commands); ++row) {
            size_t i = first + row;
            const char *label = i < count ? commands[indices[i]] : "";
            if (!count && !row) label = "No matching commands";
            menu_row(fb, row, label, i < count && i == selected);
        }
        char key = next_key(fb);
        if (key == KEY_ESC) return;
        if (key == '\b' && length) { query[--length] = 0; selected = 0; }
        else if (key >= ' ' && length + 1 < sizeof(query)) {
            query[length++] = key;
            query[length] = 0;
            selected = 0;
        } else move_selection(key, &selected, count);
    }
}

static bool operand(const char **cursor, int64_t *value) {
    const char *p = *cursor;
    while (*p == ' ') ++p;
    bool negative = *p == '-';
    if (negative) ++p;
    if (*p < '0' || *p > '9') return false;
    int64_t n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p++ - '0');
        if (n > INT32_MAX + (int64_t)negative) return false;
    }
    while (*p == ' ') ++p;
    *cursor = p;
    *value = negative ? -n : n;
    return true;
}

static void calculate(struct limine_framebuffer *fb, const char *expr) {
    int64_t a, b, result;
    if (!operand(&expr, &a) || !*expr) goto invalid;
    char op = *expr++;
    if (!operand(&expr, &b) || *expr) goto invalid;
    switch (op) {
    case '+': result = a + b; break;
    case '-': result = a - b; break;
    case '*': result = a * b; break;
    case '/':
        if (!b) { terminal_write(fb, "Division by zero"); return; }
        result = a / b;
        break;
    default: goto invalid;
    }
    terminal_write(fb, "= ");
    if (result < 0) terminal_putchar(fb, '-');
    terminal_write_uint(fb, result < 0 ? -result : result);
    return;
invalid:
    terminal_write(fb, "Use two 32-bit integers and one operator.");
}

static void calculator(struct limine_framebuffer *fb) {
    char expr[64];
    size_t length = 0;
    terminal_clear(fb);
    terminal_write(fb, "Calculator\nInteger + - * /   Enter evaluate   Esc back\n\n> ");
    for (;;) {
        char key = next_key(fb);
        if (key == KEY_ESC) return;
        if (key == '\n') {
            expr[length] = 0;
            terminal_putchar(fb, '\n');
            calculate(fb, expr);
            terminal_write(fb, "\n> ");
            length = 0;
        } else if (key == '\b' && length) {
            --length;
            terminal_backspace(fb);
        } else if (key >= ' ' && length + 1 < sizeof(expr)) {
            expr[length++] = key;
            terminal_putchar(fb, key);
        }
    }
}

static void file_browser(struct limine_framebuffer *fb) {
    struct file_item items[FILE_SLOTS];
    unsigned dir = files_cwd(), parent = 0;
    size_t selected = 0;
    for (;;) {
        int count = files_directory(dir, items, &parent);
        terminal_clear(fb);
        terminal_write(fb, "Files  ");
        if (count >= 0) files_print_path(fb, dir);
        terminal_write(fb, "\nUp/Down select   Enter open   Backspace up   Esc back\n");
        if (count <= 0) terminal_write(fb, count ? "Data disk unavailable" : "Folder is empty");
        for (;;) {
            size_t rows = visible_rows(fb), first = selected / rows * rows;
            for (size_t row = 0; count > 0 && row < rows && row < (size_t)count; ++row) {
                size_t i = first + row;
                char label[34] = {0};
                if (i < (size_t)count) {
                    size_t n = 0;
                    while (items[i].name[n]) { label[n] = items[i].name[n]; ++n; }
                    if (items[i].kind == FOLDER) label[n] = '/';
                }
                menu_row(fb, row, label, i == selected);
            }
            char key = next_key(fb);
            if (key == KEY_ESC) {
                if (!dir || count < 0) return;
                dir = parent; selected = 0; break;
            }
            if (key == '\b' && dir && count >= 0) { dir = parent; selected = 0; break; }
            if (count <= 0) continue;
            if (key != '\n') { move_selection(key, &selected, count); continue; }
            if (items[selected].kind == FOLDER) {
                dir = items[selected].slot;
                selected = 0;
            } else {
                terminal_clear(fb);
                terminal_write(fb, items[selected].name);
                terminal_write(fb, "\nEsc back\n\n");
                if (items[selected].kind == EXEC_FILE) terminal_write(fb, "Executable. Launch with run or runbg from the shell.");
                else files_preview(fb, items[selected].slot);
                while (next_key(fb) != KEY_ESC) {}
            }
            break;
        }
    }
}

static void stopwatch(struct limine_framebuffer *fb) {
    uint64_t started = scheduler_ticks(), elapsed = 0;
    bool running = true;
    terminal_clear(fb);
    for (;;) {
        terminal_tick(fb, NULL);
        terminal_begin_refresh();
        terminal_write(fb, "Stopwatch\nSpace pause/resume   R reset   Esc back\n\n");
        terminal_write_uint(fb, elapsed * 10);
        terminal_write(fb, " ms");
        terminal_end_refresh(fb);
        char key = keyboard_poll_char();
        if (key == KEY_ESC) return;
        uint64_t now = scheduler_ticks();
        if (key == ' ') { running = !running; started = now - elapsed; }
        if (key == 'r' || key == 'R') { elapsed = 0; started = now; }
        if (running) elapsed = now - started;
        scheduler_sleep(5);
    }
}

static void system_info(struct limine_framebuffer *fb) {
    terminal_clear(fb);
    for (;;) {
        terminal_tick(fb, NULL);
        terminal_begin_refresh();
        terminal_write(fb, "System information\nEsc back\n\nCronk / " ARCH_NAME " / scheduled CPU cores\nDisplay: ");
        terminal_write_uint(fb, fb->width);
        terminal_putchar(fb, 'x');
        terminal_write_uint(fb, fb->height);
        terminal_write(fb, "\nUI scale: ");
        terminal_write_uint(fb, ui_scale());
        terminal_write(fb, "x\nUptime: ");
        terminal_write_uint(fb, scheduler_seconds());
        terminal_write(fb, " s\nAvailable memory: ");
        terminal_write_uint(fb, pages_stats().available * 4);
        terminal_write(fb, " KiB\nKernel privilege level: ");
        terminal_write_uint(fb, ARCH_KERNEL_LEVEL);
        terminal_write(fb, "\nUser privilege level: ");
        terminal_write_uint(fb, ARCH_USER_LEVEL); terminal_putchar(fb, '\n');
        terminal_end_refresh(fb);
        if (next_key(fb) == KEY_ESC) return;
    }
}

static void show(struct limine_framebuffer *fb, enum app app) {
    static const char *names[] = {"applications", "stopwatch", "system information", "program manager",
                                  "memory viewer", "clock viewer", "calculator", "file browser", "help"};
    scheduler_current_name(names[app]);
    terminal_tab(fb, app == APP_HELP ? TAB_HELP : TAB_APPS);
    switch (app) {
    case APP_PROGRAMS: program_view(fb); break;
    case APP_MEMORY: memory_view(fb); break;
    case APP_CLOCK: clock_view(fb); break;
    case APP_FILES: file_browser(fb); break;
    case APP_CALC: calculator(fb); break;
    case APP_HELP: help(fb); break;
    case APP_SYSTEM: system_info(fb); break;
    case APP_STOPWATCH: stopwatch(fb); break;
    default: break;
    }
}

static void app_run(struct limine_framebuffer *fb, enum app app) {
    if (app == APP_MENU) {
        static const struct { const char *name; enum app id; } entries[] = {
            {"Program manager", APP_PROGRAMS}, {"Memory", APP_MEMORY}, {"Clock", APP_CLOCK},
            {"Stopwatch", APP_STOPWATCH}, {"Calculator", APP_CALC}, {"Files", APP_FILES}, {"System information", APP_SYSTEM}, {"Command library", APP_HELP}
        };
        size_t selected = 0;
        bool entering = true;
        for (;;) {
            if (entering) {
                terminal_tab(fb, TAB_APPS);
                terminal_clear(fb);
                terminal_write(fb, "Applications\nUp/Down select   Enter open   Esc/F1 shell");
                entering = false;
            }
            for (size_t i = 0; i < COUNT(entries); ++i)
                menu_row(fb, i, entries[i].name, selected == i);
            char key = next_key(fb);
            if (key == KEY_ESC || key == KEY_APPS) break;
            if (key == '\n') {
                show(fb, entries[selected].id);
                entering = true;
            }
            else move_selection(key, &selected, COUNT(entries));
        }
    } else show(fb, app);
    terminal_tab(fb, TAB_SHELL);
    terminal_history(fb, 0);
}

static struct kernel_service desktop;
struct app_request { struct limine_framebuffer *fb; enum app app; };
static void open_requested_app(void *context) {
    struct app_request *request = context;
    static const char *names[] = {"applications", "stopwatch", "system information", "program manager",
                                  "memory viewer", "clock viewer", "calculator", "file browser", "help"};
    scheduler_current_name(names[request->app]);
    app_run(request->fb, request->app);
    scheduler_current_name("desktop");
}
void desktop_start(struct limine_framebuffer *fb) {
    (void)fb;
    service_start(&desktop, "desktop", pages_breakdown().display * PAGE_SIZE);
}
void app_open(struct limine_framebuffer *fb, enum app app) {
    struct app_request request = {fb, app};
    service_call(&desktop, open_requested_app, &request);
}

struct editor_request { struct limine_framebuffer *fb; char *args; };
static void edit_requested_file(void *context) {
    struct editor_request *request = context;
    scheduler_current_name("text editor");
    editor_open(request->fb, request->args);
    scheduler_current_name("desktop");
}
void desktop_edit(struct limine_framebuffer *fb, char *args) {
    struct editor_request request = {fb, args};
    service_call(&desktop, edit_requested_file, &request);
}
