/*
 *  This file is part of the Jikes RVM project (http://jikesrvm.org).
 *
 *  This file is licensed to You under the Eclipse Public License (EPL);
 *  You may not use this file except in compliance with the License. You
 *  may obtain a copy of the License at
 *
 *      http://www.opensource.org/licenses/eclipse-1.0.php
 *
 *  See the COPYRIGHT.txt file distributed with this work for information
 *  regarding copyright ownership.
 */

/*
 * The initial loader of the VM.
 *
 * It deals with loading of the vm boot image into a memory segment,
 * basic processing of command line arguments, and branching to VM.boot.
 *
 * It also provides C runtime support for the virtual machine. The files
 * sys*.c contain the support services to match the entrypoints
 * declared by org.jikesrvm.runtime.Syscall .
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/signal.h>
#include <ctype.h>              // isspace()
#include <limits.h>             // UINT_MAX, ULONG_MAX, etc
#include <strings.h> /* bzero */
#include <libgen.h>  /* basename */
#include <sys/utsname.h>        // for uname(2)
#include <sys/mman.h>        // for uname(2)
#if (defined __linux__) || (defined (__SVR4) && defined (__sun))
#include <ucontext.h>
#include <signal.h>
#elif (defined __MACH__)
#include <sys/ucontext.h>
#include <signal.h>
#else
#include <sys/cache.h>
#include <sys/context.h>
// extern "C" char *sys_siglist[];
#endif

// Interface to VM data structures.
//
#define NEED_BOOT_RECORD_INITIALIZATION 1

#include "bootloader.h"       /* Automatically generated for us by the build */

#include "sys.h"

Extent initialHeapSize;
Extent maximumHeapSize;
Extent pageSize;

/** Verbose boot up set */
int verboseBoot = 0;

/** File name for part of boot image containing code */
static char *bootCodeFilename;

/** File name for part of boot image containing data */
static char *bootDataFilename;

/** File name for part of boot image containing the root map */
static char *bootRMapFilename;

void findMappable();
Extent determinePageSize();

// Definitions of constants for handling C command-line arguments

/* These definitions must remain in sync with nonStandardArgs, the array
 * immediately below. */
#define HELP_INDEX                    0
#define VERBOSE_INDEX                 HELP_INDEX + 1
#define VERBOSE_BOOT_INDEX            VERBOSE_INDEX + 1
#define MS_INDEX                      VERBOSE_BOOT_INDEX + 1
#define MX_INDEX                      MS_INDEX + 1
#define SYSLOGFILE_INDEX              MX_INDEX + 1
#define BOOTIMAGE_CODE_FILE_INDEX     SYSLOGFILE_INDEX + 1
#define BOOTIMAGE_DATA_FILE_INDEX     BOOTIMAGE_CODE_FILE_INDEX + 1
#define BOOTIMAGE_RMAP_FILE_INDEX     BOOTIMAGE_DATA_FILE_INDEX + 1
#define INDEX                      BOOTIMAGE_RMAP_FILE_INDEX + 1
#define GC_INDEX                      INDEX + 1
#define AOS_INDEX                     GC_INDEX + 1
#define IRC_INDEX                     AOS_INDEX + 1
#define RECOMP_INDEX                  IRC_INDEX + 1
#define BASE_INDEX                    RECOMP_INDEX + 1
#define OPT_INDEX                     BASE_INDEX + 1
#define VMCLASSES_INDEX               OPT_INDEX + 1
#define BOOTCLASSPATH_P_INDEX         VMCLASSES_INDEX + 1
#define BOOTCLASSPATH_A_INDEX         BOOTCLASSPATH_P_INDEX + 1
#define PROCESSORS_INDEX              BOOTCLASSPATH_A_INDEX + 1

#define numNonstandardArgs      PROCESSORS_INDEX + 1

