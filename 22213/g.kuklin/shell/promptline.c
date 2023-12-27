#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "shell.h"

int promptline(char *prompt, char *line, int sizline) {
    int n = 0;

    int written = 0;
    int target = strlen(prompt);
    do {
        int res = write(1, prompt, target - written);
        if (res == -1) {
            perror("Failed to write next prompt");
            exit(1);
        }
        written += res;
    } while (written < target);
    int stray_quote = 0;
    while (1) {
        int more = read(0, (line + n), sizline-n);
        if (more == -1) {
            perror("Failed to read next prompt");
            exit(1);
        }
        if (more == 0) return -1; // EOF

        for (int i = n; i < more + n; i++)
            stray_quote = (stray_quote + (*(line + i) == '"')) % 2;
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

        /* Command is not finished yet */
        if (*(line+n-1) != '\n' || stray_quote) {
            continue; 
        }
        return(n);      /* all done */
    }
}
