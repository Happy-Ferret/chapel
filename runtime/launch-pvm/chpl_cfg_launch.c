#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chplio_md.h>
#include <signal.h>

#include "pvm3.h"

#include "chplcgfns.h"
#include "chplrt.h"
#include "chplcomm.h"
#include "chpl_mem.h"
#include "chplsys.h"
#include "chplthreads.h"
#include "error.h"

#include "chpllaunch.h"
/*#include "chplmem.h"
#include "error.h"
*/
#define NOTIFYTAG 4194295
#define PRINTF_BUFF_LEN 1024

extern int gethostname(char *name, size_t namelen);
void error_exit(int sig);
void memory_cleanup(void);
char *replace_str(char *str, char *orig, char *rep);

int tids[32];

// For memory allocation
#define M_ARGV0REP            0x1
#define M_ARGV2               0x2
#define M_COMMANDTOPVM        0x4
#define M_ENVIRONMENT         0x8
#define M_HOSTFILE            0x10
#define M_MULTIREALMENVNAME   0x20
#define M_MULTIREALMPATHTOADD 0x40
#define M_PVMNODESTOADD       0x80
#define M_REALMTOADD          0x100
#define M_REALMTYPE           0x200
char** argv2;
char* argv0rep;
char* commandtopvm;
char* environment;
char* hostfile;
char* multirealmenvname;
char* multirealmpathtoadd[2048];
char* pvmnodestoadd[2048];
char* realmtoadd[2048];
char* realmtype;
int memalloced = 0;
int totalalloced = 0;


// Helper function
char *replace_str(char *str, char *orig, char *rep)
{
  static char buffer[4096];
  char *p;

  if(!(p = strstr(str, orig)))  // Is 'orig' even in 'str'?
    return str;

  strncpy(buffer, str, p-str); // Copy characters from 'str' start to 'orig' st$
  buffer[p-str] = '\0';

  sprintf(buffer+(p-str), "%s%s", rep, p+strlen(orig));

  return buffer;
}