static const char* nonStandardArgs[numNonstandardArgs] = {
  "-X",
  "-X:verbose",
  "-X:verboseBoot=",
  "-Xms",
  "-Xmx",
  "-X:sysLogfile=",
  "-X:ic=",
  "-X:id=",
  "-X:ir=",
  "-X:vm",
  "-X:gc",
  "-X:aos",
  "-X:irc",
  "-X:recomp",
  "-X:base",
  "-X:opt",
  "-X:vmClasses=",
  "-Xbootclasspath/p:",
  "-Xbootclasspath/a:",
  "-X:availableProcessors=",
};

// a NULL-terminated list.
static const char* nonStandardUsage[] = {
  "  -X                         Print usage on nonstandard options",
  "  -X:verbose                 Print out additional lowlevel information",
  "  -X:verboseBoot=<number>    Print out messages while booting VM",
  "  -Xms<number><unit>         Initial size of heap",
  "  -Xmx<number><unit>         Maximum size of heap",
  "  -X:sysLogfile=<filename>   Write standard error message to <filename>",
  "  -X:ic=<filename>           Read boot image code from <filename>",
  "  -X:id=<filename>           Read boot image data from <filename>",
  "  -X:ir=<filename>           Read boot image ref map from <filename>",
  "  -X:vm:<option>             Pass <option> to virtual machine",
  "        :help                Print usage choices for -X:vm",
  "  -X:gc:<option>             Pass <option> on to GC subsystem",
  "        :help                Print usage choices for -X:gc",
  "  -X:aos:<option>            Pass <option> on to adaptive optimization system",
  "        :help                Print usage choices for -X:aos",
  "  -X:irc:<option>            Pass <option> on to the initial runtime compiler",
  "        :help                Print usage choices for -X:irc",
  "  -X:recomp:<option>         Pass <option> on to the recompilation compiler(s)",
  "        :help                Print usage choices for -X:recomp",
  "  -X:base:<option>           Pass <option> on to the baseline compiler",
  "        :help                print usage choices for -X:base",
  "  -X:opt:<option>            Pass <option> on to the optimizing compiler",
  "        :help                Print usage choices for -X:opt",
  "  -X:vmClasses=<path>        Load the org.jikesrvm.* and java.* classes",
  "                             from <path>, a list like one would give to the",
  "                             -classpath argument.",
  "  -Xbootclasspath/p:<cp>     (p)repend bootclasspath with specified classpath",
  "  -Xbootclasspath/a:<cp>     (a)ppend specified classpath to bootclasspath",
  "  -X:availableProcessors=<n> desired level of application parallelism (set",
  "                             -X:gc:threads to control gc parallelism)",
  NULL                         /* End of messages */
};

/*
 * What standard command line arguments are supported?
 */
static void
usage(void)
{
  CONSOLE_PRINTF("Usage: %s [-options] class [args...]\n", Me);
  CONSOLE_PRINTF("          (to execute a class)\n");
  CONSOLE_PRINTF("   or  %s [-options] -jar jarfile [args...]\n",Me);
  CONSOLE_PRINTF("          (to execute a jar file)\n");
  CONSOLE_PRINTF("\nwhere options include:\n");
  CONSOLE_PRINTF("    -cp -classpath <directories and zip/jar files separated by :>\n");
  CONSOLE_PRINTF("              set search path for application classes and resources\n");
  CONSOLE_PRINTF("    -D<name>=<value>\n");
  CONSOLE_PRINTF("              set a system property\n");
  CONSOLE_PRINTF("    -verbose[:class|:gc|:jni]\n");
  CONSOLE_PRINTF("              enable verbose output\n");
  CONSOLE_PRINTF("    -version  print version\n");
  CONSOLE_PRINTF("    -showversion\n");
  CONSOLE_PRINTF("              print version and continue\n");
  CONSOLE_PRINTF("    -fullversion\n");
  CONSOLE_PRINTF("              like version but with more information\n");
  CONSOLE_PRINTF("    -? -help  print this message\n");
  CONSOLE_PRINTF("    -X        print help on non-standard options\n");
  CONSOLE_PRINTF("    -javaagent:<jarpath>[=<options>]\n");
  CONSOLE_PRINTF("              load Java programming language agent, see java.lang.instrument\n");

  CONSOLE_PRINTF("\n For more information see http://jikesrvm.sourceforge.net\n");

  CONSOLE_PRINTF("\n");
}

