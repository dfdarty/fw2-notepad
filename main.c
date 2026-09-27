/* notepad — a text editor for the FREE-WILi 2.
 *
 * Type with the on-screen keyboard, or with WiliBSP's two-press chord keys
 * on the five colour buttons (PAGE cycles their pages). The D-pad moves the
 * cursor, CENTER starts a new line, CANCEL deletes, OK saves. Tap the text
 * to put the cursor there. Everything is saved to /notes/notes.txt on the
 * SD card in the MAIN processor's slot, when you press OK and two seconds
 * after you stop typing. HOME held for 5 s exits, PAGE held for 5 s shows
 * the About screen (WiliBSP's rules for every app).
 *
 * Screen: status bar (0..21), text (24..135, 39 x 7 characters),
 * keyboard (144..319, four rows). Drawing goes to a framebuffer in PSRAM;
 * only the bands that changed are sent to the LCD, by DMA. */
#include "fw2.h"
#include "platform/diag.h"
#include "platform/psram.h"
#include "display/font5x7.h"
#include "pico/stdlib.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_sd.h"
#include "input/app_recovery_onewili.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ layout */
#define W ST7796_W                  /* 480 */
#define H ST7796_H                  /* 320 */
#define BAR_H 22
#define TEXT_Y 24
#define TEXT_SCALE 2
#define CW (6 * TEXT_SCALE)         /* 12 px per character */
#define LH (8 * TEXT_SCALE)         /* 16 px per line      */
#define COLS (W / CW - 1)           /* 39: the last cell is the scroll bar's */
#define ROWS 7
#define KB_Y 144
#define KEY_H 44

#define NOTE_DIR  "/notes"
#define NOTE_PATH NOTE_DIR "/notes.txt"
#define TEXT_MAX  8192              /* bytes, including the terminating 0 */
#define MAX_LINES 1024
#define AUTOSAVE_MS 2000

/* Colours as ordinary RGB565; converted to the LCD's wire order when drawn. */
#define C_BG       0x0000
#define C_TEXT     0xFFFF
#define C_BAR      0x18E3
#define C_DIM      0x8410
#define C_SCROLL   0xAD55
#define C_KEY      0x39E7
#define C_KEY_FN   0x2124
#define C_KEY_DOWN 0x2D7F
#define C_CURSOR   0xFFE0
#define C_OK       0x07E0
#define C_WARN     0xFD20
static const uint16_t k_chord_col[5] = { 0xD69A, 0xFF06, 0x1200, 0x00F8, 0x8007 };  /* grey yellow green blue red */
static const uint16_t k_chord_txt[5] = { 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF };

static inline uint16_t be(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

/* ------------------------------------------------------------- framebuffer */
static uint16_t __uninitialized_psram("fb") s_fb[W * H];
static int s_dirty_y0 = H, s_dirty_y1 = 0;       /* rows to send, [y0, y1) */

static void mark(int y0, int y1) {
    if (y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
}

static void fb_rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t v = be(c);
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = s_fb + yy * W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = v;
    }
}

static void fb_text(int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s, int n) {
    uint16_t f = be(fg), b = be(bg);
    int cw = 6 * scale, ch = 8 * scale;
    for (int i = 0; i < n && s[i]; i++, x += cw) {
        if (x < 0 || x + cw > W || y < 0 || y + ch > H) break;
        char c = s[i];
        const uint8_t *cols = (c >= FONT5X7_FIRST && c <= FONT5X7_LAST) ? font5x7[c - FONT5X7_FIRST] : font5x7[0];
        for (int gy = 0; gy < ch; gy++) {
            int r = gy / scale;
            uint16_t *row = s_fb + (y + gy) * W + x;
            for (int gx = 0; gx < cw; gx++) {
                int col = gx / scale;
                row[gx] = (col < 5 && r < 7 && ((cols[col] >> r) & 1)) ? f : b;
            }
        }
    }
}

/* Send the changed rows to the LCD. Rows are whole screen width, so a band
 * of the framebuffer is one contiguous run the DMA can read directly. */
