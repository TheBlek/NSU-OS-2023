#define MAXARGS 256
#define MAXCMDS 50

struct command {
    char *cmdargs[MAXARGS];
    char cmdflag;
    char *infile, *outfile;
};

/*  cmdflag's  */
#define OUTPIPE    0x01
#define OUTFILE    0x02
#define OUTFILEAP  0x04
#define INFILE     0x08
#define INPIPE     0x10

#define OUTREDIR   0x07

extern struct command cmds[];
extern char *infile, *outfile, *appfile;
extern char bkgrnd;

int parseline(char *);
int promptline(char *, char *, int);
