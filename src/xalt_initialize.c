/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#define  _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include "xalt_regex.h"
#include "xalt_interval.h"
#include <errno.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef __MACH__
#  include <libproc.h>
#endif
#include "xalt_obfuscate.h"
#include "base64.h"
#include "xalt_quotestring.h"
#include "xalt_header.h"
#include "xalt_config.h"
#include "xalt_fgets_alloc.h"
#include "xalt_path_parser.h"
#include "xalt_hostname_parser.h"
#include "build_uuid.h"
#include "xalt_tmpdir.h"
#include "xalt_vendor_note.h"

#if USE_DCGM && USE_NVML
#error "Both DCGM and NVML enabled.  This is not allowed."
#endif

#ifdef USE_NVML
/* This code will only ever be active in 64 bit mode and not 32 bit mode */
#include <dlfcn.h>
#include <nvml.h>
#endif

#ifdef USE_DCGM
/* This code will only ever be active in 64 bit mode and not 32 bit mode */
#  include <dcgm_agent.h>
#  include <dcgm_structs.h>
#endif

#ifdef HAVE_EXTERNAL_HOSTNAME_PARSER
#  define HOSTNAME_PARSER         my_hostname_parser
#  define HOSTNAME_PARSER_CLEANUP my_hostname_parser_cleanup
#else
#  define HOSTNAME_PARSER         hostname_parser
#  define HOSTNAME_PARSER_CLEANUP hostname_parser_cleanup
#endif


#define DATESZ    100

typedef enum { BIT_SCALAR = 1, BIT_PKGS = 2, BIT_MPI = 4} xalt_tracking_flags;
typedef enum { PKGS=1, KEEP=2, SKIP=3} xalt_parser;

typedef enum { XALT_SUCCESS = 0, XALT_TRACKING_OFF, XALT_WRONG_STATE, XALT_RUN_TWICE,
               XALT_MPI_RANK, XALT_HOSTNAME, XALT_PATH, XALT_BAD_JSON_STR, XALT_NO_OVERLAP,
	       XALT_MISSING_RUN_SUBMISSION} xalt_status;

static const char * xalt_reasonA[] = {
  "Successful XALT tracking",
  "XALT_EXECUTABLE_TRACKING is off",
  "__XALT_INITIAL_STATE__ is different from STATE",
  "XALT is trying to be twice in the same STATE",
  "XALT only tracks rank 0, in mpi programs",
  "XALT has found the host does not match hostname pattern. If this is unexpected check your config.py file: " XALT_CONFIG_PY ,
  "XALT has found the executable does not match path pattern. If this is unexpected check your config.py file: " XALT_CONFIG_PY ,
  "XALT has problem with a JSON string",
  "XALT execute type does not match requested type",
  "XALT Cannot find XALT_DIR/libexec/xalt_run_submission",
};


static const char * xalt_build_descriptA[] = {
  "track no programs at all",                     /* 0 */
  "track scalar programs only",                   /* 1 */
  "Not possible (2)",                             /* 2 */
  "Not possible (3)",                             /* 3 */
  "track MPI programs only",                      /* 4 */
  "track all programs",                           /* 5 */
  "Not possible (6)",                             /* 6 */
  "Not possible (7)",                             /* 7 */
};

static const char * xalt_run_descriptA[] = {
  "Not possible (0)",                             /* 0 */
  "program is a scalar program",                  /* 1 */
  "Not possible (2)",                             /* 2 */
  "Not possible (3)",                             /* 3 */
  "program is an MPI program"                     /* 4 */
};

static const char * xalt_run_short_descriptA[] = {
  "Not possible (0)",                             /* 0 */
  "scalar",                                       /* 1 */
  "PKGS",                                         /* 2 */
  "Not possible (3)",                             /* 3 */
  "MPI"                                           /* 4 */
};

const  char*           xalt_syshost();
static long            compute_value(const char **envA);
static void            get_abspath(char * path, int sz);
static volatile double epoch();
static unsigned int    mix(unsigned int a, unsigned int b, unsigned int c); 
static double          scalar_program_sample_probability(double runtime);
#ifdef USE_NVML
static int             load_nvml();
#endif

void myinit(int argc, char **argv);
void myfini();
void wrapper_for_myfini(int signum);
#define STR(x)   StR1_(x)
#define StR1_(x) #x

static int          countA[2];
static char         uuid_str[37];
static char         exec_path[PATH_MAX+1];
static char *       exec_pathQ;
static char *       usr_cmdline;
static char *       b64_cmdline;
static const char * my_syshost;

static char *       watermark             = NULL;
static char *       b64_watermark         = NULL;
static int          run_submission_exists = -1;             /* 0 => does not exist; 1 => exists; -1 => status unknown */
static xalt_status  reject_flag	          = XALT_SUCCESS;
static int          run_mask              = 0;
static double       probability           = 1.0;
static pid_t        ppid	          = 0;
static pid_t        pid  	          = 0;
static int          errfd	          = -1;
static double       my_rand               = 0.0;
static double       start_time	          = 0.0;
static double       end_time	          = 0.0;
static double       frac_time             = 0.0;
static long         my_rank	          = 0L;
static long         my_size	          = 1L;
static int          xalt_kind             = 0;
static int          xalt_tracing          = 0;
static int          xalt_run_tracing      = 0;
static int          xalt_gpu_tracking     = 0;
static char *       pathArg	          = NULL;
static char *       ldLibPathArg          = NULL;
static int          num_gpus              = 0;
static int          b64_len               = 0;
static int          b64_wm_len            = 0;
#ifdef USE_NVML
static unsigned long long __time          = 0;
static void * nvml_handle                 = NULL;
static nvmlReturn_t (*_nvmlDeviceGetAccountingBufferSize)(nvmlDevice_t,
                                                          unsigned int *);
