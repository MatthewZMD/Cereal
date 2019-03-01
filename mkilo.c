/*** includes ***/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct termios orig_termios;

/*** terminal ***/

void die(const char *s){
  perror(s);
  exit(1);
}

// restore terminal's original attributes on exit
void disableRawMode(){
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1){
    die("tcsetattr");
  }
}

void enableRawMode(){
  if(tcgetattr(STDIN_FILENO, &orig_termios) == -1){
    die("tcgetattr");
  }
  atexit(disableRawMode);

  struct termios raw = orig_termios;
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


/*** init ***/

int main() {
  enableRawMode();

  while (1){
    char c = '\0';
    if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN){
      die("read");
    }
    // iscntrl tests whether a character is a control character
    if(iscntrl(c)){
      printf("%d\r\n", c);
    }else {
      printf("%d ('%c')\r\n", c , c);
    }
    if (c == CTRL_KEY('q')){
      break;
    }
  }

  return 0;
}
