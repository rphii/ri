#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <rlso.h>
#include <rlc.h>
#include <sys/ioctl.h>

#ifndef ESC_CODE_H

#define ESC_CODE_CLEAR          "\e[2J"
#define ESC_CODE_GOTO(x,y)      "\e[%u;%uH", (y)+1, (x)+1
#define ESC_CODE_HOME           "\e[H"
#define ESC_CODE_CURSOR_HIDE    "\e[?25l"
#define ESC_CODE_CURSOR_SHOW    "\e[?25h"
#define ESC_CODE_CURSOR_DEFAULT "\e[0 q"
#define ESC_CODE_CURSOR_BAR     "\e[6 q"
#define ESC_CODE_CURSOR_BLOCK   "\e[2 q"

#define ESC_CODE_LEN(x)     sizeof(x)-1

#define ESC_CODE_H
#endif


struct termios termios_entry;

typedef uint8_t byte;

int u32_cmp(uint32_t a, uint32_t b) {
    return a - b;
}
int u32_hash(uint32_t a) {
    uint32_t h = a;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

LUT_INCLUDE(Tucw, tucw, uint32_t, BY_VAL, byte, BY_VAL);
LUT_IMPLEMENT(Tucw, tucw, uint32_t, BY_VAL, byte, BY_VAL, u32_hash, u32_cmp, 0, 0);

#define INPUT_MAX   16

typedef struct Input {
    byte c[INPUT_MAX];
    byte bytes;
    bool carry_esc;
} Input;

void die(const char *s) {
    perror(s);
    exit(1);
}

int input_get_byte(byte *c) {
    int nread;
    if ((nread = read(STDIN_FILENO, c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return nread;
}

int input_get(Input *input) {
    byte c;
    input->bytes = 0;
    int have_c = false;
    if(input->carry_esc) {
        c = 0x1b;
        have_c = true;
    } else {
        have_c = input_get_byte(&c);
    }
    input->carry_esc = false;
    if(have_c) {
        if(c >= 0xC0) {
            byte bytes = 0;
            if(c < 0x80) bytes = 1;
            else if((c & 0xE0) == 0xC0) bytes = 2;
            else if((c & 0xF0) == 0xE0) bytes = 3;
            else if((c & 0xF8) == 0xF0) bytes = 4;
            input->bytes = bytes;
            if(bytes > 0) input->c[0] = c;
            if(bytes > 1) input_get_byte(&input->c[1]);
            if(bytes > 2) input_get_byte(&input->c[2]);
            if(bytes > 3) input_get_byte(&input->c[3]);
        } else if (c == 0x1b) {
            byte bytes = 0;
            input->c[bytes] = c;
            while(bytes + 1 < INPUT_MAX && input_get_byte(&input->c[++bytes])) {
                if(input->c[bytes] == 0x1b) {
                    input->carry_esc = true;
                    //--bytes;
                    break;
                }
            }
            input->bytes = bytes;
        } else {
            input->bytes = 1;
            input->c[0] = c;
        }
    }
    return input->bytes;
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios_entry) == -1) {
        die("tcsetattr");
    }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &termios_entry) == -1) die("tcgetattr");
    atexit(disable_raw_mode);
    struct termios raw = termios_entry;
    cfmakeraw(&raw);
    //raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    //raw.c_oflag &= ~(OPOST);
    //raw.c_cflag |= (CS8);
    //raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int cursor_pos_get(int *x, int *y) {
    char buf[32];
    unsigned int i = 0;
    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = 0;
    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    char *endptr;
    int try_y = strtoul(&buf[2], &endptr, 10);
    int offs = endptr - buf;
    if(buf[offs++] != ';') return -1;
    int try_x = strtoul(&buf[offs], &endptr, 10);
    offs = endptr - buf;
    if(buf[offs] != 0) return -1;
    *y = try_y;
    *x = try_x;
    return -1;
}

byte input_get_width_via_write(Input input) {
    if(input.bytes == 0) return 0;
    if(input.bytes == 1 && !iscntrl(input.c[0])) {
        return 1;
    }
    if(input.bytes >= 1 && iscntrl(input.c[0])) return 0;
    int x0, xE, y0, yE;
    cursor_pos_get(&x0, &y0);
    write(STDOUT_FILENO, input.c, input.bytes);
    cursor_pos_get(&xE, &yE);
    return xE - x0;
}