static nvmlReturn_t (*_nvmlDeviceGetAccountingMode)(nvmlDevice_t,
                                                    nvmlEnableState_t *);
static nvmlReturn_t (*_nvmlDeviceGetAccountingPids)(nvmlDevice_t,
                                                    unsigned int *,
                                                    unsigned int *);
static nvmlReturn_t (*_nvmlDeviceGetAccountingStats)(nvmlDevice_t,
                                                     unsigned int,
                                                     nvmlAccountingStats_t *);
static nvmlReturn_t (*_nvmlDeviceGetCount)(unsigned int *);
static nvmlReturn_t (*_nvmlDeviceGetHandleByIndex)(unsigned int,
                                                   nvmlDevice_t *);
static char * (*_nvmlErrorString)(nvmlReturn_t);
static nvmlReturn_t (*_nvmlInit)();
static nvmlReturn_t (*_nvmlShutdown)();
#endif
#ifdef USE_DCGM
static dcgmHandle_t dcgm_handle           = NULL;
/* This code will only every be active in 64 bit mode and not 32 bit mode*/
/* Temporarily disable any stderr messages from DCGM */
#define DCGMFUNC2(FUNC,x1,x2,out)    \
{                                    \
  dcgmReturn_t __result;             \
  int fd1, fd2;                      \
  fflush(stderr);                    \
  fd1 = dup(STDERR_FILENO);          \
  fd2 = open("/dev/null", O_WRONLY); \
  dup2(fd2, STDERR_FILENO);          \
  close(fd2);                        \
  __result = FUNC(x1, x2);           \
  fflush(stderr);                    \
  dup2(fd1, STDERR_FILENO);          \
  close(fd1);                        \
  (*(out)) = __result;               \
}
#endif

#define HERE  fprintf(stderr,    "%s:%d\n",__FILE__,__LINE__)

#define DEBUG0(fp,s)             if (xalt_tracing) fprintf((fp),s)
#define DEBUG1(fp,s,x1)          if (xalt_tracing) fprintf((fp),s,(x1))
#define DEBUG2(fp,s,x1,x2)       if (xalt_tracing) fprintf((fp),s,(x1),(x2))
#define DEBUG3(fp,s,x1,x2,x3)    if (xalt_tracing) fprintf((fp),s,(x1),(x2),(x3))
#define DEBUG4(fp,s,x1,x2,x3,x4) if (xalt_tracing) fprintf((fp),s,(x1),(x2),(x3),(x4))

