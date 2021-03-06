#define _GNU_SOURCE // for clone(); has to be before the #includes

#include <stdio.h>
#include <stdlib.h> // malloc()
#include <sched.h> // clone()
#include <errno.h> // errno
#include <unistd.h> // dup(), getpid(), STDIN_FILENO, STDOUT_FILENO, execl()
#include <fcntl.h> // open()
#include <signal.h> // SIGCHLD
#include <sys/wait.h> // waitpid()
#include <sys/types.h> // pid_t, open(), waitpid()
#include <sys/stat.h> // open()
#include <sys/eventfd.h> // eventfd()
#include <stdint.h>
#include <seccomp.h>

#include "logger.h"
#include "syscall_manager.h"
#include "sandbox.h"
#include "resource_limits.h"
#include "terminate.h"

#define EXIT_CHILD_FAILURE 1
#define SB_VERBOSE

typedef struct ChildPayload {
  const char *exect_path;
  const char *jail_path;
  const char *input_file;
  const char *output_file;
  const char *whitelist;
  scmp_filter_ctx *ctx;
  int notify_c;
  int notify_p;
  uid_t uid;
  gid_t gid;
} ChildPayload;

static int childFunc(void *arg) {

  ChildPayload *cp = (ChildPayload *)arg;
  int in = open(cp -> input_file, O_RDONLY);
  if (in == -1) {
    printErr(__FILE__, __LINE__, "open failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  int out = open(cp -> output_file, O_WRONLY | O_CREAT | O_TRUNC);
  if (out == -1) {
    printErr(__FILE__, __LINE__, "open failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  // Redirect stdio of child process
  if (dup2(in, STDIN_FILENO) == -1) {
    printErr(__FILE__, __LINE__, "dup2 failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (dup2(out, STDOUT_FILENO) == -1) {
    printErr(__FILE__, __LINE__, "dup2 failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (close(in) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (close(out) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }

  // Notify parent to set resource limits and start accounting time, set
  // memory limits etc.
  uint64_t u = 1;
  // Notifies parent which then sets resource limits
  if (write(cp -> notify_p, &u, sizeof(uint64_t)) == -1) {
    printErr(__FILE__, __LINE__, "write failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  // blocks until resource limits are set in the parent and the parent
  // notifies
  if (read(cp -> notify_c, &u, sizeof(uint64_t)) == -1) {
    printErr(__FILE__, __LINE__, "read failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (close(cp -> notify_c) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (close(cp -> notify_p) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }

  // This is required because call to 'installSysCallBlocker' is made after
  // 'chroot' and the former requires to access contents of 'whitelist'
  // O_CLOEXEC is not really necessary since we're trying to close in
  // anyway in 'installSysCallBlocker'
  int whitelist_fd = open(cp -> whitelist, O_RDONLY | O_CLOEXEC);
  if (whitelist_fd == -1) {
    printErr(__FILE__, __LINE__, "open failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }

  // 'chdir', 'chroot' and drop privileges
  if (chdir(cp -> jail_path) == -1) {
    printErr(__FILE__, __LINE__, "chdir failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (chroot("./") == -1) {
    printErr(__FILE__, __LINE__, "chroot failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  // uid and gid persist even after exec and child processes inherit these
  // from the parent process. Hence all the processes of the executable will
  // have this uid and gid
  // First gid must be set and only then uid
  if (setgid(cp -> gid) == -1) {
    printErr(__FILE__, __LINE__, "setgid failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }
  if (setuid(cp -> uid) == -1) {
    printErr(__FILE__, __LINE__, "setuid failed", 1, errno);
    return EXIT_CHILD_FAILURE;
  }

  // System calls not in whitelist follow action that was specified in the
  // call to 'seccomp_init'
  if (installSysCallBlocker(cp -> ctx, whitelist_fd) == -1) {
    return EXIT_CHILD_FAILURE;
  }

  if (execl(cp -> exect_path, cp -> exect_path, (char *)NULL) == -1) {
    printErr(__FILE__, __LINE__, "execl failed", 1, errno);
  }
  return EXIT_CHILD_FAILURE;
}

static int sandboxExecFailCleanup(
  int notify_p, int notify_c, char *child_stack) {

  free(child_stack);
  int ret = 0;
  if (close(notify_p) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    ret = -1;
  }
  if (close(notify_c) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
    ret = -1;
  }
  return ret;
}

/*
  Returns:
    SB_FAILURE
    SB_RUNTIME_ERR
    SB_OK
    SB_MEM_EXCEED
    SB_TIME_EXCEED
    SB_TASK_EXCEED
*/
int sandboxExec(
  const char *exect_path, const char *jail_path,
  const char *input_file, const char *output_file,
  const CgroupLocs *cg_locs, const ResLimits *res_lims,
  const char *whitelist, uid_t uid, gid_t gid) {

  int notify_p = eventfd(0, 0);
  int notify_c = eventfd(0, 0);

  scmp_filter_ctx ctx;

  // ------------------ clone ------------------
  // TODO: what should child_stack_size be set to,
  // considering mem limits will be placed on child proc?
  long int child_stack_size = 1024 * 1024;
  char *child_stack = malloc(child_stack_size);
  if (child_stack == NULL) {
    printErr( __FILE__, __LINE__, "malloc failed\n", 0, 0);
    return SB_FAILURE;
  }
  ChildPayload cp;
  cp.exect_path = exect_path;
  cp.jail_path = jail_path;
  cp.input_file = input_file;
  cp.output_file = output_file;
  cp.whitelist = whitelist;
  cp.ctx = &ctx;
  cp.notify_p = notify_p;
  cp.notify_c = notify_c;
  cp.uid = uid;
  cp.gid = gid;

  // assuming downwardly growing stack
  // this pid is (also) the pid from kernel view
  pid_t pid = clone(
    childFunc, child_stack + child_stack_size, CLONE_NEWPID | SIGCHLD,
    &cp);
  if (pid == -1) {
    printErr(__FILE__, __LINE__, "clone failed", 1, errno);
    if (sandboxExecFailCleanup(notify_p, notify_c, child_stack) == -1) {
      printErr(
        __FILE__, __LINE__, "sandboxExecFailCleanup failed", 1, errno);
    }
    return SB_FAILURE;
  }

  // ------------------ set resource limits ------------------
  uint64_t u;
  // blocks until child notifies
  if (read(notify_p, &u, sizeof(u)) == -1) {
    printErr(__FILE__, __LINE__, "read failed", 1, errno);
    if (kill(pid, SIGTERM) == -1) {
      printErr(__FILE__, __LINE__, "kill failed", 1, errno);
    }
    if (sandboxExecFailCleanup(notify_p, notify_c, child_stack) == -1) {
      printErr(
        __FILE__, __LINE__, "sandboxExecFailCleanup failed", 1, errno);
    }
    return SB_FAILURE;
  }
  int exceeded = NO_EXCEED;
  TerminatePayload *tp;
  if (setResourceLimits(
    pid, res_lims, cg_locs, &exceeded, &tp) == -1) {
    printErr(__FILE__, __LINE__, "setResourceLimits failed", 0, 0);

    if (kill(pid, SIGTERM) == -1) {
      printErr(__FILE__, __LINE__, "kill failed", 1, errno);
    }

    if (sandboxExecFailCleanup(notify_p, notify_c, child_stack) == -1) {
      printErr(
        __FILE__, __LINE__, "sandboxExecFailCleanup failed", 1, errno);
    }
    return SB_FAILURE;
  }
  u = 1;
  // notify child that resource limits are set
  if (write(notify_c, &u, sizeof(u)) == -1) {
    printErr(__FILE__, __LINE__, "write failed", 1, errno);

    if (kill(pid, SIGTERM) == -1) {
      printErr(__FILE__, __LINE__, "kill failed", 1, errno);
    }

    if (removePidDirs(cg_locs, pid) == -1) {
      printErr(__FILE__, __LINE__, "kill failed", 1, errno);
    }

    if (sandboxExecFailCleanup(notify_p, notify_c, child_stack) == -1) {
      printErr(
        __FILE__, __LINE__, "sandboxExecFailCleanup failed", 1, errno);
    }
    return SB_FAILURE;
  }
  if (close(notify_p) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
  }
  if (close(notify_c) == -1) {
    printErr(__FILE__, __LINE__, "close failed", 1, errno);
  }

  // ------------------ wait for child to terminate ------------------
  int wstatus;
  waitpid(pid, &wstatus, 0);

  tp -> terminated = 1;

  // free resources
  free(child_stack);
  if (ctx != NULL) {
    seccomp_release(ctx);
  }

  if (tp -> once == 1) {
    // reaching here means 'terminate' was called, hence wait for it to
    // finish
    while (tp -> done == 0);
  } else {
    // need to cancel the threads
    tp -> skip = NULL;
    if (terminate(tp) == -1) {
      printErr(__FILE__, __LINE__, "terminate failed", 0, 0);
    }
  }
  free(tp);

  #ifdef SB_VERBOSE
  printf("**************** Results *********************\n");
  #endif

  if (WIFEXITED(wstatus)) {
    #ifdef SB_VERBOSE
    printf("Child exited with exit status: %d\n", WEXITSTATUS(wstatus));
    #endif
    if (WEXITSTATUS(wstatus) == EXIT_CHILD_FAILURE) {
      printErr(__FILE__, __LINE__, "Sandbox failure", 0, 0);
      return SB_FAILURE;
    }
  } else if (WIFSIGNALED(wstatus)) {
    #ifdef SB_VERBOSE
    printf("Child terminated with signal: %d\n", WTERMSIG(wstatus));
    #endif
  } else {
    printErr(__FILE__, __LINE__, "Unexpected: Child neither exited nor signaled",
      0, 0);
    return SB_FAILURE;
  }

  switch(exceeded) {
    case NO_EXCEED:
      if (WIFSIGNALED(wstatus)) {
        #ifdef SB_VERBOSE
        printf("Runtime error\n");
        #endif
        return SB_RUNTIME_ERR;
      } else {
        #ifdef SB_VERBOSE
        printf("All OK\n");
        #endif
        return SB_OK;
      }
    case FATAL_ERROR_EXCEED:
      printErr(__FILE__, __LINE__, "Sandbox failure\n", 0, 0);
      return SB_FAILURE;
    case MEM_LIM_EXCEED:
      #ifdef SB_VERBOSE
      printf("Memory limit exceeded\n");
      #endif
      return SB_MEM_EXCEED;
    case TIME_LIM_EXCEED:
      #ifdef SB_VERBOSE
      printf("Time limit exceeded\n");
      #endif
      return SB_TIME_EXCEED;
    case TASK_LIM_EXCEED:
      #ifdef SB_VERBOSE
      printf("Task limit exceeded\n");
      #endif
      return SB_TASK_EXCEED;
    default:
      printErr(__FILE__, __LINE__, "Unexpected value for exceeded", 0, 0);
      return SB_FAILURE;
  }
}
