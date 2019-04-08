/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

/*** defines ***/

#define MKILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_UP = 1000,
  ARROW_LEFT,
  ARROW_DOWN,
  ARROW_RIGHT,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
  DEL_KEY
};

/*** data ***/

typedef struct erow{
  int size;
  int rsize;
  char * chars;
  char * render;
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
  struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s){
  // clear screen on exit
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

// restore terminal's original attributes on exit
void disableRawMode(){
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
    die("tcsetattr");
  }
}

void enableRawMode(){
  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
    die("tcgetattr");
  }
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  // use bitwise-NOT operator (~) to disable the following

  // ECHO -> print input onto the screen :: ECHO is a bitflag defined as 000..0001000
  // ICANON -> canonical mode, reading input byte-by-byte instead of line-by-line
  // ISIG -> controls C-c SIGINT and C-z SIGSTP signal
  // IEXTEN -> disable C-v
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
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
    die("tcsetattr");
  }
}

// reads and handles key
int editorReadKey(){
  int nread;
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if(nread == -1 && errno != EAGAIN){
      die("read");
    }
  }

  if(c == '\x1b'){
    char seq[3];

    if (read (STDIN_FILENO, &seq[0], 1) != 1 || read (STDIN_FILENO, &seq[1], 1) != 1){
      return '\x1b';
    }

    if(seq[0] == '['){
      if(seq[1] >= '0' && seq[1] <= '9'){
        if(read(STDIN_FILENO, &seq[2], 1) != 1){
          return '\x1b';
        }
        if(seq[2] == '~'){
          switch(seq[1]){
          case '1': return HOME_KEY;
          case '3': return DEL_KEY;
          case '4': return END_KEY;
          case '5': return PAGE_UP;
          case '6': return PAGE_DOWN;
          case '7': return HOME_KEY;
          case '8': return END_KEY;
          }
        }
      }else{
        switch (seq[1]){
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
        }
      }
    }else if(seq[0] == '0'){
      switch(seq[1]){
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      }
    }
    return '\x1b';
  }else{
    return c;
  }
}

int getCursorPosition(int *rows, int *cols){
  char buf[32];
  unsigned int i = 0;

  if(write(STDOUT_FILENO, "\1xb[6n", 4) != 4){
    return -1;
  }

  while(i < sizeof(buf) - 1){
    if(read(STDIN_FILENO, &buf[i], 1) != 1){
      break;
    }
    if(buf[i] == 'R'){
      break;
    }
    ++i;
  }

  buf[i] = '\0';

  if(buf[0] != '\x1b' || buf[1] != '['){
    return -1;
  }
  if(sscanf(&buf[2], "%d;%d", rows, cols) != 2){
    return -1;
  }

  return 0;
}