void myinit(int argc, char **argv)
{
  int    i;
  int    produce_strt_rec = 0;
  char * p;
  char * p_dbg;
  char * cmdline         = NULL;
  char * ld_preload_strp = NULL;
  char   rand_str[20];
  char   dateStr[DATESZ];
  char   fullDateStr[DATESZ];
  const char * v;
  xalt_parser  results;
  xalt_parser  path_results;

  /* The SLURM env's must be last.  On Stampede2 both PMI_RANK and SLURM_PROCID are set. Only PMI_RANK is correct with multiple ibrun -n -o */
  /* Lonestar 5, Cray XC-40, only has SLURM_PROCID */
  const char * rankA[] = {"OMPI_COMM_WORLD_RANK", "MV2_COMM_WORLD_RANK", "PMI_RANK", "SLURM_PROCID",         NULL };
  const char * sizeA[] = {"OMPI_COMM_WORLD_SIZE", "MV2_COMM_WORLD_SIZE", "PMI_SIZE", "SLURM_STEP_NUM_TASKS", NULL };

  struct utsname u;

  p_dbg = getenv("XALT_TRACING");
  if (p_dbg)
    {
      xalt_tracing     = (strcmp(p_dbg,"yes") == 0);
      xalt_run_tracing = (strcmp(p_dbg,"run") == 0);
      if (xalt_tracing  || xalt_run_tracing)
	errfd	       = dup(STDERR_FILENO);
    }

  v = getenv("__XALT_INITIAL_STATE__");
  if (xalt_tracing)
    {
      if (!v) 
        {
          time_t    now    = (time_t) epoch();
          strftime(dateStr, DATESZ, "%c", localtime(&now));
          errno = 0;
          if (uname(&u) != 0)
            {
              perror("uname");
              exit(EXIT_FAILURE);
            }
          fprintf(stderr, "---------------------------------------------\n"
                          " Date:          %s\n"
                          " XALT Version:  %s\n"
                          " Nodename:      %s\n"
                          " System:        %s\n"
                          " Release:       %s\n"
                          " O.S. Version:  %s\n"
                          " Machine:       %s\n"
		          " Syshost:       %s\n"
                          "---------------------------------------------\n\n",
                  dateStr, XALT_GIT_VERSION, u.nodename, u.sysname, u.release,
                  u.version, u.machine, xalt_syshost());
        }
      get_abspath(exec_path,sizeof(exec_path));
      fprintf(stderr,"myinit(%s,%s){\n", STR(STATE),exec_path);
    }

  /***********************************************************
   * Test 0a: Initial State Test?:
   * Is this is built-in or LD_PRELOAD version of this file?
   ***********************************************************/

  DEBUG2(stderr,"  Test for __XALT_INITIAL_STATE__: \"%s\", STATE: \"%s\"\n", (v != NULL) ? v : "(NULL)", STR(STATE));
  /* Stop tracking if any myinit routine has been called */
  if (v && (strcmp(v,STR(STATE)) != 0))
    {
      DEBUG2(stderr,"    -> __XALT_INITIAL_STATE__ has a value: \"%s\" -> and it is different from STATE: \"%s\" exiting\n}\n\n",v,STR(STATE));
      reject_flag = XALT_WRONG_STATE;
      return;
    }

  /***********************************************************
   * Test 0b: Count test?:
   * Is the same version of this file run more than once?
   ***********************************************************/
  if (countA[IDX] > 0)
    {
      DEBUG2(stderr,"    -> countA[%d]: %d which is greater than 0 -> exiting\n}\n\n",IDX,countA[IDX]);
      reject_flag = XALT_RUN_TWICE;
      return;
    }
  countA[IDX]++;

  /***********************************************************
   * Test 1: XALT_EXECUTABLE_TRACKING?:
   * Is xalt tracking turned on?
   ***********************************************************/

  /* Stop tracking if XALT is turned off */
  v = getenv("XALT_EXECUTABLE_TRACKING");
  DEBUG1(stderr,"  Test for XALT_EXECUTABLE_TRACKING: %s\n",(v != NULL) ? v : "(NULL)");
  if (!v || strcmp(v,"yes") != 0)
    {
      DEBUG0(stderr,"    -> XALT_EXECUTABLE_TRACKING is off -> exiting\n}\n\n");
      reject_flag = XALT_TRACKING_OFF;
      unsetenv("XALT_RUN_UUID");
      return;
    }


  /***********************************************************
   * Test 2: MPI Rank > 0?:
   * Stop tracking if my mpi rank is not zero
   ***********************************************************/

  my_rank = compute_value(rankA);
  DEBUG1(stderr,"  Test for rank == 0, rank: %ld\n",my_rank);
  if (my_rank > 0L)
    {
      DEBUG0(stderr,"    -> MPI Rank is not zero -> exiting\n}\n\n");
      reject_flag = XALT_MPI_RANK;
      unsetenv("XALT_RUN_UUID");
      return;
    }

  /***********************************************************
   * Test 3: Test for acceptable hostname:
   ***********************************************************/
  errno = 0;
  if (uname(&u) != 0)
    {
      perror("uname");
      exit(EXIT_FAILURE);
    }

  results = HOSTNAME_PARSER(u.nodename);
  HOSTNAME_PARSER_CLEANUP();
  if (results == SKIP)
    {
      DEBUG1(stderr,"    hostname: \"%s\" is rejected\n",u.nodename);
      reject_flag = XALT_HOSTNAME;
      unsetenv("XALT_RUN_UUID");
      return; 
    }

  /***********************************************************
   * Test 4: Acceptable Executable && MPI status?
   ***********************************************************/

  int build_mask = 0;
  v = getenv("XALT_SCALAR_TRACKING");
  if (!v)
    v = XALT_SCALAR_TRACKING;
  if (strcasecmp(v,"yes") == 0)
    build_mask |= BIT_SCALAR;

  v = getenv("XALT_MPI_TRACKING");
  if (!v)
    v = XALT_MPI_TRACKING;
  if (strcasecmp(v,"yes") == 0)
    build_mask |= BIT_MPI;
      
  
  /* Test for MPI-ness */
  my_size      = compute_value(sizeA);
  if (my_size < 1L)
    my_size = 1L;

  /* Get full absolute path to executable */
  get_abspath(exec_path,sizeof(exec_path));
  path_results = keep_path(exec_path);
  path_parser_cleanup();
  
  if (path_results == SKIP)
    {
      DEBUG1(stderr,"    executable: \"%s\" is rejected\n", exec_path);
      reject_flag = XALT_PATH;
      unsetenv("XALT_RUN_UUID");
      return;
    }

  if (my_size > 1L)
    {
      run_mask |= BIT_MPI;
      xalt_kind = BIT_MPI;
    }
  else
    {
      run_mask |= BIT_SCALAR;
      xalt_kind = BIT_SCALAR;
    }

  if (path_results == PKGS)
    xalt_kind   = BIT_PKGS;
      
                      
  /* Test for an acceptable executable */
  if ((build_mask & run_mask) == 0)
    {
      DEBUG2(stderr,"    -> XALT is build to %s, Current %s -> not tracking and exiting\n}\n\n",
             xalt_build_descriptA[build_mask], xalt_run_descriptA[run_mask]);
      reject_flag = XALT_NO_OVERLAP;
      unsetenv("XALT_RUN_UUID");
      return;
    }

  setenv("__XALT_INITIAL_STATE__",STR(STATE),1);

  my_syshost = xalt_syshost();

  /* Build a json version of the user's command line. */

  /* Calculate size of buffer*/
  int sz = 0;
  for (i = 0; i < argc; ++i)
    sz += strlen(argv[i]);

  /* this size formula uses 3 facts:
   *   1) if every character was a utf-16 character that is four bytes converts to 12 (sz*3)
   *   2) Each entry has two quotes and a space (argc*3)
   *   3) There are a pair of square brackets and a null byte (+3)
   */

  sz = sz*3 + argc*3 + 3;

  usr_cmdline = (char *) malloc(sz);
  p	      = &usr_cmdline[0];
  *p++        = '[';
  for (i = 0; i < argc; ++i)
    {
      *p++ = '"';
      const char* qs  = xalt_quotestring(argv[i]);
      int         len = strlen(qs);
      memcpy(p,qs,len);
      p += len;
      *p++= '"';
      *p++= ',';
    }
  *--p = ']';
  *++p = '\0';

  int qsLen   = p - usr_cmdline;

  if (p > &usr_cmdline[sz])
    {
      fprintf(stderr,"XALT: Failure in building user command line json string!\n");
      reject_flag = XALT_BAD_JSON_STR;
      unsetenv("XALT_RUN_UUID");
      return;
    }
  xalt_quotestring_free();
  b64_cmdline = base64_encode(usr_cmdline, qsLen, &b64_len);

  build_uuid(uuid_str);

#if USE_DCGM || USE_NVML
  /* This code will only ever be active in 64 bit mode and not 32 bit mode */
  v  = getenv("XALT_GPU_TRACKING");
  if (v == NULL)
    v = XALT_GPU_TRACKING;
  xalt_gpu_tracking = (strcmp(v,"yes") == 0);
  
  do
    {
      if (xalt_gpu_tracking)
        {
          DEBUG0(stderr, "  GPU tracing\n");

#ifdef USE_NVML
          /* Open the NVML library at runtime.  This avoids failing if
             the library is not available on a particular system.  In
             that case, the handle will not be created and GPU
             tracking will be disabled. */
          if (load_nvml() == 0) {
            xalt_gpu_tracking = 0;
            break;
          }

          nvmlReturn_t result;
          struct timeval tv;

          /* Mark time the program started.  Use this to compare to
           * GPU process timestamps in fini to only count processes
           * that started during the lifetime of the program. */
          gettimeofday(&tv, NULL);
          __time = (unsigned long long)(tv.tv_sec) * 1000000 + (unsigned long long)(tv.tv_usec);

          result = _nvmlInit();
          if (result != NVML_SUCCESS)
            {
              DEBUG1(stderr, "    -> Stopping GPU Tracking => Cannot initialize NVML: %s\n\n", _nvmlErrorString(result));
              xalt_gpu_tracking = 0;
              break;
            }
#elif USE_DCGM
          dcgmReturn_t result;

          result = dcgmInit();
          if (result != DCGM_ST_OK)
            {
              DEBUG1(stderr, "    -> Stopping GPU Tracking => Cannot initialize DCGM: %s\n\n", errorString(result));
              xalt_gpu_tracking = 0;
              dcgm_handle       = NULL;
              break;
            }

          DCGMFUNC2(dcgmStartEmbedded, DCGM_OPERATION_MODE_MANUAL, &dcgm_handle, &result); 

          if (result != DCGM_ST_OK)
            {
              DEBUG1(stderr, "    -> Stopping GPU Tracking => Cannot start DCGM: %s\n\n", errorString(result));
              xalt_gpu_tracking = 0;
              dcgm_handle       = NULL;
              break;
            }

          result = dcgmJobStartStats(dcgm_handle, (dcgmGpuGrp_t)DCGM_GROUP_ALL_GPUS, uuid_str);
          if (result != DCGM_ST_OK)
            {
              DEBUG1(stderr, "    -> Stopping GPU Tracking => Cannot start DCGM job stats: %s\n\n", errorString(result));
              xalt_gpu_tracking = 0;
              dcgm_handle       = NULL;
              break;
            }

          result = dcgmWatchJobFields(dcgm_handle, (dcgmGpuGrp_t)DCGM_GROUP_ALL_GPUS, 1000, 1e9, 0);
          if (result != DCGM_ST_OK)
            {
              DEBUG1(stderr,   "    -> Stopping GPU Tracking => Cannot start DCGM job watch: %s\n\n", errorString(result));
	      if (result == DCGM_ST_REQUIRES_ROOT)
		DEBUG0(stderr, "    -> May need to enable accounting mode: sudo nvidia-smi -am 1\n");
              xalt_gpu_tracking = 0;
              dcgm_handle       = NULL;
              break;
            }

          result = dcgmUpdateAllFields(dcgm_handle, 1);
          if (result != DCGM_ST_OK)
            {
              DEBUG1(stderr, "    -> Stopping GPU Tracking => Cannot update DCGM job fields: %s\n\n", errorString(result));
              xalt_gpu_tracking = 0;
              dcgm_handle       = NULL;
              break;
            }
#endif
        }
    }
  while(0);
#endif

  start_time = epoch();
  frac_time  = start_time - (long) (start_time);

  /**********************************************************
   * Save LD_PRELOAD and clear it before running
   * xalt_run_submission.
   *********************************************************/

  p = getenv("LD_PRELOAD");
  if (p)
    ld_preload_strp = strdup(p);

  unsetenv("LD_PRELOAD");

  const char * blank = " ";

  char * env_path = getenv("PATH");
  if (!env_path)
    {
      pathArg = (char *) malloc(2);
      memcpy(pathArg,blank,2);
    }
  else
    {
      int ilen = 0;
      int plen = strlen(env_path);
      pathArg  = (char *) malloc(8 + plen + 2);
      memcpy(&pathArg[ilen], "--path \"", 8); ilen += 8;
      memcpy(&pathArg[ilen], env_path, plen); ilen += plen;
      memcpy(&pathArg[ilen], "\"", 2);
    }

  char * env_ldlibpath = getenv("LD_LIBRARY_PATH");
  if (!env_ldlibpath)
    {
      ldLibPathArg = (char *) malloc(2);
      memcpy(ldLibPathArg,blank,2);
    }
  else
    {
      int ilen     = 0;
      int plen     = strlen(env_ldlibpath);
      ldLibPathArg = (char *) malloc(14 + plen + 2);
      memcpy(&ldLibPathArg[ilen], "--ld_libpath \"", 14); ilen += 14;
      memcpy(&ldLibPathArg[ilen], env_ldlibpath, plen);   ilen += plen;
      memcpy(&ldLibPathArg[ilen], "\"", 2);
    }

  /* Push XALT_RUN_UUID, XALT_DATE_TIME into the environment so that things like
   * R and python can know what job and what start time of the this run is.
   */

  setenv("XALT_RUN_UUID",uuid_str,1);
  time_t my_time = start_time;
  
  strftime(dateStr, DATESZ, "%Y_%m_%d_%H_%M_%S",localtime(&my_time));
  sprintf(fullDateStr,"%s_%d",dateStr, (int) (frac_time*10000.0));

  setenv("XALT_DATE_TIME",fullDateStr,1);
  setenv("XALT_DIR",XALT_DIR,1);

  pid  = getpid();
  ppid = getppid();

  // This routine returns either "FALSE" for nothing found or the watermark.
  xalt_vendor_note(&watermark, xalt_tracing);

  // Now base64 encode the watermark so it can be safely passed thru a system call.
  b64_watermark = base64_encode(watermark, strlen(watermark), &b64_wm_len);

  /* 
   * XALT is only recording the end record for scalar executables and
   * not the start record.
   */

  if (run_mask & BIT_SCALAR)
    {
      v = getenv("XALT_SCALAR_SAMPLING");
      if (!v)
	v = getenv("XALT_SCALAR_AND_SPSR_SAMPLING");

      if (v && strcmp(v,"yes") == 0)
	{
	  unsigned int a    = (unsigned int) clock();
	  unsigned int b    = (unsigned int) time(NULL);
	  unsigned int c    = (unsigned int) getpid();
	  unsigned int seed = mix(a,b,c);

	  srand(seed);
	  my_rand	    = (double) rand()/(double) RAND_MAX;
	}
    }

  sprintf(&rand_str[0],"%10.6f",my_rand);
  setenv("XALT_RANDOM_NUMBER",rand_str,1);

  exec_pathQ = strdup(xalt_quotestring(exec_path));
  xalt_quotestring_free();

  if ( run_mask & BIT_MPI)  
    {

      const char * run_submission = XALT_DIR "/libexec/xalt_run_submission";
      int runable = access(run_submission, X_OK);

      if (runable == -1)
        {
          run_submission_exists = 0;
          DEBUG1(stderr, "    -> Quitting => Cannot find xalt_run_submission: %s\n}\n\n", run_submission);
          reject_flag = XALT_MISSING_RUN_SUBMISSION;
          unsetenv("XALT_RUN_UUID");
          return;
        }
      run_submission_exists = 1;

      if (xalt_tracing || xalt_run_tracing)
        {
	  char * cmd2;
          asprintf(&cmd2, "LD_LIBRARY_PATH=\"%s\" PATH=\"%s\" \"%s\" --interfaceV %s --pid %d --ppid %d --syshost \"%s\" --start \"%.4f\" --end 0 --exec \"%s\" --ntasks %ld"
                   " --kind \"%s\" --uuid \"%s\" --prob %g --ngpus 0 --watermark \"%s\" %s %s -- %s", CXX_LD_LIBRARY_PATH, XALT_SYSTEM_PATH, run_submission, XALT_INTERFACE_VERSION,
		   pid, ppid, my_syshost, start_time, exec_pathQ, my_size, xalt_run_short_descriptA[xalt_kind], uuid_str, probability, watermark, pathArg, ldLibPathArg,
		   usr_cmdline);
          fprintf(stderr, "  Recording state at beginning of %s user program:\n    %s\n\n}\n\n",
                  xalt_run_short_descriptA[run_mask], cmd2);
	  free(cmd2);
        }
      asprintf(&cmdline, "LD_LIBRARY_PATH=\"%s\" PATH=\"%s\" \"%s\" --interfaceV %s --pid %d --ppid %d --syshost \"%s\" --start \"%.4f\" --end 0 --exec \"%s\" --ntasks %ld"
	       " --kind \"%s\" --uuid \"%s\" --prob %g --ngpus 0 --watermark \"%s\" %s %s -- %s", CXX_LD_LIBRARY_PATH, XALT_SYSTEM_PATH, run_submission, XALT_INTERFACE_VERSION,
	       pid, ppid, my_syshost, start_time, exec_pathQ, my_size, xalt_run_short_descriptA[xalt_kind], uuid_str, probability, b64_watermark, pathArg, ldLibPathArg,
	       b64_cmdline);

      system(cmdline);
      free(cmdline);
    }
  else
    {
      DEBUG2(stderr,"    -> XALT is build to %s, Current %s -> Not producing a start record\n}\n\n",
             xalt_build_descriptA[build_mask], xalt_run_descriptA[run_mask]);
    }

  /**********************************************************
   * Restore LD_PRELOAD after running xalt_run_submission.
   * This way the application and child apps will have
   * LD_PRELOAD set. (I'm looking at you mpiexec.hydra!)
   *********************************************************/

  if (ld_preload_strp)
    {
      setenv("LD_PRELOAD", ld_preload_strp, 1);
      free(ld_preload_strp);
    }

  /************************************************************
   * Register a signal handler wrapper_for_myfini for all the
   * important signals. This way a program terminated by
   * ^C, SIGFPE, SIGSEGV, etc will produce an end record.
   *********************************************************/
  v = getenv("XALT_SIGNAL_HANDLER");
  if (!v || strcmp(v,"no") != 0)
    {
      int signalA[] = {SIGHUP, SIGQUIT, SIGILL,  SIGTRAP, SIGABRT, SIGBUS, 
		       SIGFPE, SIGSEGV, SIGTERM, SIGXCPU, SIGUSR1};
      int signalSz  = N_ELEMENTS(signalA);
      struct sigaction action;
      struct sigaction old;
      memset(&action, 0, sizeof(struct sigaction));
      action.sa_handler = wrapper_for_myfini;
      for (i = 0; i < signalSz; ++i)
        {
          int signum = signalA[i];

          sigaction(signum, NULL, &old);

          if (old.sa_handler == NULL)
            sigaction(signum, &action, NULL);
        }
    }
}
void wrapper_for_myfini(int signum)
{
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  sigemptyset( &action.sa_mask);
  action.sa_handler = SIG_DFL;
  sigaction(signum, &action, NULL);
  myfini();
  raise(signum);
}