#define F_CTRL FG_YL_B
void input_fmt_ctrl(So *out, Input input) {
    if(input.bytes == 1) {
        if(input.c[0] == 0x1b) so_fmt(out, F("<esc>", F_CTRL));
        if(input.c[0] == 0x0d) so_fmt(out, F("<enter>", F_CTRL));
        if(input.c[0] == 0x7f) so_fmt(out, F("<bksp>", F_CTRL));
    } else if(input.bytes == 3) { 
        if(!memcmp(input.c, "\x1b[A", input.bytes)) so_fmt(out, F("<up>", F_CTRL));
        if(!memcmp(input.c, "\x1b[B", input.bytes)) so_fmt(out, F("<down>", F_CTRL));
        if(!memcmp(input.c, "\x1b[C", input.bytes)) so_fmt(out, F("<right>", F_CTRL));
        if(!memcmp(input.c, "\x1b[D", input.bytes)) so_fmt(out, F("<left>", F_CTRL));
        if(!memcmp(input.c, "\x1b[H", input.bytes)) so_fmt(out, F("<home>", F_CTRL));
        if(!memcmp(input.c, "\x1b[F", input.bytes)) so_fmt(out, F("<end>", F_CTRL));
        if(!memcmp(input.c, "\x1bOP", input.bytes)) so_fmt(out, F("<F1>", F_CTRL));
        if(!memcmp(input.c, "\x1bOQ", input.bytes)) so_fmt(out, F("<F2>", F_CTRL));
        if(!memcmp(input.c, "\x1bOR", input.bytes)) so_fmt(out, F("<F3>", F_CTRL));
        if(!memcmp(input.c, "\x1bOS", input.bytes)) so_fmt(out, F("<F4>", F_CTRL));
    } else if(input.bytes == 4) { 
        if(!memcmp(input.c, "\x1b[2~", input.bytes)) so_fmt(out, F("<insert>", F_CTRL));
        if(!memcmp(input.c, "\x1b[3~", input.bytes)) so_fmt(out, F("<delete>", F_CTRL));
        if(!memcmp(input.c, "\x1b[5~", input.bytes)) so_fmt(out, F("<pgup>", F_CTRL));
        if(!memcmp(input.c, "\x1b[6~", input.bytes)) so_fmt(out, F("<pgdn>", F_CTRL));
    } else if(input.bytes == 5) { 
        if(!memcmp(input.c, "\x1b[15~", input.bytes)) so_fmt(out, F("<F5>", F_CTRL));
        if(!memcmp(input.c, "\x1b[17~", input.bytes)) so_fmt(out, F("<F6>", F_CTRL));
        if(!memcmp(input.c, "\x1b[18~", input.bytes)) so_fmt(out, F("<F7>", F_CTRL));
        if(!memcmp(input.c, "\x1b[19~", input.bytes)) so_fmt(out, F("<F8>", F_CTRL));
        if(!memcmp(input.c, "\x1b[20~", input.bytes)) so_fmt(out, F("<F9>", F_CTRL));
        if(!memcmp(input.c, "\x1b[21~", input.bytes)) so_fmt(out, F("<F10>", F_CTRL));
        if(!memcmp(input.c, "\x1b[24~", input.bytes)) so_fmt(out, F("<F12>", F_CTRL));
    } else if(input.bytes == 6) { 
        if(!memcmp(input.c, "\x1b[1;5P", input.bytes)) so_fmt(out, F("<ctrl+F1>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;5Q", input.bytes)) so_fmt(out, F("<ctrl+F2>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;5R", input.bytes)) so_fmt(out, F("<ctrl+F3>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;5S", input.bytes)) so_fmt(out, F("<ctrl+F4>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;2P", input.bytes)) so_fmt(out, F("<shift+F1>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;2Q", input.bytes)) so_fmt(out, F("<shift+F2>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;2R", input.bytes)) so_fmt(out, F("<shift+F3>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;2S", input.bytes)) so_fmt(out, F("<shift+F4>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;6P", input.bytes)) so_fmt(out, F("<shift+ctrl+F1>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;6Q", input.bytes)) so_fmt(out, F("<shift+ctrl+F2>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;6R", input.bytes)) so_fmt(out, F("<shift+ctrl+F3>", F_CTRL));
        if(!memcmp(input.c, "\x1b[1;6S", input.bytes)) so_fmt(out, F("<shift+ctrl+F4>", F_CTRL));
    } else if(input.bytes == 7) { 
        if(!memcmp(input.c, "\x1b[15;5~", input.bytes)) so_fmt(out, F("<ctrl+F5>", F_CTRL));
        if(!memcmp(input.c, "\x1b[17;5~", input.bytes)) so_fmt(out, F("<ctrl+F6>", F_CTRL));
        if(!memcmp(input.c, "\x1b[18;5~", input.bytes)) so_fmt(out, F("<ctrl+F7>", F_CTRL));
        if(!memcmp(input.c, "\x1b[19;5~", input.bytes)) so_fmt(out, F("<ctrl+F8>", F_CTRL));
        if(!memcmp(input.c, "\x1b[20;5~", input.bytes)) so_fmt(out, F("<ctrl+F9>", F_CTRL));
        if(!memcmp(input.c, "\x1b[21;5~", input.bytes)) so_fmt(out, F("<ctrl+F10>", F_CTRL));
        if(!memcmp(input.c, "\x1b[23;5~", input.bytes)) so_fmt(out, F("<ctrl+F11>", F_CTRL));
        if(!memcmp(input.c, "\x1b[24;5~", input.bytes)) so_fmt(out, F("<ctrl+F12>", F_CTRL));
        if(!memcmp(input.c, "\x1b[15;2~", input.bytes)) so_fmt(out, F("<shift+F5>", F_CTRL));
        if(!memcmp(input.c, "\x1b[17;2~", input.bytes)) so_fmt(out, F("<shift+F6>", F_CTRL));
        if(!memcmp(input.c, "\x1b[18;2~", input.bytes)) so_fmt(out, F("<shift+F7>", F_CTRL));
        if(!memcmp(input.c, "\x1b[19;2~", input.bytes)) so_fmt(out, F("<shift+F8>", F_CTRL));
        if(!memcmp(input.c, "\x1b[20;2~", input.bytes)) so_fmt(out, F("<shift+F9>", F_CTRL));
        if(!memcmp(input.c, "\x1b[21;2~", input.bytes)) so_fmt(out, F("<shift+F10>", F_CTRL));
        if(!memcmp(input.c, "\x1b[23;2~", input.bytes)) so_fmt(out, F("<shift+F11>", F_CTRL));
        if(!memcmp(input.c, "\x1b[24;2~", input.bytes)) so_fmt(out, F("<shift+F12>", F_CTRL));
        if(!memcmp(input.c, "\x1b[15;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F5>", F_CTRL));
        if(!memcmp(input.c, "\x1b[17;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F6>", F_CTRL));
        if(!memcmp(input.c, "\x1b[18;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F7>", F_CTRL));
        if(!memcmp(input.c, "\x1b[19;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F8>", F_CTRL));
        if(!memcmp(input.c, "\x1b[20;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F9>", F_CTRL));
        if(!memcmp(input.c, "\x1b[21;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F10>", F_CTRL));
        if(!memcmp(input.c, "\x1b[23;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F11>", F_CTRL));
        if(!memcmp(input.c, "\x1b[24;6~", input.bytes)) so_fmt(out, F("<shift+ctrl+F12>", F_CTRL));
    }
#if 0
#endif
}

