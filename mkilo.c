/*** includes ***/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
struct editorConfig {
  int screenrows;
  int screencols;
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
char editorReadKey(){
  int nread;
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if(nread == -1 && errno != EAGAIN){
      die("read");
    }
  }
  return c;
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

  printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);

  editorReadKey();

  return -1;
}

int getWindowSize(int *rows, int *cols){
  struct winsize ws;

  // if ioctl failed or result is 0;
  if(1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
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

/*** output ***/

// handles how drawing each row of the buffer of text being edited
void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; ++y){
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

void editorRefreshScreen(){
  // write 4 bytes, first byte is \x1b (escape sequence)
  // escape sequence instructs terminal to do varias text formatting
  // J command is erase in display, 2 means clear entire screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // 3 bytes long, H command cursor position <esc>[1;1H default
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();

  write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

void editorProcessKeypress(){
  char c = editorReadKey();

  switch(c){
  case CTRL_KEY('q'):
    // clear screen on exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    exit(0);
    break;
  }
}


/*** init ***/

void initEditor(){
  if(getWindowSize(&E.screenrows, &E.screencols) == -1){
    die("getwindowsize");
  }
}

int main() {
  enableRawMode();
  initEditor();

  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