static void flush(void) {
    if (s_dirty_y0 >= s_dirty_y1 || st7796_flush_busy()) return;
    int y0 = s_dirty_y0, y1 = s_dirty_y1;
    s_dirty_y0 = H; s_dirty_y1 = 0;
    st7796_flush_async(0, (uint16_t)y0, W - 1, (uint16_t)(y1 - 1), s_fb + y0 * W, NULL);
}

/* ------------------------------------------------------------------ text */
static char s_text[TEXT_MAX];
static int s_len, s_cur;            /* length, cursor index */
static int s_top;                   /* first visible line */
static int s_line[MAX_LINES], s_nlines;
static int s_goal_col = -1;         /* column to keep while moving up/down */

/* Word wrap: a line ends at a newline, or at the last space that lets the
 * next word fit; a word longer than a line is broken at the edge. */
static void layout(void) {
    s_nlines = 0;
    s_line[s_nlines++] = 0;
    int i = 0;
    while (i < s_len && s_nlines < MAX_LINES) {
        int start = i, last_space = -1, col = 0;
        while (i < s_len && s_text[i] != '\n' && col < COLS) {
            if (s_text[i] == ' ') last_space = i;
            i++; col++;
        }
        if (i < s_len && s_text[i] == '\n') i++;
        else if (i < s_len && col == COLS) {
            if (s_text[i] == ' ') i++;
            else if (last_space >= start) i = last_space + 1;
        } else break;
        s_line[s_nlines++] = i;
    }
}

static int line_of(int idx) {
    int k = 0;
    while (k + 1 < s_nlines && s_line[k + 1] <= idx) k++;
    return k;
}
static int line_end(int k) {           /* last index that belongs to line k (excl. newline) */
    int e = k + 1 < s_nlines ? s_line[k + 1] : s_len;
    if (e > s_line[k] && e <= s_len && s_text[e - 1] == '\n') e--;
    return e;
}

/* ------------------------------------------------------------------ state */
typedef enum { ST_EDITED, ST_SAVED, ST_NO_SD, ST_FULL, ST_ERROR, ST_LOADING } status_t;
static status_t s_status = ST_LOADING;
static bool s_dirty;                 /* text differs from the card */
static uint64_t s_last_edit_us;
static bool s_sd_ok;
static ow_device s_dev;              /* ~37 KB of link buffers: static, not on the stack */

static bool s_text_changed = true, s_bar_changed = true, s_kb_changed = true;

static void edited(void) {
    s_dirty = true;
    s_last_edit_us = time_us_64();
    s_status = ST_EDITED;
    s_text_changed = s_bar_changed = true;
    s_goal_col = -1;
}

static void insert(char c) {
    if (s_len >= TEXT_MAX - 1) { s_status = ST_FULL; s_bar_changed = true; return; }
    memmove(s_text + s_cur + 1, s_text + s_cur, (size_t)(s_len - s_cur));
    s_text[s_cur++] = c;
    s_len++;
    edited();
}
static void backspace(void) {
    if (!s_cur) return;
    memmove(s_text + s_cur - 1, s_text + s_cur, (size_t)(s_len - s_cur));
    s_cur--; s_len--;
    edited();
}
static void del(void) {
    if (s_cur >= s_len) return;
    s_cur++;
    backspace();
}

static void move_to(int idx, bool keep_goal) {
    if (idx < 0) idx = 0;
    if (idx > s_len) idx = s_len;
    s_cur = idx;
    if (!keep_goal) s_goal_col = -1;
    s_text_changed = true;
}
static void move_line(int dir) {
    layout();
    int k = line_of(s_cur);
    if (s_goal_col < 0) s_goal_col = s_cur - s_line[k];
    int nk = k + dir;
    if (nk < 0) { move_to(0, false); return; }
    if (nk >= s_nlines) { move_to(s_len, false); return; }
    int idx = s_line[nk] + s_goal_col;
    if (idx > line_end(nk)) idx = line_end(nk);
    move_to(idx, true);
}