/*
 * What nonstandard command line arguments are supported?
 */
static void
nonstandard_usage()
{
  const char * const *msgp = nonStandardUsage;
  CONSOLE_PRINTF("Usage: %s [options] class [args...]\n",Me);
  CONSOLE_PRINTF("          (to execute a class)\n");
  CONSOLE_PRINTF("where options include\n");
  for (; *msgp; ++msgp) {
    CONSOLE_PRINTF( "%s\n", *msgp);
  }
}

static void
shortVersion()
{
  CONSOLE_PRINTF( "%s %s\n",rvm_configuration, rvm_version);
}

static void
fullVersion()
{
  shortVersion();
  CONSOLE_PRINTF( "\thost config: %s\n\ttarget config: %s\n",
                  rvm_host_configuration, rvm_target_configuration);
  CONSOLE_PRINTF( "\theap default initial size: %u MiBytes\n",
                  heap_default_initial_size/(1024*1024));
  CONSOLE_PRINTF( "\theap default maximum size: %u MiBytes\n",
                  heap_default_maximum_size/(1024*1024));
}

/*
 * Identify all command line arguments that are VM directives.
 * VM directives are positional, they must occur before the application
 * class or any application arguments are specified.
 *
 * Identify command line arguments that are processed here:
 *   All heap memory directives. (e.g. -X:h).
 *   Any informational messages (e.g. -help).
 *
 * Input an array of command line arguments.
 * Return an array containing application arguments and VM arguments that
 *        are not processed here.
 * Side Effect  global varable JavaArgc is set.
 *
 * We reuse the array 'CLAs' to contain the return values.  We're
 * guaranteed that we will not generate any new command-line arguments, but
 * only consume them. So, n_JCLAs indexes 'CLAs', and it's always the case
 * that n_JCLAs <= n_CLAs, and is always true that n_JCLAs <= i (CLA index).
 *
 * By reusing CLAs, we avoid any unpleasantries with memory allocation.
 *
 * In case of trouble, we set fastExit.  We call exit(0) if no trouble, but
 * still want to exit.
 */