static char* chpl_launch_create_command(int argc, char* argv[], int32_t numLocales) {
  int i, j, info;
  int infos[256];
  int infos2[256];
  char myhostname[256];
  char pvmnodetoadd[256];
  int pvmsize = 0;
  int commsig = 0;
  static char *hosts2redo[2048];
  int numt;
  static char *argtostart[] = {(char*)""};
  int bufid;

  // These are for receiving singals from slaves
  int hostsexit;
  int fdnum;
  char buffer[PRINTF_BUFF_LEN];
  char description[PRINTF_BUFF_LEN];  // gdb specific
  int ignorestatus;                   // gdb specific
  int who;                            // gdb specific

  char nameofbin[1024];
  char numlocstr[128];

  // Add nodes to PVM configuration.
  FILE* nodelistfile;

  int k;                              // k iterates over chpl_numRealms
  int lpr;                            // locales per realm
  char* multirealmenv;
  int baserealm;

  // Signal handlers
  signal(SIGINT, error_exit);
  signal(SIGQUIT, error_exit);
  signal(SIGKILL, error_exit);
  signal(SIGTERM, error_exit);

  // Get a new argument list for PVM spawn.
  // The last argument needs to be the number of locations for the PVM
  // comm layer to use it. The comm layer strips this off.

  argv2 = chpl_malloc(((argc+1) * sizeof(char *)), sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
  memalloced |= M_ARGV2;
  for (i=0; i < (argc-1); i++) {
    argv2[i] = argv[i+1];
  }
  sprintf(numlocstr, "%d", numLocales);
  argv2[argc-1] = numlocstr;
  argv2[argc] = NULL;

  // Add nodes to PVM configuration.
  i = 0;
  for (k = 0; k < chpl_numRealms; k++) {
    if (chpl_numRealms != 1) {
      lpr = chpl_localesPerRealm(k);
      if (lpr == 0) {
        continue;
      }
    } else {
      lpr = numLocales;
    }
    hostfile = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
    memalloced |= M_HOSTFILE;
    realmtype = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
    memalloced |= M_REALMTYPE;
    multirealmenvname = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
    memalloced |= M_MULTIREALMENVNAME;
    if (chpl_numRealms != 1) {
      sprintf(realmtype, "%s", chpl_realmType(k));
    } else {
      sprintf(realmtype, "%s", getenv((char *)"CHPL_HOST_PLATFORM"));
    }
    sprintf(multirealmenvname, "CHPL_MULTIREALM_LAUNCH_DIR_%s", realmtype);
    multirealmenv = getenv(multirealmenvname);
    if (multirealmenv == NULL) {
      multirealmenv = getenv((char *)"CHPL_HOME");
    }
    chpl_free(multirealmenvname, -1, "");
    memalloced &= ~M_MULTIREALMENVNAME;
    sprintf(hostfile, "%s%s%s", getenv((char *)"CHPL_HOME"), "/hostfile.", realmtype);
    
    if ((nodelistfile = fopen(hostfile, "r")) == NULL) {
      memory_cleanup();
      // Let the main launcher print the error (unable to locale file)
      return (char *)"";
    }
    chpl_free(hostfile, -1, "");
    memalloced &= ~M_HOSTFILE;
    j = 0;
    while (((fscanf(nodelistfile, "%s", pvmnodetoadd)) == 1) && (j < lpr)) {
      pvmnodestoadd[i] = chpl_malloc((strlen(pvmnodetoadd)+1), sizeof(char *), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
      memalloced |= M_PVMNODESTOADD;
      realmtoadd[i] = chpl_malloc((strlen(realmtype)+1), sizeof(char *), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
      memalloced |= M_REALMTOADD;
      multirealmpathtoadd[i] = chpl_malloc((strlen(multirealmenv)+1), sizeof(char *), CHPL_RT_MD_PVM_LIST_OF_NODES, -1, "");
      memalloced |= M_MULTIREALMPATHTOADD;
      strcpy(pvmnodestoadd[i], pvmnodetoadd);
      strcpy(realmtoadd[i], realmtype);
      strcpy(multirealmpathtoadd[i], multirealmenv);
      //      fprintf(stderr, "Adding pvmnodestoadd[%d], realm %s of j iter %d on directory %s: %s\n", i, realmtoadd[i], j, multirealmpathtoadd[i], pvmnodestoadd[i]);
      i++;
      j++;
    }
    chpl_free(realmtype, -1, "");
    memalloced &= ~M_REALMTYPE;
    // Check to make sure user hasn't specified more nodes (-nl <n>) than
    // what's included in the hostfile.
    if (j < lpr) {
      fclose(nodelistfile);
      memory_cleanup();
      chpl_internal_error("Number of locales specified is greater than what's known in PVM hostfile.");
    }
    fclose(nodelistfile);
  }
  totalalloced = i;

  // Check to see if daemon is started or not by this user. If not, start one.
  i = pvm_setopt(PvmAutoErr, 0);
  info = pvm_start_pvmd(0, argtostart, 1);
  pvm_setopt(PvmAutoErr, i);

  if ((info != 0) && (info != -28)) {
    fprintf(stderr, "Exiting -- pvm_start_pvmd error %d.\n", info);
    memory_cleanup();
    chpl_internal_error("Problem starting PVM daemon.");
  }

  // Find the node we're on. We use this in spawning (to know what realm
  // type we are to replace that string with an architecture appropriate one
  gethostname(myhostname, 256);
  for (i = 0; i < numLocales; i++) {
    if (!(strcmp((char *)pvmnodestoadd[i], myhostname))) {
      baserealm = i;
      break;
    }
  }

  // Add everything (turn off errors -- we don't care if we add something
  // that's already there).
  i = pvm_setopt(PvmAutoErr, 0);
  info = pvm_addhosts( (char **)pvmnodestoadd, numLocales, infos );
  pvm_setopt(PvmAutoErr, i);
  // Something happened on addhosts -- likely old pvmd running
  for (i = 0; i < numLocales; i++) {
    if ((infos[i] < 0) && (infos[i] != -28)) {
      sprintf(buffer, "ssh -q %s \"touch /tmp/Chplpvmtmp && rm -rf /tmp/*pvm* && killall -q -9 pvmd3\"", pvmnodestoadd[i]);
      system(buffer);
      hosts2redo[0] = pvmnodestoadd[i];
      info = pvm_addhosts( (char **)hosts2redo, 1, infos2);
      if (infos2[0] < 0) {
        fprintf(stderr, "Remote error on %s: %d\n", hosts2redo[0], infos2[0]);
        fprintf(stderr, "Shutting down host.\n");
        memory_cleanup();
        pvm_halt();
        chpl_internal_error("Exiting");
      }
    }
  }

  argv0rep = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
  memalloced |= M_ARGV0REP;
  strcpy(argv0rep, argv[0]);
  strcpy(nameofbin, strrchr(argv[0], '/'));
  // Build the command to send to pvm_spawn.
  // First, try the command built from CHPL_MULTIREALM_LAUNCH_DIR_<realm>
  //      and the executable_real. Replace architecture strings with target
  //      architecture names.
  // If this doesn't work, store the name of the file tried with the node
  //      into a debug message. Then try just what was passed on the command
  //      line.
  // Failing that, try the current working directory with executable_real.
  // If this doesn't work, error out with the debug message.
  for (i = 0; i < numLocales; i++) {
    //    fprintf(stderr, "Loop i=%d (iteration %d of %d)\n", i, i+1, numLocales);
    pvmsize += strlen(multirealmpathtoadd[i]) + strlen("_real") + strlen(nameofbin);

    commandtopvm = chpl_malloc(pvmsize, sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
    memalloced |= M_COMMANDTOPVM;
    *commandtopvm = '\0';
    environment = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
    memalloced |= M_ENVIRONMENT;
    *environment = '\0';
    strcat(environment, multirealmpathtoadd[i]);
    strcat(commandtopvm, environment);
    chpl_free(environment, -1, "");
    memalloced &= ~M_ENVIRONMENT;
    strcat(commandtopvm, nameofbin);
    strcat(commandtopvm, "_real");

    while (strstr(commandtopvm, realmtoadd[baserealm]) && 
           (chpl_numRealms != 1) &&
           (strcmp(realmtoadd[baserealm], realmtoadd[i]))) {
      commandtopvm = replace_str(commandtopvm, realmtoadd[baserealm], realmtoadd[i]);
    }

    //    fprintf(stderr, "spawning %s on %s\n", commandtopvm, pvmnodestoadd[i]);
    numt = pvm_spawn( (char *)commandtopvm, argv2, 1, (char *)pvmnodestoadd[i], 1, &tids[i] );
    //    fprintf(stderr, "numt was %d, tids[%d] was %d\n", numt, i, tids[i]);
    if (numt == 0) {
      if (tids[i] == PvmNoFile) {
        chpl_free(commandtopvm, -1, "");
        memalloced &= ~M_COMMANDTOPVM;
        pvmsize = strlen(argv0rep) + strlen("_real");
        commandtopvm = chpl_malloc(pvmsize, sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
        memalloced |= M_COMMANDTOPVM;
        *commandtopvm = '\0';
        strcat(commandtopvm, argv0rep);
        strcat(commandtopvm, "_real");
        while (strstr(commandtopvm, realmtoadd[baserealm]) && 
               (chpl_numRealms != 1) &&
               (strcmp(realmtoadd[baserealm], realmtoadd[i]))) {
          commandtopvm = replace_str(commandtopvm, realmtoadd[baserealm], realmtoadd[i]);
        }
        //        fprintf(stderr, "trying again to spawn %s on %s\n", commandtopvm, pvmnodestoadd[i]);
        numt = pvm_spawn( (char *)commandtopvm, argv2, 1, (char *)pvmnodestoadd[i], 1, &tids[i] );
        //        fprintf(stderr, "numt was %d, tids[%d] was %d\n", numt, i, tids[i]);
        if (numt == 0) {
          if (tids[i] == PvmNoFile) {
            chpl_free(commandtopvm, -1, "");
            memalloced &= ~M_COMMANDTOPVM;
            commandtopvm = chpl_malloc(1024, sizeof(char*), CHPL_RT_MD_PVM_SPAWN_THING, -1, "");
            memalloced |= M_COMMANDTOPVM;
            *commandtopvm = '\0';
            sprintf(commandtopvm, "%s%s%s", getenv((char *)"PWD"), nameofbin, "_real");
            //            fprintf(stderr, "try 3 to spawn %s on %s\n", commandtopvm, pvmnodestoadd[i]);
            numt = pvm_spawn( (char *)commandtopvm, argv2, 1, (char *)pvmnodestoadd[i], 1, &tids[i] );
            //            fprintf(stderr, "numt was %d, tids[%d] was %d\n", numt, i, tids[i]);
            if (numt == 0) {
              i = pvm_setopt(PvmAutoErr, 0);
              info = pvm_delhosts( (char **)pvmnodestoadd, numLocales, infos );
              pvm_setopt(PvmAutoErr, i);
              memory_cleanup();
              // Let the main launcher print the error (unable to locale file)
              return (char *)"";
            }
          }
        }
      }
    }
  }
  chpl_free(argv0rep, -1, "");
  memalloced &= ~M_ARGV0REP;
  chpl_free(argv2, -1, "");
  memalloced &= ~M_ARGV2;
  chpl_free(commandtopvm, -1, "");
  memalloced &= ~M_COMMANDTOPVM;
  for (i = 0; i < totalalloced; i++) chpl_free(multirealmpathtoadd[i], -1, "");
  memalloced &= ~M_MULTIREALMPATHTOADD;
  for (i = 0; i < totalalloced; i++) chpl_free(realmtoadd[i], -1, "");
  memalloced &= ~M_REALMTOADD;

  // We have a working configuration. What follows is the communication
  // between the slaves and the parent (this process).
  info = pvm_mytid();

  hostsexit = 0;
  while (commsig == 0) {
    bufid = pvm_recv(-1, NOTIFYTAG);
    pvm_upkint(&commsig, 1, 1);
    // exit case
    if (commsig == 1) {
      hostsexit++;
      if (hostsexit != numLocales) {
        commsig = 0;
      }
    }
    // fprintf case
    if (commsig == 2) {
      pvm_upkint(&fdnum, 1, 1);
      pvm_upkstr(buffer);
      if (fdnum == 0) {
        fprintf(stdin, "%s", buffer);
      } else if (fdnum == 1) {
        fprintf(stdout, "%s", buffer);
      } else {
        fprintf(stderr, "%s", buffer);
      }
      fflush(stdout);
      fflush(stderr);
      commsig = 0;
    }
    // printf case
    if (commsig == 3) {
      pvm_upkstr(buffer);
      printf("%s", buffer);
      fflush(stdout);
      fflush(stderr);
      commsig = 0;
    }
    // Run in gdb mode
    if (commsig == 4) {
      pvm_upkint(&who, 1, 1);
      pvm_upkstr(buffer);
      pvm_upkstr(description);
      pvm_upkint(&ignorestatus, 1, 1);
      info = system(buffer);
      pvm_initsend(PvmDataDefault);
      pvm_pkint(&info, 1, 1);
      pvm_send(who, NOTIFYTAG);
      if (info == -1) {
        chpl_error("system() fork failed", 0, "(command-line)");
      } else if (info != 0 && !ignorestatus) {
        chpl_error(description, 0, "(command-line)");
      }
    }
  }

  i = pvm_setopt(PvmAutoErr, 0);
  info = pvm_delhosts( (char **)pvmnodestoadd, numLocales, infos );
  pvm_setopt(PvmAutoErr, i);

  memory_cleanup();
  exit(0);
  return (char *)"";
}

void chpl_launch(int argc, char* argv[], int32_t numLocales) {
  chpl_launch_using_system(chpl_launch_create_command(argc, argv, numLocales),
                           argv[0]);
}


int chpl_launch_handle_arg(int argc, char* argv[], int argNum,
                           int32_t lineno, chpl_string filename) {
  return 0;
}

void error_exit(int sig) {
  int i;
  char buffer[PRINTF_BUFF_LEN];

  fflush(stdout);
  fflush(stderr);

  for (i=0; tids[i]; i++) {
    sprintf(buffer, "ssh -q %s \"touch /tmp/Chplpvmtmp && rm -rf /tmp/*pvm* && killall -q -9 pvmd3\"", pvmnodestoadd[i]);
    system(buffer);
  }
  
  memory_cleanup();
  exit(1);
}

void memory_cleanup() {
  int i;
  if (memalloced & M_ARGV0REP) chpl_free(argv0rep, -1, "");
  if (memalloced & M_ARGV2) chpl_free(argv2, -1, "");
  if (memalloced & M_COMMANDTOPVM) chpl_free(commandtopvm, -1, "");
  if (memalloced & M_ENVIRONMENT) chpl_free(environment, -1, "");
  if (memalloced & M_HOSTFILE) chpl_free(hostfile, -1, "");
  if (memalloced & M_MULTIREALMENVNAME) chpl_free(multirealmenvname, -1, "");
  if (memalloced & M_MULTIREALMPATHTOADD) for (i = 0; i < totalalloced; i++) chpl_free(multirealmpathtoadd[i], -1, "");
  if (memalloced & M_PVMNODESTOADD) for (i = 0; i < totalalloced; i++) chpl_free(pvmnodestoadd[i], -1, "");
  if (memalloced & M_REALMTOADD) for (i = 0; i < totalalloced; i++) chpl_free(realmtoadd[i], -1, "");
  if (memalloced & M_REALMTYPE) chpl_free(realmtype, -1, "");
  return;
}