/* ----------------------------------------------------------------- SD card */
static void sd_connect(void) {
    if (fw2_app_recovery_open_onewili(&s_dev) != OW_OK || fw2_app_recovery_wrap_sd() != OW_OK) {
        DIAG("notepad: no link to the MAIN processor\n");
        s_status = ST_NO_SD;
        return;
    }
    ow_sd_mkdir(&s_dev, NOTE_DIR);                 /* fails harmlessly if it exists */
    fw2_app_recovery_task();
    bool is_dir = false;
    uint32_t size = 0;
    ow_status st = ow_sd_stat(&s_dev, NOTE_PATH, &is_dir, &size);
    if (st != OW_OK) {
        s_sd_ok = st == OW_ERR_FAILED;             /* MAIN answered "not found": the card is there */
        DIAG("notepad: no %s yet (ow %d, sdfs %d)\n", NOTE_PATH, (int)st, (int)ow_sd_last_error());
        s_status = s_sd_ok ? ST_SAVED : ST_NO_SD;
        return;
    }
    size_t got = 0;
    if (ow_sd_get_mem(&s_dev, NOTE_PATH, s_text, TEXT_MAX - 1, &got) != OW_OK) {
        DIAG("notepad: read failed (sdfs %d)\n", (int)ow_sd_last_error());
        s_status = ST_ERROR;
        return;
    }
    s_sd_ok = true;
    s_len = (int)got;
    s_cur = s_len;
    s_status = ST_SAVED;
    DIAG("notepad: loaded %d bytes from %s\n", s_len, NOTE_PATH);
}

/* Written in 512-byte pieces: the OneWili client WiliBSP ships sends each
 * ow_sd_write() as one burst, and a long burst can overrun the MAIN
 * processor's 2 KB receive buffer while the card is busy. */
static bool sd_save(void) {
    if (!s_sd_ok) { s_status = ST_NO_SD; s_bar_changed = true; return false; }
    ow_sd_file f;
    bool ok = ow_sd_open(&s_dev, &f, NOTE_PATH, OW_SD_WRITE) == OW_OK;
    for (int off = 0; ok && off < s_len; off += 512) {
        int n = s_len - off < 512 ? s_len - off : 512;
        ok = ow_sd_write(&f, s_text + off, (size_t)n) == OW_OK;
        fw2_app_recovery_task();
    }
    if (ow_sd_close(&f) != OW_OK) ok = false;
    if (ok) {
        s_dirty = false;
        s_status = ST_SAVED;
        DIAG("notepad: saved %d bytes to %s\n", s_len, NOTE_PATH);
    } else {
        s_status = ST_ERROR;
        DIAG("notepad: save failed (sdfs %d)\n", (int)ow_sd_last_error());
    }
    s_bar_changed = true;
    return ok;
}

/* ---------------------------------------------------------------- keyboard */
enum { K_SHIFT = 1, K_BKSP, K_ENTER, K_MODE };
typedef struct { int16_t x, y, w; char code; } kb_key_t;
#define MAX_KEYS 40                   /* 33 per layer */
static kb_key_t s_keys[2][MAX_KEYS];
static int s_nkeys[2];
static int s_layer;                   /* 0 letters, 1 numbers & symbols */
static bool s_shift;
static int s_down_key = -1;           /* key under the finger */

static void add_key(int layer, int x, int row, int w, char code) {
    kb_key_t *k = &s_keys[layer][s_nkeys[layer]++];
    k->x = (int16_t)x; k->y = (int16_t)(KB_Y + row * KEY_H); k->w = (int16_t)w; k->code = code;
}
static void add_row(int layer, int row, int x, const char *chars) {
    for (; *chars; chars++, x += 48) add_key(layer, x, row, 48, *chars);
}
static void build_keys(void) {
    static const char *const rows[2][3] = {
        { "qwertyuiop", "asdfghjkl", "zxcvbnm" },
        { "1234567890", "-/:;()$&@", "?!'\"=+*" },
    };
    for (int l = 0; l < 2; l++) {
        add_row(l, 0, 0, rows[l][0]);
        add_row(l, 1, 24, rows[l][1]);
        add_key(l, 0, 2, 72, l == 0 ? K_SHIFT : '#');
        add_row(l, 2, 72, rows[l][2]);
        add_key(l, 408, 2, 72, K_BKSP);
        add_key(l, 0, 3, 72, K_MODE);
        add_key(l, 72, 3, 48, ',');
        add_key(l, 120, 3, 216, ' ');
        add_key(l, 336, 3, 48, '.');
        add_key(l, 384, 3, 96, K_ENTER);
    }
}

