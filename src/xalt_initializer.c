#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <time.h>

#define PATHMAX 4096
static char   uuid_str[37];
static double start_time = 0.0;
static double end_time   = 0.0;
static long   my_rank    = 0L;
static long   my_size    = 1L;
static char   path[4096];
static char   cmdline[16384];

long compute_size(const char **envA)
{
  long          sz = 0L;
  const char ** p;
  for (p = &envA[0]; *p; ++p)
    {
      char *v = getenv(*p);
      if (v)
        sz += strtol(v, (char **) NULL, 10);
    }

  return sz;
}

void myinit(int argc, char **argv, char **envp)
{
  const char *  rankA[] = {"PMI_RANK", "OMPI_COMM_WORLD_RANK", "MV2_COMM_WORLD_RANK", NULL}; 
  const char *  sizeA[] = {"PMI_SIZE", "OMPI_COMM_WORLD_SIZE", "MV2_COMM_WORLD_SIZE", NULL}; 
  const char ** p;
  struct timeval tv;

  uuid_t uuid;

  my_rank = compute_size(rankA);
  if (my_rank > 0L)
    return;

  my_size = compute_size(sizeA);
  if (my_size < 1L)
    my_size = 1L;

  if (argv[0][0] == '/')
    strcpy(path,argv[0]);
  else
    {
      int len;
      getcwd(path, PATHMAX);
      len = strlen(path);
      path[len] = '/';
      strcpy(&path[len+1], argv[0]);
    }
  

  uuid_generate(uuid);
  uuid_unparse_lower(uuid,uuid_str);
  gettimeofday(&tv,NULL);
  start_time = tv.tv_sec + 1.e-6*tv.tv_usec;

  
  sprintf(cmdline, "%s %s --start \"%.3f\" --end 0 --uuidgen \"%s\" -- '[{\"exec_prog\": \"%s\", \"ntask\": %ld}]",
	  "/usr/local/bin/python","xalt_run_submission.py", start_time, uuid_str, path, my_size);

  printf("cmd: %s\n\n",cmdline);
}

void myfini()
{
  struct timeval tv;

  if (my_rank > 0L)
    return;

  gettimeofday(&tv,NULL);
  end_time = tv.tv_sec + 1.e-6*tv.tv_usec;

  sprintf(cmdline, "%s %s --start \"%.3f\" --end \"%.3f\" --uuidgen \"%s\" -- '[{\"exec_prog\": \"%s\", \"ntask\": %ld}]",
	  "/usr/local/bin/python","xalt_run_submission.py", start_time, end_time, uuid_str, path, my_size);
  printf("cmd: %s\n\n",cmdline);
}

#ifdef __MACH__
  __attribute__((section("__DATA,__mod_init_func"), used, aligned(sizeof(void*)))) typeof(myinit) *__init = myinit;
  __attribute__((section("__DATA,__mod_term_func"), used, aligned(sizeof(void*)))) typeof(myfini) *__fini = myfini;
#else
  __attribute__((section(".init_array"))) typeof(myinit) *__init = myinit;
  __attribute__((section(".fini_array"))) typeof(myfini) *__fini = myfini;
#endif