static const char **
processCommandLineArguments(const char *CLAs[], int n_CLAs, int *fastExit)
{
  int n_JCLAs = 0;
  int startApplicationOptions = 0;
  int i;
  const char *subtoken;

  for (i = 0; i < n_CLAs; i++) {
    const char *token = CLAs[i];
    subtoken = NULL;        // strictly, not needed.

    // examining application options?
    if (startApplicationOptions) {
      CLAs[n_JCLAs++]=token;
      continue;
    }
    // pass on all command line arguments that do not start with a dash, '-'.
    if (token[0] != '-') {
      CLAs[n_JCLAs++]=token;
      ++startApplicationOptions;
      continue;
    }

    //   while (*argv && **argv == '-')    {
    if (STREQUAL(token, "-help") || STREQUAL(token, "-?") ) {
      usage();
      *fastExit = 1;
      break;
    }
    if (STREQUAL(token, nonStandardArgs[HELP_INDEX])) {
      nonstandard_usage();
      *fastExit = 1;
      break;
    }
    if (STREQUAL(token, nonStandardArgs[VERBOSE_INDEX])) {
      ++verbose;
      continue;
    }
    if (STRNEQUAL(token, nonStandardArgs[VERBOSE_BOOT_INDEX], 15)) {
      subtoken = token + 15;
      errno = 0;
      char *endp;
      long vb = strtol(subtoken, &endp, 0);
      while (*endp && isspace(*endp)) // gobble trailing spaces
        ++endp;

      if (vb < 0) {
        CONSOLE_PRINTF("%s: \"%s\": You may not specify a negative verboseBoot value\n", Me, token);
        *fastExit = 1;
        break;
      } else if (errno == ERANGE
                 || vb > INT_MAX ) {
        CONSOLE_PRINTF("%s: \"%s\": too big a number to represent internally\n", Me, token);
        *fastExit = 1;
        break;
      } else if (*endp) {
        CONSOLE_PRINTF("%s: \"%s\": I don't recognize \"%s\" as a number\n", Me, token, subtoken);
        *fastExit = 1;
        break;
      }

      verboseBoot = vb;
      continue;
    }
    /*  Args that don't apply to us (from the Sun JVM); skip 'em. */
    if (STREQUAL(token, "-server"))
      continue;
    if (STREQUAL(token, "-client"))
      continue;
    if (STREQUAL(token, "-version")) {
      shortVersion();
      exit(0);
    }
    if (STREQUAL(token, "-fullversion")) {
      fullVersion();
      exit(0);
    }
    if (STREQUAL(token, "-showversion")) {
      shortVersion();
      continue;
    }
    if (STREQUAL(token, "-showfullversion")) {
      fullVersion();
      continue;
    }
    if (STREQUAL(token, "-findMappable")) {
      findMappable();
      exit(0);            // success, no?
    }
    if (STRNEQUAL(token, "-verbose:gc", 11)) {
      long level;         // a long, since we need to use strtol()
      if (token[11] == '\0') {
        level = 1;
      } else {
        /* skip to after the "=" in "-verbose:gc=<num>" */
        subtoken = token + 12;
        errno = 0;
        char *endp;
        level = strtol(subtoken, &endp, 0);
        while (*endp && isspace(*endp)) // gobble trailing spaces
          ++endp;

        if (level < 0) {
          CONSOLE_PRINTF( "%s: \"%s\": You may not specify a negative GC verbose value\n", Me, token);
          *fastExit = 1;
        } else if (errno == ERANGE || level > INT_MAX ) {
          CONSOLE_PRINTF( "%s: \"%s\": too big a number to represent internally\n", Me, token);
          *fastExit = 1;
        } else if (*endp) {
          CONSOLE_PRINTF( "%s: \"%s\": I don't recognize \"%s\" as a number\n", Me, token, subtoken);
          *fastExit = 1;
        }
        if (*fastExit) {
          CONSOLE_PRINTF( "%s: please specify GC verbose level as  \"-verbose:gc=<number>\" or as \"-verbose:gc\"\n", Me);
          break;
        }
      }
      /* Canonicalize the argument, and pass it on to the heavy-weight
       * Java code that parses -X:gc:verbose */
      const size_t bufsiz = 20;
      char *buf = (char *) checkMalloc(bufsiz);
      int ret = snprintf(buf, bufsiz, "-X:gc:verbose=%ld", level);
      if (ret < 0) {
        ERROR_PRINTF("%s: Internal error processing the argument"
                     " \"%s\"\n", Me, token);
        exit(EXIT_STATUS_IMPOSSIBLE_LIBRARY_FUNCTION_ERROR);
      }
      if ((unsigned) ret >= bufsiz) {
        ERROR_PRINTF( "%s: \"%s\": %ld is too big a number"
                      " to process internally\n", Me, token, level);
        *fastExit = 1;
        break;
      }

      CLAs[n_JCLAs++] = buf; // Leave buf allocated!
      continue;
    }

    if (STRNEQUAL(token, nonStandardArgs[MS_INDEX], 4)) {
      subtoken = token + 4;
      initialHeapSize
        = parse_memory_size("initial heap size", "ms", "", pageSize,
                            token, subtoken, fastExit);
      if (*fastExit)
        break;
      continue;
    }

    if (STRNEQUAL(token, nonStandardArgs[MX_INDEX], 4)) {
      subtoken = token + 4;
      maximumHeapSize
        = parse_memory_size("maximum heap size", "mx", "", pageSize,
                            token, subtoken, fastExit);
      if (*fastExit)
        break;
      continue;
    }

    if (STRNEQUAL(token, nonStandardArgs[SYSLOGFILE_INDEX],14)) {
      subtoken = token + 14;
      FILE* ftmp = fopen(subtoken, "a");
      if (!ftmp) {
        CONSOLE_PRINTF( "%s: can't open SysTraceFile \"%s\": %s\n", Me, subtoken, strerror(errno));
        *fastExit = 1;
        break;
        continue;
      }
      CONSOLE_PRINTF( "%s: redirecting sysWrites to \"%s\"\n",Me, subtoken);
      SysTraceFile = ftmp;
      continue;
    }
    if (STRNEQUAL(token, nonStandardArgs[BOOTIMAGE_CODE_FILE_INDEX], 6)) {
      bootCodeFilename = token + 6;
      continue;
    }
    if (STRNEQUAL(token, nonStandardArgs[BOOTIMAGE_DATA_FILE_INDEX], 6)) {
      bootDataFilename = token + 6;
      continue;
    }
    if (STRNEQUAL(token, nonStandardArgs[BOOTIMAGE_RMAP_FILE_INDEX], 6)) {
      bootRMapFilename = token + 6;
      continue;
    }

    //
    // All VM directives that are not handled here but in VM.java
    // must be identified.
    //

    // All VM directives that take one token
    if (STRNEQUAL(token, "-D", 2)
        || STRNEQUAL(token, nonStandardArgs[INDEX], 5)
        || STRNEQUAL(token, nonStandardArgs[GC_INDEX], 5)
        || STRNEQUAL(token, nonStandardArgs[AOS_INDEX],6)
        || STRNEQUAL(token, nonStandardArgs[IRC_INDEX], 6)
        || STRNEQUAL(token, nonStandardArgs[RECOMP_INDEX], 9)
        || STRNEQUAL(token, nonStandardArgs[BASE_INDEX],7)
        || STRNEQUAL(token, nonStandardArgs[OPT_INDEX], 6)
        || STREQUAL(token, "-verbose")
        || STREQUAL(token, "-verbose:class")
        || STREQUAL(token, "-verbose:gc")
        || STREQUAL(token, "-verbose:jni")
        || STRNEQUAL(token, "-javaagent:", 11)
        || STRNEQUAL(token, nonStandardArgs[VMCLASSES_INDEX], 13)
        || STRNEQUAL(token, nonStandardArgs[BOOTCLASSPATH_P_INDEX], 18)
        || STRNEQUAL(token, nonStandardArgs[BOOTCLASSPATH_A_INDEX], 18)
        || STRNEQUAL(token, nonStandardArgs[PROCESSORS_INDEX], 14))
    {
      CLAs[n_JCLAs++]=token;
      continue;
    }
    // All VM directives that take two tokens
    if (STREQUAL(token, "-cp") || STREQUAL(token, "-classpath")) {
      CLAs[n_JCLAs++]=token;
      token=CLAs[++i];
      CLAs[n_JCLAs++]=token;
      continue;
    }

    CLAs[n_JCLAs++]=token;
    ++startApplicationOptions; // found one that we do not recognize;
    // start to copy them all blindly
  } // for ()

  /* and set the count */
  JavaArgc = n_JCLAs;
  return CLAs;
}