static char key_char(char code) {
    if (s_layer == 0 && s_shift && code >= 'a' && code <= 'z') return (char)(code - 'a' + 'A');
    return code;
}

static void key_label(const kb_key_t *k, char *out) {
    switch (k->code) {
    case K_SHIFT: strcpy(out, s_shift ? "SHIFT" : "shift"); break;
    case K_BKSP:  strcpy(out, "<del"); break;
    case K_ENTER: strcpy(out, "enter"); break;
    case K_MODE:  strcpy(out, s_layer ? "abc" : "123"); break;
    case ' ':     strcpy(out, "space"); break;
    default:      out[0] = key_char(k->code); out[1] = 0; break;
    }
}

static void draw_keyboard(void) {
    fb_rect(0, KB_Y, W, H - KB_Y, C_BG);
    for (int i = 0; i < s_nkeys[s_layer]; i++) {
        const kb_key_t *k = &s_keys[s_layer][i];
        bool fn = k->code < ' ' || k->code == ' ';
        uint16_t bg = i == s_down_key ? C_KEY_DOWN : (fn ? C_KEY_FN : C_KEY);
        if (k->code == K_SHIFT && s_shift) bg = C_KEY_DOWN;
        fb_rect(k->x + 1, k->y + 1, k->w - 2, KEY_H - 2, bg);
        char label[8];
        key_label(k, label);
        int n = (int)strlen(label), scale = n == 1 ? 3 : 2;
        int tw = n * 6 * scale, th = 8 * scale;
        fb_text(k->x + (k->w - tw) / 2, k->y + (KEY_H - th) / 2 + 1, scale, C_TEXT, bg, label, n);
    }
    mark(KB_Y, H);
}

static int key_at(int x, int y) {
    for (int i = 0; i < s_nkeys[s_layer]; i++) {
        const kb_key_t *k = &s_keys[s_layer][i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + KEY_H) return i;
    }
    return -1;
}

static void key_press(int i) {
    char code = s_keys[s_layer][i].code;
    switch (code) {
    case K_SHIFT: s_shift = !s_shift; break;
    case K_MODE:  s_layer ^= 1; s_shift = false; break;
    case K_BKSP:  backspace(); break;
    case K_ENTER: insert('\n'); break;
    default:
        insert(key_char(code));
        if (s_shift) s_shift = false;          /* one-shot */
        break;
    }
    s_kb_changed = true;
}

/* ------------------------------------------------------------ status bar */
static fw2kb_t s_kb;
static char s_chord_cache[5][6];

static void draw_bar(void) {
    fb_rect(0, 0, W, BAR_H, C_BAR);
    static const char *const words[] = { "edited", "saved", "no SD card", "note full", "SD error", "loading" };
    static const uint16_t cols[] = { C_WARN, C_OK, C_WARN, C_WARN, C_WARN, C_DIM };
    char left[32];
    int n = snprintf(left, sizeof left, "%d ", s_len);
    fb_text(4, 3, 2, C_TEXT, C_BAR, left, n);
    fb_text(4 + n * 12, 3, 2, cols[s_status], C_BAR, words[s_status], (int)strlen(words[s_status]));
    /* The chord keys: what each colour button types next. */
    const char *labels[5];
    fw2kb_get_labels(&s_kb, labels);
    for (int i = 0; i < 5; i++) {
        int x = 270 + i * 42;
        fb_rect(x, 3, 40, 16, k_chord_col[i]);
        int len = (int)strlen(labels[i]);
        if (len > 5) len = 5;
        fb_text(x + (40 - len * 6) / 2, 7, 1, k_chord_txt[i], k_chord_col[i], labels[i], len);
        memcpy(s_chord_cache[i], labels[i], (size_t)len);
        s_chord_cache[i][len] = 0;
    }
    mark(0, BAR_H);
}