typedef enum {
    KEY_NONE,
    KEY_ESC,
    KEY_ENTER,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
} Key_List;

typedef struct InputDecode {
    Key_List id;
    bool esc;
    bool shift;
    bool ctrl;
    bool alt;
} InputDecode;

bool input_decode(Input *input, InputDecode *decode) {
    *decode = (InputDecode){0};
    if(input->bytes == 0) return false;
    if(!iscntrl(input->c[0])) return false;
    if(input->bytes == 1) {
        if(*input->c == 0x1b) decode->id = KEY_ESC;
        if(*input->c == 0x0d) decode->id = KEY_ENTER;
    } else if(input->bytes == 3) {
        if(!strncmp((char *)input->c, "\x1b[A", input->bytes)) decode->id = KEY_UP;
        if(!strncmp((char *)input->c, "\x1b[B", input->bytes)) decode->id = KEY_DOWN;
        if(!strncmp((char *)input->c, "\x1b[C", input->bytes)) decode->id = KEY_RIGHT;
        if(!strncmp((char *)input->c, "\x1b[D", input->bytes)) decode->id = KEY_LEFT;
    }
    return true;
}

#define ri_write_cstr(s) \
    write(STDOUT_FILENO, s, sizeof(s)-1)

#define ri_write_nstr(s, n) \
    write(STDOUT_FILENO, s, n)