/**
 * Map the given file to memory
 *
 * Taken:     fileName         [in] name of file
 *            targetAddress    [in] address to load file to
 *            executable       [in] are we mapping code into memory
 *            writable         [in] do we need to write to this memory?
 *            roundedImageSize [out] size of mapped memory rounded up to a whole
 * Returned:  address of mapped region
 */
static void* mapImageFile(const char *fileName, const void *targetAddress,
                          jboolean executable, jboolean writable, Extent *roundedImageSize) {
  Extent actualImageSize;
  void *bootRegion = 0;
  TRACE_PRINTF("%s: mapImageFile \"%s\" to %p\n", Me, fileName, targetAddress);
  FILE *fin = fopen (fileName, "r");
  if (!fin) {
    ERROR_PRINTF("%s: can't find bootimage file\"%s\"\n", Me, fileName);
    return 0;
  }
  /* measure image size */
  fseek (fin, 0L, SEEK_END);
  actualImageSize = (uint64_t) ftell(fin);
  *roundedImageSize = pageRoundUp(actualImageSize, pageSize);
  fseek (fin, 0L, SEEK_SET);
  int prot = PROT_READ;
  if (writable)
    prot |= PROT_WRITE;
  if (executable)
    prot |= PROT_EXEC;
  bootRegion = mmap((void*)targetAddress, *roundedImageSize,
       prot,
       MAP_FIXED | MAP_PRIVATE | MAP_NORESERVE,
       fileno(fin), 0);
  if (bootRegion == (void *) MAP_FAILED) {
    ERROR_PRINTF("%s: mmap failed (errno=%d): %s\n", Me, errno, strerror(errno));
    return 0;
  }
  /* Quoting from the Linux mmap(2) manual page:
     "closing the file descriptor does not unmap the region."
  */
  if (fclose (fin) != 0) {
    ERROR_PRINTF("%s: close failed (errno=%d)\n", Me, errno);
    return 0;
  }
  if (bootRegion != targetAddress) {
    ERROR_PRINTF("%s: Attempted to mapImageFile to the address %p; "
    " got %p instead.  This should never happen.",
    Me, targetAddress, bootRegion);
    (void) munmap(bootRegion, *roundedImageSize);
    return 0;
  }
  return bootRegion;
}