static bool chord_labels_changed(void) {
    const char *labels[5];
    fw2kb_get_labels(&s_kb, labels);
    for (int i = 0; i < 5; i++) if (strncmp(labels[i], s_chord_cache[i], 5)) return true;
    return false;
}

/* ------------------------------------------------------------ text area */
static void draw_text(void) {
    layout();
    int ck = line_of(s_cur);
    if (ck < s_top) s_top = ck;
    if (ck >= s_top + ROWS) s_top = ck - ROWS + 1;
    fb_rect(0, BAR_H, W, KB_Y - BAR_H, C_BG);
    for (int r = 0; r < ROWS && s_top + r < s_nlines; r++) {
        int k = s_top + r;
        fb_text(0, TEXT_Y + r * LH, TEXT_SCALE, C_TEXT, C_BG, s_text + s_line[k], line_end(k) - s_line[k]);
    }
    int col = s_cur - s_line[ck];
    int row = ck - s_top;
    if (col >= COLS) { col = 0; row++; }
    if (row < ROWS) fb_rect(col * CW, TEXT_Y + row * LH - 1, 2, LH, C_CURSOR);
    if (s_nlines > ROWS) {                      /* where we are in a long note */
        int track = ROWS * LH, bar = track * ROWS / s_nlines;
        if (bar < 8) bar = 8;
        int pos = (track - bar) * s_top / (s_nlines - ROWS);
        fb_rect(W - 5, TEXT_Y + pos, 4, bar, C_SCROLL);
    }
    mark(BAR_H, KB_Y);
}

static void tap_text(int x, int y) {
    layout();
    int row = (y - TEXT_Y) / LH;
    if (row < 0) row = 0;
    int k = s_top + row;
    if (k >= s_nlines) { move_to(s_len, false); return; }
    int idx = s_line[k] + (x + CW / 2) / CW;
    if (idx > line_end(k)) idx = line_end(k);
    move_to(idx, false);
}

/* ------------------------------------------------------------------ input */
/* Held keys repeat: after 450 ms, every 70 ms. */
typedef struct { bool held; uint64_t next_us; } repeat_t;
static bool repeat_due(repeat_t *r, uint64_t now) {
    if (!r->held || now < r->next_us) return false;
    r->next_us = now + 70000;
    return true;
}
static repeat_t s_btn_rep[UARTKBD_BTN_COUNT], s_touch_rep;

static void button_action(uartkbd_btn_t b) {
    switch (b) {
    case UARTKBD_BTN_NAV_LEFT:   move_to(s_cur - 1, false); break;
    case UARTKBD_BTN_NAV_RIGHT:  move_to(s_cur + 1, false); break;
    case UARTKBD_BTN_NAV_UP:     move_line(-1); break;
    case UARTKBD_BTN_NAV_DOWN:   move_line(+1); break;
    case UARTKBD_BTN_NAV_CENTER: insert('\n'); break;
    case UARTKBD_BTN_CANCEL:     backspace(); break;
    default: break;
    }
}

static void handle_buttons(uint64_t now) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        repeat_t *r = &s_btn_rep[ev.btn];
        r->held = ev.pressed;
        if (!ev.pressed) continue;
        r->next_us = now + 450000;
        if (ev.btn <= UARTKBD_BTN_RED) fw2kb_press(&s_kb, (fw2kb_btn)ev.btn);   /* GREY..RED == GRAY..RED */
        else if (ev.btn == UARTKBD_BTN_PAGE) fw2kb_press(&s_kb, FW2KB_BTN_AI);   /* next chord page */
        else if (ev.btn == UARTKBD_BTN_OK) sd_save();
        else button_action(ev.btn);
    }
    for (int b = UARTKBD_BTN_NAV_UP; b <= UARTKBD_BTN_NAV_RIGHT; b++)
        if (repeat_due(&s_btn_rep[b], now)) button_action((uartkbd_btn_t)b);
    if (repeat_due(&s_btn_rep[UARTKBD_BTN_CANCEL], now)) backspace();
}

