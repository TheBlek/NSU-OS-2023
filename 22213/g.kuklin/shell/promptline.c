#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"

int promptline(char *prompt, char *line, int sizline) {
    int n;
start:
    n = 0;

    write(1, prompt, strlen(prompt));
    while (1) {
        int more = read(0, (line + n), sizline-n);
        if (more < 0 && errno == EINTR) {
            char newline = '\n';
            // Another interrupt may occur
            while(write(1, &newline, 1) != 1) {};
            goto start;
        }
        n += more;
        *(line+n) = '\0';
        /*
         *  check to see if command line extends onto
         *  next line.  If so, append next line to command line
         */

        if (*(line+n-2) == '\\' && *(line+n-1) == '\n') {
            *(line+n) = ' ';
            *(line+n-1) = ' ';
            *(line+n-2) = ' ';
            continue;   /*  read next line  */
        }
        return(n);      /* all done */
    }
}
