#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

// restore terminal's original attributes on exit
void disableRawMode(){
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode(){
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  /* use bitwise-NOT operator (~) to disable */
  /* ECHO -> print input onto the screen :: ECHO is a bitflag defined as 000..0001000 */
  /* ICANON ->  canonical mode, reading input byte-by-byte instead of line-by-line*/
  raw.c_lflag &= ~(ECHO | ICANON);

  // TCSAFLUSH allows the leftover input no longer fed into the shell
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
  enableRawMode();
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q'){
    // iscntrl tests whether a character is a control character
    if(iscntrl(c)){
      printf("%d\n", c);
    }else {
      printf("%d ('%c')\n", c , c);
    }
  }
  return 0;
}