static void handle_touch(uint64_t now) {
    static bool was_down;
    uint16_t x, y;
    bool down = ft6336_poll(&x, &y);
    if (down && !was_down) {
        if (y >= KB_Y) {
            s_down_key = key_at(x, y);
            if (s_down_key >= 0) {
                key_press(s_down_key);
                s_touch_rep.held = s_keys[s_layer][s_down_key].code == K_BKSP;
                s_touch_rep.next_us = now + 450000;
            }
        } else if (y >= BAR_H) {
            tap_text(x, y);
        }
    } else if (!down && was_down) {
        s_down_key = -1;
        s_touch_rep.held = false;
        s_kb_changed = true;
    }
    if (down && repeat_due(&s_touch_rep, now)) backspace();
    was_down = down;
}

static void handle_chords(void) {
    fw2kb_event ev;
    while (fw2kb_next_event(&s_kb, &ev)) {
        switch (ev.key) {
        case FW2KB_KEY_CHAR:      insert(ev.ch); break;
        case FW2KB_KEY_BACKSPACE: backspace(); break;
        case FW2KB_KEY_DEL:       del(); break;
        case FW2KB_KEY_ENTER:     insert('\n'); break;
        case FW2KB_KEY_TAB:       for (int i = 0; i < 4; i++) insert(' '); break;
        case FW2KB_KEY_LEFT:      move_to(s_cur - 1, false); break;
        case FW2KB_KEY_RIGHT:     move_to(s_cur + 1, false); break;
        case FW2KB_KEY_UP:        move_line(-1); break;
        case FW2KB_KEY_DOWN:      move_line(+1); break;
        case FW2KB_KEY_HOME:      layout(); move_to(s_line[line_of(s_cur)], false); break;
        case FW2KB_KEY_END:       layout(); move_to(line_end(line_of(s_cur)), false); break;
        case FW2KB_KEY_SAVE:      sd_save(); break;
        default: break;
        }
    }
    if (chord_labels_changed()) s_bar_changed = true;
}

/* After the About screen (PAGE held 5 s) the whole screen is redrawn. */
static void redraw_all(void) { s_text_changed = s_bar_changed = s_kb_changed = true; }

int main(void) {
    board_init();                    /* before the OneWili link: uart_init reads clk_peri */
    fw2_app_recovery_init();
    if (psram_init() < sizeof s_fb) {
        DIAG("notepad: PSRAM missing, can't hold the screen\n");
        for (;;) { fw2_app_recovery_task(); tight_loop_contents(); }
    }
    st7796_init();
    fw2_app_about_use_lcd_restore(redraw_all);
    ft6336_init();
    fw2kb_init(&s_kb);
    agentio_init();                  /* WiliBSP's fw.py press / touch / type / screenshot */
    agentio_bind_keyboard(&s_kb);
    build_keys();

    fb_rect(0, 0, W, H, C_BG);
    draw_bar();
    draw_text();
    draw_keyboard();
    st7796_fill_screen(be(C_BG));
    board_backlight_set(1);
    flush();

    sd_connect();
    DIAG("notepad: ready\n");
    redraw_all();

    for (;;) {
        fw2_app_recovery_task();
        uint64_t now = time_us_64();
        handle_buttons(now);
        handle_touch(now);
        handle_chords();

        if (s_dirty && s_sd_ok && time_us_64() - s_last_edit_us >= AUTOSAVE_MS * 1000ull) sd_save();

        if (!st7796_flush_busy()) {                 /* draw only between flushes: no tearing */
            if (s_bar_changed)  { draw_bar();      s_bar_changed = false; }
            if (s_text_changed) { draw_text();     s_text_changed = false; }
            if (s_kb_changed)   { draw_keyboard(); s_kb_changed = false; }
            flush();
        }
        agentio_task();
        fw2_app_recovery_sleep_ms(2);
    }
}
