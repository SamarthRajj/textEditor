#include <iostream>
#include <unistd.h>
#include <termios.h>
#include <string>
using namespace std;

void enableRawMode(){
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);

    raw.c_iflag &= ~(ECHO);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(){
    enableRawMode();
    char c;
    while(c != 'q'){
        cin >> c;
    }
    return 0;
}