#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void enableRawMode() {
  struct termios raw;

  tcgetattr(STDIN_FILENO, &raw);
  raw.c_lflag &= ~(ECHO);

  int i = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  printf("%d\n",i);
}

int main() {
  enableRawMode();
  char c;
  while (_read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
    //printf("%d - %c\n", c , c);
    if (iscntrl(c)) {
      printf("%d\n", c);
    } else {
      printf("%d ('%c')\n", c, c);
    }
  }
  return 0;
}
