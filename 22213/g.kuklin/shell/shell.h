#define MAXARGS 256
#define MAXCMDS 50
#define MAXSQNCS 16

struct command {
    char *cmdargs[MAXARGS];
    char cmdflag;
    char *infile, *outfile;
};

struct command_sequence {
    struct command *cmds;
    int cnt;
    char background;
};

/*  cmdflag's  */
#define OUTPIPE    0x01
#define OUTFILE    0x02
#define OUTFILEAP  0x04
#define INFILE     0x08
#define INPIPE     0x10

#define OUTREDIR   0x07

extern struct command_sequence cmds[];

int parseline(char *, struct command_sequence *);
int promptline(char *, char *, int);