typedef struct Point {
    ssize_t x;
    ssize_t y;
} Point;

int tucw_get_or_determine(Tucw *tucw, So so, So_Uc_Point *ucp) {
    if(so_uc_point(so, ucp)) return -1;
    if(ucp->val < 0x80) {
        if(!iscntrl((int)ucp->val)) return 1;
        else return 0; /* TODO control characters.. might want to display as ^A for ctrl+A */
    }
    TucwKV *kv = tucw_get_kv(tucw, ucp->val);
    if(kv) return kv->val;
    int x0, xE, y0, yE;
    ri_write_cstr(ESC_CODE_CURSOR_HIDE);
    ri_write_cstr(ESC_CODE_HOME);
    cursor_pos_get(&x0, &y0);
    ri_write_nstr(so_it0(so), ucp->bytes);
    cursor_pos_get(&xE, &yE);
    byte result = xE - x0;
    tucw_set(tucw, ucp->val, result);
    return result;
}

void ri_fmt_text_line(So *out, Point dimension, Tucw *tucw, So line, ssize_t offset) {
    ssize_t bytes = line.len;
    ssize_t line_len = 0;
    size_t n_ch = 0;
    So_Uc_Point ucp;
    if(offset < 0) {
        line_len += -offset;
        so_fmt(out, "%*s", -offset, "");
    } 
    for(ssize_t i = 0; i < bytes; i += ucp.bytes, ++n_ch) {
        So so0 = so_i0(line, i);
        int cw = tucw_get_or_determine(tucw, so0, &ucp);
        if(cw < 0) break;
        if(line_len + cw > dimension.x) break;
        line_len += cw;
        if(line_len < offset) continue;
        so_extend(out, so_iE(so0, ucp.bytes));
    }
}

void ri_fmt_text_lines(So *out, Point dimension, Tucw *tucw, VSo lines, Point offset) {
    ssize_t n_lines = array_len(lines);
    size_t n_line = 0;
    for(ssize_t i = offset.y; i < n_lines; ++i, ++n_line) {
        if(i > offset.y) so_extend(out, so("\r\n"));
        if(i >= 0) {
            So line = array_at(lines, i);
            ri_fmt_text_line(out, dimension, tucw, line, offset.x);
        }
        if(n_line >= dimension.y) break;
    }
}

int main(void) {
    struct winsize w;
    ioctl(0, TIOCGWINSZ, &w);
    Point dimension = { .x = w.ws_col, .y = w.ws_row };
    Point offset = {0};
    Tucw tucw = {0};
    enable_raw_mode();
    Input input = {0};
    InputDecode input_dc = {0};
    byte exit_cmp[4] = { 0x1b, 0x3a, 0x71, 0x0d };
    byte exit_count = 0;
    So out = {0};
    VSo lines = 0;
    vso_push(&lines, SO);
    So *line = &lines[0];
    write(STDOUT_FILENO, so_it0(out), out.len);
    bool quit = false;
    while(!quit) {
        if(!input_get(&input)) continue;
        if(input_decode(&input, &input_dc)) {
            switch(input_dc.id) {
                case KEY_ESC: quit = true; break;
                case KEY_UP: --offset.y; break;
                case KEY_DOWN: ++offset.y; break;
                case KEY_LEFT: --offset.x; break;
                case KEY_RIGHT: ++offset.x; break;
                case KEY_ENTER: {
                    size_t len = array_len(lines);
                    vso_push(&lines, SO);
                    line = &lines[len];
                } break;
                default: break;
            }
        } else {
            so_extend(line, so_ll(input.c, input.bytes));
            so_clear(&out);
        }
        /* render */
        so_clear(&out);
        //so_extend(&out, so(ESC_CODE_CURSOR_HIDE));
        so_extend(&out, so(ESC_CODE_CLEAR));
        so_extend(&out, so(ESC_CODE_HOME));
        ri_fmt_text_lines(&out, dimension, &tucw, lines, offset);
        ri_write_cstr(ESC_CODE_CURSOR_HIDE);
        write(STDOUT_FILENO, so_it0(out), out.len);
        ri_write_cstr(ESC_CODE_CURSOR_SHOW);
    }
    so_free(&out);
    return 0;
}