void myfini()
{
  FILE * my_stderr = NULL;
  char * cmdline;
  double run_time;
  int    xalt_err = xalt_tracing || xalt_run_tracing;

  if (xalt_err)
    {
      fflush(stderr);
      close(STDERR_FILENO);
      dup2(errfd, STDERR_FILENO);
      my_stderr = fdopen(errfd,"w");
      DEBUG1(my_stderr,"\nmyfini(%s){\n", STR(STATE));
    }

  /* Stop tracking if my mpi rank is not zero or the path was rejected. */
  if (reject_flag != XALT_SUCCESS)
    {
      if (xalt_kind == BIT_PKGS)
        remove_xalt_tmpdir(&uuid_str[0]);

      DEBUG2(my_stderr,"    -> exiting because reject is set to: %s for program: %s\n}\n\n",
	     xalt_reasonA[reject_flag], exec_path);
      if (xalt_err)
	{
	  fclose(my_stderr);
	  close(errfd);
	  close(STDERR_FILENO);
	}
      return;
    }

  end_time = epoch();
  unsetenv("LD_PRELOAD");

#if USE_DCGM || USE_NVML
  /* This code will only ever be active in 64 bit mode and not 32 bit mode */
  if (xalt_gpu_tracking)
    {
#ifdef USE_NVML
      nvmlReturn_t result;
      unsigned int device_count = 0;

      /* Get the number of GPU devices in the system */
      result = _nvmlDeviceGetCount(&device_count);
      if (result == NVML_SUCCESS)
        {
          DEBUG1(my_stderr, "  %d GPUs detected\n", device_count);

          /* Loop over GPU devices */
          unsigned int i = 0;
          for (i = 0 ; i < device_count ; i++)
            {
              nvmlDevice_t device;
              nvmlEnableState_t mode;
              unsigned int pid_count = 0, max_pid_count = 0;
              unsigned int *pids = 0;
              unsigned int num_active_pids = 0;

              /* Get a device handle */
              result = _nvmlDeviceGetHandleByIndex(i, &device);
              if (result != NVML_SUCCESS)
                {
                  DEBUG2(my_stderr, "  Unable to get device handle for GPU %d: %s\n", i, _nvmlErrorString(result));
                  continue;
                }

              /* Check whether accounting mode is enabled for this device */
              result = _nvmlDeviceGetAccountingMode(device, &mode);
              if (result != NVML_SUCCESS)
                {
                  DEBUG2(my_stderr, "  Unable to get accounting mode for GPU %d: %s\n", i, _nvmlErrorString(result));
                  continue;
                }
              if (mode != NVML_FEATURE_ENABLED)
                {
                  DEBUG2(my_stderr, "  Accounting mode is not enabled for GPU %d. Enable accounting mode: sudo nvidia-smi -i %d -am 1\n", i, i);
                  continue;
                }

              /* Get the size of the accounting buffer */
              result = _nvmlDeviceGetAccountingBufferSize(device, &max_pid_count);
              if (result != NVML_SUCCESS)
                {
                  DEBUG2(my_stderr, "  Unable to get the accounting buffer size for GPU %d: %s\n", i, _nvmlErrorString(result));
                  continue;
                }

              /* Allocate space for the accounting data */
              pids = (unsigned int *)malloc(sizeof(unsigned int)*max_pid_count);
              memset(pids, 0, sizeof(unsigned int)*max_pid_count);

              /* Get PID accounting data */
              pid_count = max_pid_count;
              result = _nvmlDeviceGetAccountingPids(device, &pid_count, pids);
              if (result != NVML_SUCCESS)
                {
                  DEBUG2(my_stderr, "  Unable to get accounting data for GPU %d: %s\n", i, _nvmlErrorString(result));
                  free(pids);
                  continue;
                }

              /* Loop over the PID accounting data looking for PIDs that
               * started after the program. Count those as belonging to
               * this program execution. */
              unsigned int j = 0;
              for (j = 0 ; j < pid_count ; j++)
                {
                  nvmlAccountingStats_t stats;

                  result = _nvmlDeviceGetAccountingStats(device, pids[j], &stats);
                  if (result == NVML_SUCCESS)
                    {
                      /* If the GPU PID start time is later than the
                         program start time, consider the GPU PID to
                         belong to the program.  This would not
                         necessarily be true if the system is
                         shared by unrelated programs. */
                      if (stats.startTime >= __time)
                        {
                          /* There is a mystery PID that appears for all
                             GPUs.  The PID is constant and is assumed
                             to be a CUDA cleanup process.  That PID
                             has a maxMemoryUsage of 0 while a real
                             PID will always have a non-zero value, so
                             use that to exclude it. */
                          if (stats.maxMemoryUsage > 0)
                            {
                              num_active_pids++;
                              DEBUG4(my_stderr, "  PID %d startTime=%llu time=%llu isRunning=%d\n", pids[j], stats.startTime, stats.time, stats.isRunning);
                            }
                        }
                      /* Note that the GPU compute process has not
                         terminated yet.  Therefore stats.isRunning ==
                         1, and stats.time == 0, so it is not possible
                         to get the amount of time the GPU device was
                         actually in use. */
                    }
                }

              free(pids);

              DEBUG2(my_stderr, "  GPU %d: num compute pids %d\n", i, num_active_pids);

              if (num_active_pids > 0) {
                num_gpus++;
              }
            }

          /* Cleanup */
          result = _nvmlShutdown();
          if (result != NVML_SUCCESS)
            {
              DEBUG1(my_stderr, "  Error shutting down NVML: %s\n", _nvmlErrorString(result));
            }
          dlclose(nvml_handle);
        }
#elif USE_DCGM
      if (dcgm_handle != NULL)
        {
          /* Collect DCGM job stats */
          dcgmReturn_t result;
          dcgmJobInfo_t job_info;

          DEBUG0(my_stderr, "  GPU tracing\n");

          dcgmUpdateAllFields(dcgm_handle, 1);
          dcgmJobStopStats(dcgm_handle, uuid_str);

          job_info.version = dcgmJobInfo_version2;
          result = dcgmJobGetStats(dcgm_handle, uuid_str, &job_info);
          if (result == DCGM_ST_OK)
            {
              int i = 0;
              DEBUG1(my_stderr, "  %d GPUs detected\n", job_info.numGpus);
              for (i = 0 ; i < job_info.numGpus ; i++)
                {
                  DEBUG2(my_stderr, "  GPU %d: num compute pids %d\n", i, job_info.gpus[i].numComputePids);
                  if (job_info.gpus[i].numComputePids > 0)
                    num_gpus++;
                }
              DEBUG2(my_stderr, "  %d of %d GPUs were used\n", num_gpus, job_info.numGpus);
            }

          dcgmJobRemove(dcgm_handle, uuid_str);
          dcgmStopEmbedded(dcgm_handle);
          dcgmShutdown();
        }
#endif
    }
#endif

  if (run_mask & BIT_SCALAR)
    {
      const char * v;
      v = getenv("XALT_SCALAR_SAMPLING");
      if (!v)
	v = getenv("XALT_SCALAR_AND_SPSR_SAMPLING");

      if (v && strcmp(v,"yes") == 0)
	{
	  double run_time = end_time - start_time;
	  probability     = scalar_program_sample_probability(run_time);

	  if (my_rand >= probability)
	    {
	      DEBUG4(my_stderr, "    -> exiting because scalar sampling. "
		     "run_time: %g, (my_rand: %g > prob: %g) for program: %s\n}\n\n",
		     run_time, my_rand, probability, exec_path);
	      if (xalt_err) 
		{
		  fclose(my_stderr);
		  close(errfd);
		  close(STDERR_FILENO);
		}
	      return;
	    }
	  else
	    DEBUG4(my_stderr, "    -> Scalar Sampling program run_time: %g: (my_rand: %g <= prob: %g) for program: %s\n", 
		   run_time, my_rand, probability, exec_path);
	}
    }
  
  const char * run_submission = XALT_DIR "/libexec/xalt_run_submission";
  int runable = (run_submission_exists == 1) ? 1 : access(run_submission, X_OK);
  if (runable == -1)
    {
      DEBUG1(my_stderr, "    -> Quitting => Cannot find xalt_run_submission: %s\n}\n\n", run_submission);
      reject_flag = XALT_MISSING_RUN_SUBMISSION;
    }
  else
    {
      if (xalt_tracing || xalt_run_tracing )
        {
          int    dLen;
	  char * cmd2    = NULL;
          char * decoded = (char *) base64_decode(b64_cmdline, strlen(b64_cmdline), &dLen);
          asprintf(&cmd2, "LD_LIBRARY_PATH=\"%s\" PATH=\"%s\" \"%s\" --interfaceV %s --pid %d --ppid %d --syshost \"%s\" --start \"%.4f\" --end \"%.4f\" --exec \"%s\""
                   " --ntasks %ld --kind \"%s\" --uuid \"%s\" --prob %g --ngpus %d --watermark \"%s\" %s %s -- %s", CXX_LD_LIBRARY_PATH, XALT_SYSTEM_PATH, run_submission,
		   XALT_INTERFACE_VERSION, pid, ppid, my_syshost, start_time, end_time, exec_pathQ, my_size, xalt_run_short_descriptA[xalt_kind], uuid_str,
		   probability, num_gpus, watermark, pathArg, ldLibPathArg, decoded);
          //		   probability, num_gpus, watermark, pathArg, ldLibPathArg, decoded);
	  fprintf(my_stderr,"  len: %u, b64_cmd: %s\n", (unsigned int) strlen(b64_cmdline), b64_cmdline);
          fprintf(my_stderr,"  Recording State at end of %s user program:\n    %s\n}\n\n",
                  xalt_run_short_descriptA[run_mask], cmd2);
	  fflush(my_stderr);
        }
      asprintf(&cmdline, "LD_LIBRARY_PATH=\"%s\" PATH=\"%s\" \"%s\" --interfaceV %s --pid %d --ppid %d --syshost \"%s\" --start \"%.4f\" --end \"%.4f\" --exec \"%s\""
               " --ntasks %ld --kind \"%s\" --uuid \"%s\" --prob %g --ngpus %d --watermark \"%s\" %s %s -- %s", CXX_LD_LIBRARY_PATH, XALT_SYSTEM_PATH, run_submission,
	       XALT_INTERFACE_VERSION, pid, ppid, my_syshost, start_time, end_time, exec_pathQ, my_size, xalt_run_short_descriptA[xalt_kind], uuid_str,
	       probability, num_gpus, b64_watermark, pathArg, ldLibPathArg, b64_cmdline);

      system(cmdline);
    }

  if (xalt_err) 
    {
      fclose(my_stderr);
      close(errfd);
      close(STDERR_FILENO);
    }
}