int getWindowSize(int *rows, int *cols){
  struct winsize ws;

  // if ioctl failed or result is 0;
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12){
      return -1;
    }else{
      editorReadKey();
    }
    return getCursorPosition(rows, cols);
  }else{
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
  int rx = 0;
  int j;
  for (j = 0; j < cx; ++j){
    if(row -> chars[j] == '\t'){
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    ++rx;
  }
  return rx;
}

void editorUpdateRow(erow *row){
  int tabs = 0;
  int j;
  for(j = 0; j < row->size; ++j){
    if(row->chars[j] == '\t'){
      ++tabs;
    }
  }
  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) +1);

  int idx = 0;
  for(j = 0; j < row->size; ++j){
    if(row->chars[j] == '\t'){
      row->render[idx++] = ' ';
      while(idx % KILO_TAB_STOP != 0){
        row->render[idx++] = ' ';
      }
    } else {
      row->render[++idx] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len){
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  ++E.numrows;
}

/*** file i/o ***/

void editorOpen(char *filename){
  FILE *fp = fopen(filename, "r");
  if(!fp){
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen = 13;

  linelen = getline(&line, &linecap, fp);
  while((linelen = getline(&line, &linecap, fp)) != -1){
    while(linelen > 0 && (line[linelen -1] == '\n' || line[linelen -1] == '\r')){
      --linelen;
    }
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

// append buffer ab
void abAppend(struct abuf *ab, const char *s, int len){
  char *new = realloc(ab->b, ab->len + len);

  if(new==NULL){
    return;
  }

  // copy from s to new
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab){
  free(ab->b);
}

/*** output ***/

void editorScroll(){
  E.rx = E.cx;

  if(E.cy < E.numrows){
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if(E.cy < E.rowoff){
    E.rowoff = E.cy;
  }
  if(E.cy >= E.rowoff + E.screenrows){
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if(E.rx < E.coloff){
    E.coloff = E.rx;
  }
  if(E.rx >= E.coloff + E.screencols){
    E.coloff = E.rx - E.screencols +1;
  }
}

// handles how drawing each row of the buffer of text being edited
void editorDrawRows(struct abuf *ab){
  int y;
  for (y = 0; y < E.screenrows; ++y){
    int filerow = y + E.rowoff; // vertical scroll
    if(filerow >= E.numrows){
      if(E.numrows == 0 && y == E.screenrows / 3){
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Welcome to M-Kilo v%s", MKILO_VERSION);
        if(welcomelen > E.screencols){
          welcomelen = E.screencols;
        }
        // center msg: divide the screen then subrract half of string's length
        int padding = (E.screencols - welcomelen) / 2;
        // add ~ to first col
        if(padding){
          abAppend(ab, "~", 1);
          --padding;
        }
        // add spaces
        while(padding--){
          abAppend(ab, " ", 1);
        }
        // append welcome to ab
        abAppend(ab, welcome, welcomelen);
      }else{
        abAppend(ab, "~", 1);
      }
    }else{
      int len = E.row[filerow].rsize - E.coloff;
      if(len < 0){
        len = 0;
      }
      if (len > E.screencols){
        len = E.screencols;
      }
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    // clear 1 line at a time
    abAppend(ab, "\x1b[K", 3);
    if(y < E.screenrows -1){
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen(){
  editorScroll();

  struct abuf ab = ABUF_INIT;
  // first byte is \x1b (escape sequence)
  // escape sequence instructs terminal to do varias text formatt

  //hide cursor before displaying onto screen
  abAppend(&ab, "\x1b[?25l", 6);
  // 3 bytes long, H command cursor position <esc>[1;1H default
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  // print moving cursor
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1); // terminal uses 1-indexed values
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key){
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key){
  case ARROW_UP:
    if(E.cy != 0){
      --E.cy;
    } else if (E.cy > 0){
      --E.cy;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_DOWN:
    if(E.cy < E.numrows){
      ++E.cy;
    }
    break;
  case ARROW_LEFT:
    if(E.cx != 0){
      --E.cx;
    }else if(E.cy > 0){
      // move cursor up
      --E.cy;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if(row && E.cx < row->size){
      ++E.cx;
    }else if(row && E.cx == row->size){
      ++E.cy;
      E.cx = 0;
    }
    break;
  }

  // cursor to the end of line when necessary
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if(E.cx > rowlen){
    E.cx = rowlen;
  }
}

void editorProcessKeypress(){
  int c = editorReadKey();

  switch(c){
  case CTRL_KEY('q'):
    // clear screen on exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    exit(0);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    E.cx = E.screencols - 1;
    break;

  case PAGE_UP:
  case PAGE_DOWN:
    {
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      }else if (c == PAGE_DOWN){
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > E.numrows){
          E.cy = E.numrows;
        }
      }
      int times = E.screenrows;
      while (times--){
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    }
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}


/*** init ***/

void initEditor(){
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;

  if(getWindowSize(&E.screenrows, &E.screencols) == -1){
    die("getwindowsize");
  }
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if(argc >= 2){
    editorOpen(argv[1]);
  }

  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