/**
 * Start the VM
 *
 * Taken:     vmInSeparateThread [in] create a thread for the VM to
 * execute in rather than this thread
 * Returned:  1 upon any errors.  Never returns except to report an
 * error.
 */
static int createVM(int vmInSeparateThread)
{
  Extent roundedDataRegionSize;
  // Note that the data segment must be mapped as executable
  // because code for lazy compilation trampolines is placed
  // in the TIBs and TIBs are placed in the data segment.
  // See RVM-678.
  void *bootDataRegion = mapImageFile(bootDataFilename,
             bootImageDataAddress,
             JNI_TRUE,
                                      JNI_TRUE,
             &roundedDataRegionSize);
  if (bootDataRegion != bootImageDataAddress)
    return 1;

  Extent roundedCodeRegionSize;
  // Note that the code segment must be mapped as writable because the
  // optimizing compiler may try to patch methods in the boot image. If the
  // code from the boot image were write-protected, this would cause a
  // segmentation fault, which would manifest as a NullPointerException
  // with the current implementation (May 2015). If we wanted to have
  // read-only code for the boot image, we would need to make sure that
  // it is never necessary to patch code from the boot image.
  void *bootCodeRegion = mapImageFile(bootCodeFilename,
             bootImageCodeAddress,
             JNI_TRUE,
                                      JNI_TRUE,
             &roundedCodeRegionSize);
  if (bootCodeRegion != bootImageCodeAddress)
    return 1;

  Extent roundedRMapRegionSize;
  void *bootRMapRegion = mapImageFile(bootRMapFilename,
             bootImageRMapAddress,
             JNI_FALSE,
                                      JNI_FALSE,
             &roundedRMapRegionSize);
  if (bootRMapRegion != bootImageRMapAddress)
    return 1;


  /* validate contents of boot record */
  bootRecord = (struct BootRecord *) bootDataRegion;

  if (bootRecord->bootImageDataStart != (Address) bootDataRegion) {
    ERROR_PRINTF("%s: image load error: built for %p but loaded at %p\n",
     Me, bootRecord->bootImageDataStart, bootDataRegion);
    return 1;
  }

  if (bootRecord->bootImageCodeStart != (Address) bootCodeRegion) {
    ERROR_PRINTF("%s: image load error: built for %p but loaded at %p\n",
     Me, bootRecord->bootImageCodeStart, bootCodeRegion);
    return 1;
  }

  if (bootRecord->bootImageRMapStart != (Address) bootRMapRegion) {
    ERROR_PRINTF("%s: image load error: built for %p but loaded at %p\n",
     Me, bootRecord->bootImageRMapStart, bootRMapRegion);
    return 1;
  }

  if ((bootRecord->spRegister % __SIZEOF_POINTER__) != 0) {
    ERROR_PRINTF("%s: image format error: sp (%p) is not word aligned\n",
     Me, bootRecord->spRegister);
    return 1;
  }

  if ((bootRecord->ipRegister % __SIZEOF_POINTER__) != 0) {
    ERROR_PRINTF("%s: image format error: ip (%p) is not word aligned\n",
     Me, bootRecord->ipRegister);
    return 1;
  }

  if (((u_int32_t *) bootRecord->spRegister)[-1] != 0xdeadbabe) {
    ERROR_PRINTF("%s: image format error: missing stack sanity check marker (%p)\n",
    Me, ((int *) bootRecord->spRegister)[-1]);
    return 1;
  }

  /* write freespace information into boot record */
  bootRecord->initialHeapSize  = initialHeapSize;
  bootRecord->maximumHeapSize  = maximumHeapSize;
  bootRecord->bootImageDataStart   = (Address) bootDataRegion;
  bootRecord->bootImageDataEnd     = (Address) bootDataRegion + roundedDataRegionSize;
  bootRecord->bootImageCodeStart   = (Address) bootCodeRegion;
  bootRecord->bootImageCodeEnd     = (Address) bootCodeRegion + roundedCodeRegionSize;
  bootRecord->bootImageRMapStart   = (Address) bootRMapRegion;
  bootRecord->bootImageRMapEnd     = (Address) bootRMapRegion + roundedRMapRegionSize;
  bootRecord->verboseBoot      = verboseBoot;
  bootRecord->bytesInPage = pageSize;

  /* write sys.C linkage information into boot record */
  setLinkage(bootRecord);

  /* Initialize system call routines and side data structures */
  sysInitialize();

  if (verbose) {
    TRACE_PRINTF("%s: boot record contents:\n", Me);
    TRACE_PRINTF("   bootImageDataStart:   %p\n", bootRecord->bootImageDataStart);
    TRACE_PRINTF("   bootImageDataEnd:     %p\n", bootRecord->bootImageDataEnd);
    TRACE_PRINTF("   bootImageCodeStart:   %p\n", bootRecord->bootImageCodeStart);
    TRACE_PRINTF("   bootImageCodeEnd:     %p\n", bootRecord->bootImageCodeEnd);
    TRACE_PRINTF("   bootImageRMapStart:   %p\n", bootRecord->bootImageRMapStart);
    TRACE_PRINTF("   bootImageRMapEnd:     %p\n", bootRecord->bootImageRMapEnd);
    TRACE_PRINTF("   initialHeapSize:      %p\n", bootRecord->initialHeapSize);
    TRACE_PRINTF("   maximumHeapSize:      %p\n", bootRecord->maximumHeapSize);
    TRACE_PRINTF("   spRegister:           %p\n", bootRecord->spRegister);
    TRACE_PRINTF("   ipRegister:           %p\n", bootRecord->ipRegister);
    TRACE_PRINTF("   tocRegister:          %p\n", bootRecord->tocRegister);
    TRACE_PRINTF("   sysConsoleWriteCharIP:%p\n", bootRecord->sysConsoleWriteCharIP);
    TRACE_PRINTF("   ...etc...                   \n");
  }

  /* force any machine code within image that's still in dcache to be
   * written out to main memory so that it will be seen by icache when
   * instructions are fetched back
   */
  sysSyncCache(bootCodeRegion, roundedCodeRegionSize);

  sysStartMainThread(vmInSeparateThread, bootRecord->ipRegister, bootRecord->spRegister,
                     *(Address *) (bootRecord->tocRegister + bootRecord->bootThreadOffset),
                     bootRecord->tocRegister, &bootRecord->bootCompleted);
}