#ifdef USE_NVML
static int load_nvml()
{
  /* Open the NVML library.  Let the dynamic loader find it (do not
     specify a path). */
  nvml_handle = dlopen("libnvidia-ml.so", RTLD_LAZY);
  if (! nvml_handle)
    {
      DEBUG1(stderr, "    -> Unable to open libnvidia-ml.so: %s\n\n",
             dlerror());
      return 0;
    }

  /* Load symbols */
  *(void**)(&_nvmlDeviceGetAccountingBufferSize) =
    dlsym(nvml_handle, "nvmlDeviceGetAccountingBufferSize");
  *(void**)(&_nvmlDeviceGetAccountingMode) =
    dlsym(nvml_handle, "nvmlDeviceGetAccountingMode");
  *(void**)(&_nvmlDeviceGetAccountingPids) =
    dlsym(nvml_handle, "nvmlDeviceGetAccountingPids");
  *(void**)(&_nvmlDeviceGetAccountingStats) =
    dlsym(nvml_handle, "nvmlDeviceGetAccountingStats");
  *(void**)(&_nvmlDeviceGetCount) = dlsym(nvml_handle, "nvmlDeviceGetCount");
  *(void**)(&_nvmlDeviceGetHandleByIndex) =
    dlsym(nvml_handle, "nvmlDeviceGetHandleByIndex");
  *(void**)(&_nvmlErrorString) = dlsym(nvml_handle, "nvmlErrorString");
  *(void**)(&_nvmlInit) = dlsym(nvml_handle, "nvmlInit");
  *(void**)(&_nvmlShutdown) = dlsym(nvml_handle, "nvmlShutdown");

  return 1;
}
#endif

