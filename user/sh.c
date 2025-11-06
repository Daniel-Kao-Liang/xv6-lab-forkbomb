// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
  int back; // Custom flag: 1 if this EXEC is in background
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);

// Global job list
int bg_jobs[NPROC];

// Helper: Add a job
void add_job(int pid) {
    for (int i = 0; i < NPROC; i++) {
        if (bg_jobs[i] == 0) {
            bg_jobs[i] = pid;
            return;
        }
    }
}

// Helper: Remove a job
void remove_job(int pid) {
    for (int i = 0; i < NPROC; i++) {
        if (bg_jobs[i] == pid) {
            bg_jobs[i] = 0;
            return;
        }
    }
}

// Helper: Check if a PID is a background job
int is_bg_job(int pid) {
    for (int i = 0; i < NPROC; i++) {
        if (bg_jobs[i] == pid) {
            return 1;
        }
    }
    return 0;
}

// Helper: Print jobs
void print_jobs() {
    for (int i = 0; i < NPROC; i++) {
        if (bg_jobs[i] != 0) {
            printf("%d\n", bg_jobs[i]);
        }
    }
}

// Helper: Process a reaped job
void reap_background_job(int pid, int status) {
    if(is_bg_job(pid)) {
        remove_job(pid);
        printf("[bg %d] exited with status %d\n", pid, status);
    }
}

// Helper: Reap all available zombie background jobs
void reap_zombies() {
    int status;
    int pid;
    while ((pid = wait_noblock(&status)) > 0) {
        reap_background_job(pid, status);
    }
}

// Helper: Wait for a specific foreground process
// This is critical for Case 6
void wait_for_foreground(int foreground_pid) {
    int status;
    int reaped_pid;
    
    // Wait for *a* child to exit
    while((reaped_pid = wait(&status)) > 0) {
        if (reaped_pid == foreground_pid) {
            // Our foreground job finished. We're done.
            break;
        } else {
            // We accidentally reaped a background job.
            // Process it and keep waiting.
            reap_background_job(reaped_pid, status);
        }
    }
}


// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;
  int pid;
  int pid_left;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);

    // Handle built-in 'cd'
    if(strcmp(ecmd->argv[0], "cd") == 0){
        if(ecmd->argv[1] == 0 || chdir(ecmd->argv[1]) < 0)
            fprintf(2, "cannot cd %s\n", ecmd->argv[1] ? ecmd->argv[1] : "");
        // 'cd' is built-in, so we don't exit the shell
        return; 
    }

    // Handle built-in 'jobs'
    if(strcmp(ecmd->argv[0], "jobs") == 0){
        print_jobs();
        return;
    }

    // Fork for the exec
    pid = fork1();
    if(pid == 0) { // Child
      exec(ecmd->argv[0], ecmd->argv);
      fprintf(2, "exec %s failed\n", ecmd->argv[0]);
      exit(0); // Use 0 for failure
    }
    
    // Parent
    if(ecmd->back){
        // This is a background job
        add_job(pid);
        printf("[%d]\n", pid);
    } else {
        // This is a foreground job
        wait_for_foreground(pid);
    }
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      return;
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    // We run the left side.
    // If it's foreground, we wait. If background, we don't.
    // The fork/wait logic is handled *inside* the recursive call.
    runcmd(lcmd->left);
    
    // Then we run the right side.
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    
    pid_left = fork1();
    if(pid_left == 0){ // Left child
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
      exit(0);
    }

    int pid_right = fork1();
    if(pid_right == 0){ // Right child
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
      exit(0);
    }
    
    close(p[0]);
    close(p[1]);

    // This pipe command might be in the background
    // We check the 'back' flag on the *left* side (arbitrary, both are part of pipe)
    int is_background = 0;
    if(pcmd->left->type == EXEC)
        is_background = ((struct execcmd*)pcmd->left)->back;
    else if (pcmd->right->type == EXEC)
        is_background = ((struct execcmd*)pcmd->right)->back;

    if(is_background){
        // Lab requires printing PID of the first command
        add_job(pid_left);
        // We also add the right job, so it can be reaped
        // but we only print the first one.
        add_job(pid_right); 
        printf("[%d]\n", pid_left);
    } else {
        // Wait for both foreground children
        wait_for_foreground(pid_left);
        wait_for_foreground(pid_right);
    }
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;

    // Mark the inner command as 'background'
    // This is a bit of a hack, but `parsecmd` doesn't do it.
    // We only need to handle EXEC and PIPE.
    if(bcmd->cmd->type == EXEC) {
        ((struct execcmd*)bcmd->cmd)->back = 1;
    } else if (bcmd->cmd->type == PIPE) {
        struct pipecmd* p = (struct pipecmd*)bcmd->cmd;
        if(p->left->type == EXEC) ((struct execcmd*)p->left)->back = 1;
        if(p->right->type == EXEC) ((struct execcmd*)p->right)->back = 1;
    }
    
    runcmd(bcmd->cmd);
    break;
  }
  
  // Only the initial shell process should reach here.
  // Child processes will exit(1) or return from built-ins.
}

int
getcmd(char *buf, int nbuf, int fd)
{
  // if(fd == 0) {
  //  // Interactive mode
  //  fprintf(2, "$ ");
  // }
  
  memset(buf, 0, nbuf);
  
  // Read a line from fd
  char *p = buf;
  int i = 0;
  while(i < nbuf - 1) {
    if(read(fd, p, 1) != 1) {
      if (i == 0) // EOF or error
        return -1;
      break; // End of input
    }
    if(*p == '\n')
      break;
    p++;
    i++;
  }
  *p = '\0'; // Null-terminate

  if(buf[0] == 0) // Handle empty input or EOF
    return -1;

  return 0;
}

int
main(int argc, char *argv[])
{
  static char buf[100];
  int fd;

  // Init job list
  for(int i = 0; i < NPROC; i++)
    bg_jobs[i] = 0;

  // Shell script execution (Step 4)
  if(argc > 1){
    // Run from script
    if((fd = open(argv[1], O_RDONLY)) < 0){
      fprintf(2, "sh: cannot open %s\n", argv[1]);
      exit(1);
    }
  } else {
    // Interactive mode
    fd = 0;
  }


  while(1){
    // 1. (僅限互動模式) 收割上一輪的僵屍
    //    這保證 `[bg ...]` 訊息在 `$` 之前印出
    // if(fd == 0) {
    //     reap_zombies();
    // }

    // 2. (僅限互動模式) 印出提示符
    if(fd == 0) {
        fprintf(2, "$ ");
    }
    
    // 3. 讀取命令 (getcmd 內部已不再印出 $)
    if(getcmd(buf, sizeof(buf), fd) < 0){
      break; // EOF
    }
    

    if(buf[0] == '\n' || buf[0] == 0) // Empty line
      continue;

    // 4. 執行命令
    runcmd(parsecmd(buf));

    if(fd == 0){
      reap_zombies();

    }
  }
  
  exit(0);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}


//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}