/**
 * Parse command line arguments to find those arguments that
 *   1) affect the starting of the VM,
 *   2) can be handled without starting the VM, or
 *   3) contain quotes
 * then call createVM().
 */
int main(int argc, const char **argv)
{
  int j;
  SysErrorFile = stderr;
  SysTraceFile = stdout;
  setbuf (SysErrorFile, NULL);
  setbuf (SysTraceFile, NULL);
  setvbuf(stdout,NULL,_IONBF,0);
  setvbuf(stderr,NULL,_IONBF,0);
  Me            = strrchr(*argv, '/') + 1;
  ++argv, --argc;
  initialHeapSize = heap_default_initial_size;
  maximumHeapSize = heap_default_maximum_size;

  // Determine page size information early because
  // it's needed to parse command line options
  pageSize = (Extent) determinePageSize();
  if (pageSize <= 0) {
    ERROR_PRINTF("RunBootImage.main(): invalid page size %u", pageSize);
    exit(EXIT_STATUS_IMPOSSIBLE_LIBRARY_FUNCTION_ERROR);
  }

  /*
   * Debugging: print out command line arguments.
   */
  if (TRACE) {
    TRACE_PRINTF("RunBootImage.main(): process %d command line arguments\n",argc);
    for (j = 0; j < argc; j++) {
      TRACE_PRINTF("\targv[%d] is \"%s\"\n",j, argv[j]);
     }
  }

  // call processCommandLineArguments().
  int fastBreak = 0;
  // Sets JavaArgc
  JavaArgs = processCommandLineArguments(argv, argc, &fastBreak);
  if (fastBreak) {
    sysExit(EXIT_STATUS_BOGUS_COMMAND_LINE_ARG);
  }

  if (TRACE) {
    TRACE_PRINTF("RunBootImage.main(): after processCommandLineArguments: %d command line arguments\n", JavaArgc);
    for (j = 0; j < JavaArgc; j++) {
      TRACE_PRINTF("\tJavaArgs[%d] is \"%s\"\n", j, JavaArgs[j]);
    }
  }

  /* Verify heap sizes for sanity. */
  if (initialHeapSize == heap_default_initial_size &&
      maximumHeapSize != heap_default_maximum_size &&
      initialHeapSize > maximumHeapSize) {
    initialHeapSize = maximumHeapSize;
  }

  if (maximumHeapSize == heap_default_maximum_size &&
      initialHeapSize != heap_default_initial_size &&
      initialHeapSize > maximumHeapSize) {
    maximumHeapSize = initialHeapSize;
  }

  if (maximumHeapSize < initialHeapSize) {
    CONSOLE_PRINTF( "%s: maximum heap size %lu MiB is less than initial heap size %lu MiB\n",
                    Me, (unsigned long) maximumHeapSize/(1024*1024),
                    (unsigned long) initialHeapSize/(1024*1024));
    return EXIT_STATUS_BOGUS_COMMAND_LINE_ARG;
  }

  TRACE_PRINTF("\nRunBootImage.main(): VM variable settings\n");
  TRACE_PRINTF("initialHeapSize %lu\nmaxHeapSize %lu\n"
           "bootCodeFileName \"%s\"\nbootDataFileName \"%s\"\n"
           "bootRmapFileName \"%s\"\n"
           "verbose %d\n",
           (unsigned long) initialHeapSize,
           (unsigned long) maximumHeapSize,
           bootCodeFilename, bootDataFilename, bootRMapFilename,
           verbose);

  if (!bootCodeFilename) {
    CONSOLE_PRINTF( "%s: please specify name of boot image code file using \"-X:ic=<filename>\"\n", Me);
    return EXIT_STATUS_BOGUS_COMMAND_LINE_ARG;
  }

  if (!bootDataFilename) {
    CONSOLE_PRINTF( "%s: please specify name of boot image data file using \"-X:id=<filename>\"\n", Me);
    return EXIT_STATUS_BOGUS_COMMAND_LINE_ARG;
  }

  if (!bootRMapFilename) {
    CONSOLE_PRINTF( "%s: please specify name of boot image ref map file using \"-X:ir=<filename>\"\n", Me);
    return EXIT_STATUS_BOGUS_COMMAND_LINE_ARG;
  }

  int ret = createVM(0);
  if (ret == 1) {
    ERROR_PRINTF("%s: Could not create the virtual machine; goodbye\n", Me);
    exit(EXIT_STATUS_MISC_TROUBLE);
  }
  return 0;
}

/**
 * Determines the page size.
 * Taken:     (no arguments)
 * Returned:  page size in bytes (Java int)
 */
Extent determinePageSize()
{
  TRACE_PRINTF("%s: determinePageSize\n", Me);
  Extent pageSize = -1;
#ifdef _SC_PAGESIZE
  pageSize = (Extent) sysconf(_SC_PAGESIZE);
#elif _SC_PAGE_SIZE
  pageSize = (Extent) sysconf(_SC_PAGE_SIZE);
#else
  pageSize = (Extent) getpagesize();
#endif
  return pageSize;
}
