/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/*** defines ***/

#define CEREAL_VERSION "0.0.1"
#define CEREAL_TAB_STOP 8
#define CEREAL_QUIT_TIMES 3

// CTRL key strips bits 5 and 6 from the key pressed in combination with CTRL.
// This behavier is reproduced using Bitmasking with 0x1f, that is 00011111.
#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    DEL_KEY
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

/*** data ***/

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
    // clear screen on exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

// restore terminal's original attributes on exit
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    // use bitwise-NOT operator (~) to disable the following

    // ECHO -> print input onto the screen :: ECHO is a bitflag defined as
    // 000..0001000 ICANON -> canonical mode, reading input byte-by-byte instead
    // of line-by-line ISIG -> controls C-c SIGINT and C-z SIGSTP signal IEXTEN ->
    // disable C-v
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // IXON -> software flow control C-s and C-q
    // ICRNL -> Input Carriage Return New Line :: C-m
    // BRKINT, INPICK, ISTRIP -> tradition to turn off in raw mode
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);

    // tradition
    raw.c_iflag |= (CS8);

    // OPOST -> Post processing of output :: \n to \r\n
    raw.c_oflag &= ~(OPOST);

    // timeout for read
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // TCSAFLUSH allows the leftover input no longer fed into the shell
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// reads and handles key
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // \x1b is <esc>, or 27 in terminal. <esc>[ following specific commands forms
    // the VT100 escape sequence. It instructs the terminal to do various text
    // formatting tasks.
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
            read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        } else if (seq[0] == '0') {
            switch (seq[1]) {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        ++i;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    // if ioctl failed or result is 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

void editorUpdateSyntax (erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);
    for (int i = 0; i < row->rsize; ++i) {
        if (isdigit(row->render[i])) {
            row->hl[i] = HL_NUMBER;
        }
    }
}

// ANSI Colors. See https://en.wikipedia.org/wiki/ANSI_escape_code#Colors
int editorSyntaxToColor (int hl) {
    switch (hl){
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
    }
}

/*** row operations ***/

// convert chars-x to render-x
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; ++j) {
        if (row->chars[j] == '\t') {
            rx += (CEREAL_TAB_STOP - 1) - (rx % CEREAL_TAB_STOP);
        }
        ++rx;
    }
    return rx;
}