static long compute_value(const char **envA)
{
  long          value = 0L;
  const char ** p;
  for (p = &envA[0]; *p; ++p)
    {
      char *v = getenv(*p);
      if (v)
	{
	  value += strtol(v, (char **) NULL, 10);
	  break;
	}
    }
  return value;
}

/* Get full absolute path to executable */
/* works for Linux and Mac OS X */
static void get_abspath(char * path, int sz)
{
  path[0] = '\0';
  #ifdef __MACH__
    int iret = proc_pidpath(getpid(), path, sz-1);
    if (iret <= 0)
      {
	fprintf(stderr,"PID %ud: proc_pid();\n",getpid());
	fprintf(stderr,"    %s:\n", strerror(errno));
      }
  #else
    readlink("/proc/self/exe",path,sz-1);
  #endif
}

static volatile double epoch()
{
  struct timeval tm;
  gettimeofday(&tm, 0);
  return tm.tv_sec + 1.0e-6*tm.tv_usec;
}

static unsigned int mix(unsigned int a, unsigned int b, unsigned int c)
{
  a=a-b;  a=a-c;  a=a^(c >> 13);
  b=b-c;  b=b-a;  b=b^(a << 8);
  c=c-a;  c=c-b;  c=c^(b >> 13);
  a=a-b;  a=a-c;  a=a^(c >> 12);
  b=b-c;  b=b-a;  b=b^(a << 16);
  c=c-a;  c=c-b;  c=c^(b >> 5);
  a=a-b;  a=a-c;  a=a^(c >> 3);
  b=b-c;  b=b-a;  b=b^(a << 10);
  c=c-a;  c=c-b;  c=c^(b >> 15);
  return c;
}

static  double scalar_program_sample_probability(double runtime)
{
  double prob = 1.0;
  int i;
  for (i = 0; i < rangeSz-1; ++i)
    {
      if (runtime < rangeA[i+1].left)
  	{
  	  prob = rangeA[i].prob;
  	  break;
  	}
    }
  return prob;
}  


#ifdef __MACH__
  __attribute__((section("__DATA,__mod_init_func"), used, aligned(sizeof(void*)))) __typeof__(myinit) *__init = myinit;
  __attribute__((section("__DATA,__mod_term_func"), used, aligned(sizeof(void*)))) __typeof__(myfini) *__fini = myfini;
#else
  __attribute__((section(".init_array"))) __typeof__(myinit) *__init = myinit;
  __attribute__((section(".fini_array"))) __typeof__(myfini) *__fini = myfini;
#endif