// convert render-x to chars-x
int editorRowRxToCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; ++cx){
        if (row->chars[cx] == '\t'){
            cur_rx += (CEREAL_TAB_STOP - 1) - (cur_rx % CEREAL_TAB_STOP);
        }
        ++cur_rx;
        if (cur_rx > rx){
            return cx;
        }
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            ++tabs;
        }
    }
    free(row->render);
    row->render = malloc(row->size + tabs * (CEREAL_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % CEREAL_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    ++E.numrows;
    ++E.dirty;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at){
    if(at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    --E.numrows;
    ++E.dirty;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    ++row->size;
    row->chars[at] = c;
    editorUpdateRow(row);
    ++E.dirty;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    ++E.dirty;
}

void editorInsertNewline() {
    if (E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    ++E.cy;
    E.cx = 0;
}

void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    --row->size;
    editorUpdateRow(row);
    ++E.dirty;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    ++E.cx;
}

void editorDelChar(){
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if(E.cx > 0){
        editorRowDelChar(row, E.cx - 1);
        --E.cx;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        --E.cy;
    }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; ++j) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; ++j) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        ++p;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            --linelen;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    // new file
    if(E.filename == NULL) {
        E.filename = editorPrompt("Save as : %s (ESC or C-g to cancel)", NULL);
        if (E.filename == NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);
    // create a new file if it doesn't already exist (O_CREAT),
    //  then open it for reading and writing (O_RDWR)
    // 0644 is the standard permission for text files needed due to O_CREAT flag
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1){
        if(ftruncate(fd, len) != -1){ // sets the file size to len
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** search ***/

void editorSearchCallback(char *query, int key){
   static int last_match = -1;
   static int direction = 1;

   static int saved_hl_line;
   static char *saved_hl = NULL;

   if (saved_hl) {
       memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
       free(saved_hl);
       saved_hl = NULL;
   }

    if (key == '\r' || key == '\x1b' || key == CTRL_KEY('g')){
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN ||
               key == CTRL_KEY('n') || key == CTRL_KEY('s')){
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP ||
               key == CTRL_KEY('p') || key == CTRL_KEY('r')){
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1){
        direction = 1;
    }
    int current = last_match;
    for (int i = 0; i < E.numrows; ++i){
        current += direction;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }
        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if(match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorSearch() {
    int orig_cx = E.cx;
    int orig_cy = E.cy;
    int orig_coloff = E.coloff;
    int orig_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC or C-g to Cancel | C-s to Search Forward | C-r to Search Backward)", editorSearchCallback);

    if (query){
        free(query);
    } else {
        // cancel search
        E.cx = orig_cx;
        E.cy = orig_cy;
        E.coloff = orig_coloff;
        E.rowoff = orig_rowoff;
    }
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT                               \
    { NULL, 0 }

// append buffer ab
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }

    // copy from s to new
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

void editorScroll() {
    E.rx = 0;

    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

// handles how drawing each row of the buffer of text being edited
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; ++y) {
        int filerow = y + E.rowoff; // vertical scroll
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Welcome to Cereal v%s", CEREAL_VERSION);
                if (welcomelen > E.screencols) {
                    welcomelen = E.screencols;
                }
                // center msg: divide the screen then subtract half of string's length
                int padding = (E.screencols - welcomelen) / 2;
                // add ~ to first col
                if (padding) {
                    abAppend(ab, "~", 1);
                    --padding;
                }
                // add spaces
                while (padding--) {
                    abAppend(ab, " ", 1);
                }
                // append welcome to ab
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            for (int j = 0; j < len; ++j){
                if (hl[j] == HL_NORMAL) {
                    if (current_color != -1){
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        // clear 1 line at a time
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // <esc>[7m switches to inverted colors
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s %s",
                       E.filename ? E.filename : "[No Name]",
                       E.dirty ? "(modified)" : "");
    int rlen =
        snprintf(rstatus, sizeof(rstatus), "line %d of %d", E.cy + 1, E.numrows);
    if (len > E.screencols) {
        len = E.screencols;
    }
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            ++len;
        }
    }
    abAppend(ab, "\x1b[m", 3); // switches back
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    // first byte is \x1b (escape sequence)
    // escape sequence instructs terminal to do varias text formatt

    // hide cursor before displaying onto screen
    abAppend(&ab, "\x1b[?25l", 6);
    // 3 bytes long, H command cursor position <esc>[1;1H default
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // print moving cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1); // terminal uses 1-indexed values
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == '\x1b' || c == CTRL_KEY('g')){
            editorSetStatusMessage("");
            if (callback){
                callback(buf, c);
            }
            free(buf);
            return NULL;
        }else if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen != 0){
                buf[--buflen] = '\0';
            }
        }else if (c == '\r') {
            if(buflen != 0){
                editorSetStatusMessage("");
                if (callback){
                    callback(buf, c);
                }
                return buf;
            }
        }else if(!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback){
            callback(buf, c);
        }
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_UP:
        if (E.cy != 0) {
            --E.cy;
        } else if (E.cy > 0) {
            --E.cy;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) {
            ++E.cy;
        }
        break;
    case ARROW_LEFT:
        if (E.cx != 0) {
            --E.cx;
        } else if (E.cy > 0) {
            // move cursor up
            --E.cy;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            ++E.cx;
        } else if (row && E.cx == row->size) {
            ++E.cy;
            E.cx = 0;
        }
        break;
    }

    // cursor to the end of line when necessary
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    static int quit_times = CEREAL_QUIT_TIMES;

    int c = editorReadKey();

    // Use EMACS bindings
    switch (c) {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('a'):
        c = HOME_KEY;
        break;
    case CTRL_KEY('e'):
        c = END_KEY;
        break;
    case CTRL_KEY('p'):
        c = ARROW_UP;
        break;
    case CTRL_KEY('n'):
        c = ARROW_DOWN;
        break;
    case CTRL_KEY('f'):
        c = ARROW_RIGHT;
        break;
    case CTRL_KEY('b'):
        c = ARROW_LEFT;
        break;
    case CTRL_KEY('v'):
        c = PAGE_DOWN;
        break;
    case CTRL_KEY('d'):
        c = DEL_KEY;
    }

    switch (c) {
    case CTRL_KEY('q'):
        if(E.dirty && quit_times > 0){
            editorSetStatusMessage("WARNING! File has unsaved changes. "
                                   "Process C-q %d more times to REAL quit.", quit_times);
            --quit_times;
            return;
        }
        // clear screen on exit
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;

    case CTRL_KEY('x'): {
        int c2 = editorReadKey();
        switch (c2) {
        case CTRL_KEY('s'):
            editorSave();
            break;
        }
        break;
    }

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        }
        break;

    case CTRL_KEY('s'):
        editorSearch();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY){
            editorMoveCursor(ARROW_RIGHT);
        }
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows) {
                E.cy = E.numrows;
            }
        }
        int times = E.screenrows;
        while (times--) {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    } break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

        // ignore C-l and ECTRLSC
    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = CEREAL_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getwindowsize");
    }
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Save with C-x C-s | Quit with C-q | Search with C-s");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
