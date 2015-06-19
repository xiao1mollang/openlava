/*
 * Copyright (C) 2014-2015 David Bigagli
 * Copyright (C) 2007 Platform Computing Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sbd.h"

extern void do_sbdDebug(XDR *xdrs, int chfd, struct LSFHeader *reqHdr);
static void sinit(void);
static void init_sstate(void);
static void processMsg(struct clientNode *);
static void clientIO(struct Masks *);
static void houseKeeping(void);
static int authCmdRequest(struct clientNode *,
                          XDR *,
			  struct LSFHeader *);
static int isLSFAdmin(struct lsfAuth *);
static int get_new_master(struct sockaddr_in *);

extern void do_modifyjob(XDR *, int, struct LSFHeader *);

time_t bootTime = 0;

char errbuf[MAXLINELEN];

char *lsbManager = NULL;
int debug = 0;
int lsb_CheckMode = 0;
int lsb_CheckError = 0;
uid_t batchId      = 0;
int  managerId    = 0;
char masterme = FALSE;
char master_unknown = TRUE;
char myStatus = 0;
char need_checkfinish = FALSE;
int  failcnt = 0;
ushort sbd_port;
ushort mbd_port;
int    sbdSleepTime  = DEF_SSLEEPTIME;
int    msleeptime  = DEF_MSLEEPTIME;
int    retryIntvl  = DEF_RETRY_INTVL;
int    preemPeriod = DEF_PREEM_PERIOD;
int    pgSuspIdleT = DEF_PG_SUSP_IT;
int    rusageUpdateRate = DEF_RUSAGE_UPDATE_RATE;
int    rusageUpdatePercent = DEF_RUSAGE_UPDATE_PERCENT;
char   *cpuset_mount = NULL;
char   *memory_mount = NULL;
bool_t cgroup_memory_mounted = false;
bool_t cgroup_cpuset_mounted = false;
struct infoCPUs *array_cpus;

int    jobTerminateInterval = DEF_JTERMINATE_INTERVAL;
char   psbdJobSpoolDir[MAXPATHLEN];

time_t now;
int connTimeout;
int readTimeout;

int    batchSock;

int    mbdPid = 0;
short  mbdExitVal = MASTER_NULL;
int    mbdExitCnt = 0;
int    jobcnt = 0;
int    maxJobs = 0;
int    urgentJob = 0;
int    uJobLimit = 0;
float  myFactor = 0.0;

int statusChan = -1;

windows_t  *host_week[8];
time_t     host_windEdge = 0;
char       host_active = TRUE;

char delay_check = FALSE;
char   *env_dir = NULL;

char   *masterHost;
char   *clusterName;
struct jobCard *jobQueHead;
struct lsInfo *allLsInfo;
struct clientNode *clientList;
struct bucket *jmQueue;
struct tclLsInfo *tclLsInfo;

#define CLEAN_TIME (12*60*60)

extern int initenv_(struct config_param *, char *);

#define CHECK_MBD_TIME 30
static char mbdStartedBySbd = FALSE;

int getpwnamRetry = 1;
int lsbMemEnforce = FALSE;
int lsbJobCpuLimit = -1;
int lsbJobMemLimit = -1;
int lsbStdoutDirect = FALSE;

int sbdLogMask;
int numCPUs;
struct infoCPUs *array_cpus ;

int
main(int argc, char **argv)
{
    int nready, i;
    sigset_t oldsigmask, newmask;
    struct timeval timeout;
    struct Masks sockmask, chanmask;
    int aopt;
    char *msg = NULL;

    saveDaemonDir_(argv[0]);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && argv[i+1] != NULL) {
            env_dir = argv[i+1];
            putEnv("LSF_ENVDIR",env_dir);
            break;
        }
    }

    if (env_dir == NULL) {
        if ((env_dir = getenv("LSF_ENVDIR")) == NULL )
            env_dir = LSETCDIR;
    }

    if (argc > 1) {
        if (!strcmp(argv[1],"-V")) {
            fputs(_LS_VERSION_, stderr);
            exit(0);
        }
    }

    if (initenv_(daemonParams, env_dir) < 0) {
        ls_openlog("sbatchd",
                   daemonParams[LSF_LOGDIR].paramValue, (debug > 1),
                   daemonParams[LSF_LOG_MASK].paramValue);
        ls_syslog(LOG_ERR, "%s: initenv_() failed: %M", __func__);
        die(SLAVE_FATAL);
    }

    if( (daemonParams[LSB_STDOUT_DIRECT].paramValue != NULL)
	&&(daemonParams[LSB_STDOUT_DIRECT].paramValue[0] == 'y'
           || daemonParams[LSB_STDOUT_DIRECT].paramValue[0] == 'Y' ) ) {
        lsbStdoutDirect = TRUE;
    }

    ls_syslog(LOG_WARNING, "\
%s: LSB_STDOUT_DIRECT configured as %s", __func__,
	      (daemonParams[LSB_STDOUT_DIRECT].paramValue)
	      ? daemonParams[LSB_STDOUT_DIRECT].paramValue :"NULL");

    opterr = 0;
    while ((aopt = getopt(argc, argv, "hV:d:123")) != EOF) {

        switch(aopt) {
            case '1':
            case '2':
            case '3':
                debug = aopt - '0';
            break;
            case 'd':
                env_dir = optarg;
                break;
            case 'h':
            default:
                fprintf(stderr,
                        "%s: sbatchd [-h] [-V] [-d env_dir] [-1 | -2 | -3]\n",
                        I18N_Usage);
            exit(1);
        }
    }

    if (!debug && isint_(daemonParams[LSB_DEBUG].paramValue)) {
	debug = atoi(daemonParams[LSB_DEBUG].paramValue);
	if (debug <= 0 || debug > 3)
	    debug = 1;
    }

    if (debug < 2) {
        for (i = sysconf(_SC_OPEN_MAX) ; i >= 3 ; i--)
            close(i);
    }

    daemon_doinit();

    if (debug == 0) {
        if (getuid() != 0) {
            fprintf(stderr, "%s: Real uid is %d, not root\n",
                    argv[0], (int)getuid());
            exit(1);
        }
        if (geteuid() != 0) {
            fprintf(stderr, "%s: Effective uid is %d, not root\n",
                    argv[0], (int)geteuid());
            exit(1);
        }
    }

    umask(022);

    if (debug < 2) {
	daemonize_();
    }

    getLogClass_(daemonParams[LSB_DEBUG_SBD].paramValue,
                 daemonParams[LSB_TIME_SBD].paramValue);

    if (debug > 1)
        ls_openlog("sbatchd", daemonParams[LSF_LOGDIR].paramValue, TRUE,
                   daemonParams[LSF_LOG_MASK].paramValue);
    else
        ls_openlog("sbatchd", daemonParams[LSF_LOGDIR].paramValue, FALSE,
                   daemonParams[LSF_LOG_MASK].paramValue);

    sbdLogMask = getLogMask(&msg, daemonParams[LSF_LOG_MASK].paramValue);

    if (isint_(daemonParams[LSB_SBD_CONNTIMEOUT].paramValue))
        connTimeout = atoi(daemonParams[LSB_SBD_CONNTIMEOUT].paramValue);
    else
        connTimeout = 6;

    if (isint_(daemonParams[LSB_SBD_READTIMEOUT].paramValue))
        readTimeout = atoi(daemonParams[LSB_SBD_READTIMEOUT].paramValue);
    else
        readTimeout = 20;

    if ((daemonParams[LSF_GETPWNAM_RETRY].paramValue != NULL)
        && (isint_(daemonParams[LSF_GETPWNAM_RETRY].paramValue)))
        getpwnamRetry = atoi(daemonParams[LSF_GETPWNAM_RETRY].paramValue);

    if (daemonParams[LSB_MEMLIMIT_ENFORCE].paramValue != NULL) {
	if (!strcasecmp(daemonParams[LSB_MEMLIMIT_ENFORCE].paramValue, "y")) {
            lsbMemEnforce = TRUE;
	}
    }

    lsbJobCpuLimit = -1;
    if (daemonParams[LSB_JOB_CPULIMIT].paramValue != NULL) {
	if (!strcasecmp(daemonParams[LSB_JOB_CPULIMIT].paramValue, "y")) {
	    lsbJobCpuLimit = 1;
	} else if (!strcasecmp(daemonParams[LSB_JOB_CPULIMIT].paramValue,
                               "n")) {
	    lsbJobCpuLimit = 0;
	} else {
	    ls_syslog(LOG_ERR, "\
%s: LSB_JOB_CPULIMIT <%s> in lsf.conf is invalid", __func__,
                      daemonParams[LSB_JOB_CPULIMIT].paramValue);
	}
    }

    lsbJobMemLimit = -1;
    if (daemonParams[LSB_JOB_MEMLIMIT].paramValue != NULL) {
	if (!strcasecmp(daemonParams[LSB_JOB_MEMLIMIT].paramValue, "y")) {
	    lsbJobMemLimit = 1;
	} else if (!strcasecmp(daemonParams[LSB_JOB_MEMLIMIT].paramValue,
                               "n")) {
	    lsbJobMemLimit = 0;
	} else {
	    ls_syslog(LOG_ERR, "\
%s: LSB_JOB_MEMLIMIT <%s> in lsf.conf is invalid.", __func__,
                      daemonParams[LSB_JOB_MEMLIMIT].paramValue);
	}
    }

    now = time(NULL);

    for (i = 0; i < 8; i++)
        host_week[i] = NULL;

    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    while ((allLsInfo = ls_info()) == NULL) {
        ls_syslog(LOG_ERR, "%s: ls_info() failed: %M; trying ...", __func__);
	millisleep_(6000);
    }

    for (i = allLsInfo->nModels; i < MAXMODELS; i++)
        allLsInfo->cpuFactor[i] = 1.0;

    initParse (allLsInfo);
    tclLsInfo = getTclLsInfo();
    initTcl(tclLsInfo);

    sigprocmask(SIG_SETMASK, NULL, &oldsigmask);

    sinit();
    ls_syslog(LOG_INFO, "%s: sbatchd (re-)started", __func__);

    getLSFAdmins_();

    for(;;) {
        int    s;
        struct sockaddr_in from;
        struct clientNode *client;

	sigemptyset(&newmask);
	sigaddset(&newmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &newmask, NULL);

        /* job_checking is exclusive
         */
        if (!debug)
            chdir(LSTMPDIR);

	if (!delay_check) {
	    TIMEIT(1, job_checking(), "job_checking");
            status_report();
	} else {
	    timeout.tv_sec = sbdSleepTime/10;
	}

	if (failcnt && failcnt < 5)
	    timeout.tv_sec = sbdSleepTime/(5-failcnt);

	sigprocmask(SIG_SETMASK, &oldsigmask, NULL);

        if (need_checkfinish) {
            need_checkfinish = FALSE;
	    TIMEIT(1, checkFinish(), "checkFinish");
        }

        FD_ZERO(&sockmask.rmask);
	houseKeeping();

        nready = chanSelect_(&sockmask, &chanmask, &timeout);
        now = time(NULL);
        if (nready < 0) {
	    if (errno == EINTR)
		delay_check = FALSE;
	    else
                ls_syslog(LOG_ERR, "%s: select() failed: %m", __func__);
            continue;
        }

        timeout.tv_sec = sbdSleepTime;
	sigemptyset(&newmask);
	sigaddset(&newmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &newmask, NULL);

        if (nready == 0) {
	    if (delay_check)
		delay_check = FALSE;
            continue;
        }

	if (statusChan >= 0
            && (FD_ISSET(statusChan, &chanmask.rmask)
                || FD_ISSET(statusChan, &chanmask.emask))) {

	    if (logclass & LC_COMM)
		ls_syslog(LOG_DEBUG, "\
%s: Exception on statusChan <%d>, rmask <%x>", __func__,
			  statusChan, chanmask.rmask);
	    chanClose_(statusChan);
	    statusChan = -1;
	}

        if (!FD_ISSET(batchSock, &chanmask.rmask)) {
	    ls_syslog(LOG_DEBUG,"main: connection already known");
            clientIO(&chanmask);
	    continue;
	}

        s = chanAccept_(batchSock, &from);
        if (s == -1) {
            ls_syslog(LOG_ERR, "%s: chanAccept_ failed: %M", __func__);
	    continue;
        }

        client = malloc(sizeof(struct clientNode));
        if (!client) {
            ls_syslog(LOG_ERR, "\
%s: malloc failed. Unable to accept connection", __func__);
                chanClose_(s);
            continue;
        }

        client->chanfd = s;

        client->from = from;
	client->jp = NULL;
	client->jobId = -1;

        inList( (struct listEntry *)clientList, (struct listEntry *) client);

        if (logclass & LC_COMM )
	    ls_syslog(LOG_DEBUG, "\
%s: Accepted connection from host <%s> on channel <%d>", __func__,
                      sockAdd2Str_(&from), client->chanfd);

	clientIO(&chanmask);
    }

    return 0;
}

static void
clientIO(struct Masks *chanmask)
{
    struct clientNode *cliPtr;
    struct clientNode *nextClient;

    if (logclass & LC_TRACE)
	ls_syslog(LOG_DEBUG, "%s: Entering...", __func__);

    for(cliPtr = clientList->forw; cliPtr != clientList; cliPtr = nextClient) {
        nextClient = cliPtr->forw;

        if (FD_ISSET(cliPtr->chanfd, &chanmask->emask)) {

            shutDownClient(cliPtr);
            continue;
        }

        if (FD_ISSET(cliPtr->chanfd, &chanmask->rmask)) {
	    processMsg(cliPtr);
        }
    }
}

static void
processMsg(struct clientNode *client)
{
    struct Buffer *buf;
    int s;
    sbdReqType sbdReqtype;
    struct LSFHeader reqHdr;
    XDR xdrs;
    int cc;

    s = chanSock_(client->chanfd);

    if (chanDequeue_(client->chanfd, &buf) < 0) {
        ls_syslog(LOG_ERR, "\
%s: chanDequeue_() failed; cherrno = %d", __func__, cherrno);
        shutDownClient(client);
        return;
    }

    xdrmem_create(&xdrs, buf->data, buf->len, XDR_DECODE);
    if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
        ls_syslog(LOG_ERR, "%s: Bad header received", __func__);
        shutDownClient(client);
	xdr_destroy(&xdrs);
        chanFreeBuf_(buf);
        return;
    }

    sbdReqtype = reqHdr.opCode;

    if (logclass & (LC_TRACE | LC_COMM))
	ls_syslog(LOG_DEBUG,"%s: received msg <%d>", __func__, sbdReqtype);

    /* -Wenum-compare warning issued by smart gcc 4.9.1
     * compiler when using sbdReqType != PREPARE_FOR_OP
     */
    if (reqHdr.opCode != PREPARE_FOR_OP)
        io_block_(s);

    if (sbdReqtype == MBD_NEW_JOB || sbdReqtype == MBD_SIG_JOB
        || sbdReqtype == MBD_SWIT_JOB || sbdReqtype == MBD_PROBE
        || sbdReqtype == MBD_REBOOT || sbdReqtype == MBD_SHUTDOWN
        || sbdReqtype == MBD_MODIFY_JOB) {

        if (get_new_master(&client->from) < 0) {
            errorBack(client->chanfd, LSBE_NOLSF_HOST, &client->from);
	    shutDownClient(client);
	    chanFreeBuf_(buf);
	    xdr_destroy(&xdrs);
	    return;
        }

    } else if (sbdReqtype == CMD_SBD_REBOOT
               || sbdReqtype == CMD_SBD_SHUTDOWN
               || sbdReqtype == CMD_SBD_DEBUG) {

	if ((cc = authCmdRequest(client, &xdrs, &reqHdr)) != LSBE_NO_ERROR) {
            ls_syslog(LOG_ERR, "\
%s: authCmdRequest from <%s> reqtype %d failed", __func__,
		      sockAdd2Str_(&client->from), sbdReqtype);
            errorBack(client->chanfd, cc, &client->from);
	    shutDownClient(client);
	    chanFreeBuf_(buf);
	    xdr_destroy(&xdrs);
	    return;
	}
    }


    switch (sbdReqtype) {

        case PREPARE_FOR_OP:
            if (do_readyOp(&xdrs, client->chanfd, &client->from, &reqHdr) < 0)
                shutDownClient(client);
            break;

        case MBD_NEW_JOB:
            TIMEIT(2, do_newjob (&xdrs, client->chanfd, &reqHdr), "do_newjob");
            delay_check = TRUE;
            break;

        case MBD_SIG_JOB:
            TIMEIT(2, do_sigjob (&xdrs, client->chanfd, &reqHdr), "do_sigjob");
            delay_check = TRUE;
            break;

        case MBD_SWIT_JOB:
            TIMEIT(2, do_switchjob (&xdrs, client->chanfd, &reqHdr), "do_switchjob");
            delay_check = TRUE;
            break;

        case MBD_MODIFY_JOB:
            TIMEIT(2, do_modifyjob (&xdrs, client->chanfd, &reqHdr), "do_modifyjob");
            delay_check = TRUE;
            break;
        case MBD_PROBE:
            TIMEIT(2, do_probe (&xdrs, client->chanfd, &reqHdr), "do_probe");
            delay_check = TRUE;
            break;

        case MBD_REBOOT:
        case CMD_SBD_REBOOT:
            TIMEIT(2, do_reboot (&xdrs, client->chanfd, &reqHdr), "do_reboot");
            break;

        case MBD_SHUTDOWN:
        case CMD_SBD_SHUTDOWN:
            TIMEIT(2, do_shutdown (&xdrs, client->chanfd, &reqHdr), "do_shutdown");
            break;

        case CMD_SBD_DEBUG:
            TIMEIT(2, do_sbdDebug(&xdrs, client->chanfd, &reqHdr), "do_sbdDebug");
            break;

        case SBD_JOB_SETUP:
            TIMEIT(2, do_jobSetup(&xdrs, client->chanfd, &reqHdr), "do_jobSetup");
            delay_check = TRUE;
            break;

        case SBD_SYSLOG:
            TIMEIT(4, do_jobSyslog(&xdrs, client->chanfd, &reqHdr), "do_jobSyslog");;
            delay_check = TRUE;
            break;
        default:
            ls_syslog(LOG_ERR, "\
%s: Unknown request type <%d> from %s", __func__, sbdReqtype,
                      sockAdd2Str_(&client->from));
            break;
    }

    xdr_destroy(&xdrs);
    chanFreeBuf_(buf);
    if (reqHdr.opCode != PREPARE_FOR_OP)
        shutDownClient(client);

}



void
shutDownClient(struct clientNode *client)
{
    chanClose_(client->chanfd);
    offList((struct listEntry *)client);

    if (client->jp) {
	client->jp->client = NULL;

	client->jp->regOpFlag &= REG_RUSAGE;
    }
    FREEUP(client);
}

void
start_master(void)
{
    char *margv[6];
    int i;
    int newMbdPid;
    char *myhostnm;
    static time_t lastTime;

    ls_syslog(LOG_DEBUG, "%s: Entering this routine...", __func__);

    if (mbdStartedBySbd) {
	switch (mbdExitVal) {
            case MASTER_RECONFIG:
                break;
            case MASTER_RESIGN:
                if (now - lastTime < mbdExitCnt * 60)
                    return;
                break;
            case MASTER_FATAL:
            case MASTER_MEM:
            case MASTER_CONF:
                if (now - lastTime < mbdExitCnt * 30)
                    return;
                break;
            default:
                if (now - lastTime < mbdExitCnt * CHECK_MBD_TIME) {
                    now = time(0);
                    return;
                }
                break;
	}
    }

    lastTime = now;
    margv[0]= getDaemonPath_("/mbatchd", daemonParams[LSF_SERVERDIR].paramValue);

    i = 1;
    if (debug) {
	margv[i] = my_malloc(MAXFILENAMELEN, __func__);
	sprintf(margv[i], "-%d", debug);
	i++;
    }
    if (env_dir != NULL) {
	margv[i] = "-d";
	i++;
	margv[i] = env_dir;
	i++;
    }
    margv[i] = NULL;

    newMbdPid = fork();
    if (newMbdPid < 0) {
        ls_syslog(LOG_ERR, "\
%s: failed to fork() new master batch daemon %m", __func__);
        return;
    }

    if (newMbdPid == 0) {
	sigset_t newmask;

	sigemptyset(&newmask);
	sigprocmask(SIG_SETMASK, &newmask, NULL);

        closeBatchSocket();

	execve(margv[0], margv, environ);
	ls_syslog(LOG_ERR, "\
%s: execve() failed %m", __func__);
	lsb_mperr("Cannot execute mbatchd");
	exit(-1);
    }

    if (debug)
        free(margv[1]);

    if ((myhostnm = ls_getmyhostname()) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: ls_getmyhostname failed", __func__);
        die(SLAVE_FATAL);
    }

    if (newMbdPid > 0) {
        mbdPid = newMbdPid;
	mbdStartedBySbd = TRUE;
	ls_syslog(LOG_NOTICE, "\
%s: Master [%d] started by sbatchd on host %s", __func__,
                  mbdPid, myhostnm);
    }

}

static int
get_new_master(struct sockaddr_in *from)
{
    static char mHost[MAXHOSTNAMELEN];
    struct hostent *hp;

    hp = Gethostbyaddr_(&from->sin_addr.s_addr,
                        sizeof(in_addr_t),
                        AF_INET);
    if (hp == NULL) {
        ls_syslog(LOG_ERR, "\
%s: gethostbyaddr() %s failed", __func__, sockAdd2Str_(from));
        return -1;
    }

    if (hostOk(hp->h_name, 0) < 0) {
        ls_syslog(LOG_ERR, "\
%s: Request from non-LSF host %s/%s",
                  __func__, hp->h_name, sockAdd2Str_(from));
        return -1;
    }

    strcpy(mHost, hp->h_name);
    masterHost = mHost;
    master_unknown = FALSE;

    return 0;
}

static void
sinit(void)
{
    struct hostInfo *myinfo;
    char *myhostname;

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "sbatchd/%s: Entering this routine...", __func__);

    if (getBootTime(&bootTime) == -1) {
        ls_syslog(LOG_ERR, "\
%s: getBootTime() failed; assuming host was not rebooted while sbatchd was down",
                  __func__);
	bootTime = 0;
	die(SLAVE_FATAL);
    }

    Signal_(SIGALRM, SIG_IGN);
    Signal_(SIGHUP,  SIG_IGN);
    Signal_(SIGTERM, (SIGFUNCTYPE) die);
    Signal_(SIGINT,  (SIGFUNCTYPE) die);
    Signal_(SIGCHLD, (SIGFUNCTYPE) child_handler);
    Signal_(SIGPIPE, SIG_IGN);

    if (!debug) {
	Signal_(SIGTTOU, SIG_IGN);
	Signal_(SIGTTIN, SIG_IGN);
	Signal_(SIGTSTP, SIG_IGN);
    }

    jobQueHead = (struct jobCard *)mkListHeader() ;
    clientList = (struct clientNode *)mkListHeader() ;
    if (!jobQueHead || !clientList ) {
        ls_syslog(LOG_ERR, "%s: mkListHeader() failed", __func__);
        die(SLAVE_FATAL);
    }

    if ((clusterName = ls_getclustername()) == NULL) {
        ls_syslog(LOG_ERR, "%s: ls_getclustername() failed: %M", __func__);
	while ((clusterName = ls_getclustername()) == NULL)
	    millisleep_(sbdSleepTime * 1000);
    }

    if ((masterHost = ls_getmastername()) == NULL) {
        ls_syslog(LOG_ERR, "%s: ls_getmastername() failed: %M", __func__);
	while ((masterHost = ls_getmastername()) == NULL)
	    millisleep_(sbdSleepTime * 1000);
    }

    ls_syslog(LOG_INFO, "\
%s: Cluster %s, master %s", __func__, clusterName, masterHost);

    master_unknown = FALSE;
    batchId = getuid();
    myhostname = ls_getmyhostname();
    if (myhostname == NULL) {
        ls_syslog(LOG_ERR, "%s: ls_getmyhostname() failed: %M", __func__);
	die(SLAVE_FATAL);
    }

    while ((myinfo = ls_gethostinfo(NULL,
                                    NULL,
                                    &myhostname,
                                    1, 0)) == NULL) {
	ls_syslog(LOG_ERR, "%s: ls_gethostinfo() failed: %M", __func__);
	millisleep_(sbdSleepTime * 1000);
    }

    myFactor = myinfo->cpuFactor;

    if (get_ports() < 0)
        die(SLAVE_FATAL);

    init_sstate();

    batchSock = init_ServSock(sbd_port);
    if (batchSock < 0) {
        lsb_mperr("sbatchd: Cannot init servsocket");
        die(SLAVE_FATAL);
    }

    if (! debug) {
	nice(NICE_LEAST);
	nice(NICE_MIDDLE);
	nice(0);
    }

    if (!debug) {
	if (chdir(LSTMPDIR) < 0) {
            ls_syslog(LOG_ERR, "%s: chdir(%s) failed: %m", __func__, "/tmp");
	    lsb_mperr1("sbatchd: chdir %s failed\n", "/tmp");
	    die(SLAVE_FATAL);
	}
    }

    if (chanInit_() < 0) {
        ls_syslog(LOG_ERR, "%s: chanInit_ Failed to init channel", __func__);
        die(SLAVE_FATAL);
    }
}

static void
init_sstate(void)
{
    struct sbdPackage sbdPackage;
    int i;

    getJobsState(&sbdPackage);
    getManagerId(&sbdPackage);
    mbdPid = sbdPackage.mbdPid;

    sbdSleepTime = sbdPackage.sbdSleepTime;
    retryIntvl = sbdPackage.retryIntvl;
    preemPeriod = sbdPackage.preemPeriod;
    pgSuspIdleT = sbdPackage.pgSuspIdleT;
    maxJobs = sbdPackage.maxJobs;
    uJobLimit = sbdPackage.uJobLimit;
    rusageUpdateRate = sbdPackage.rusageUpdateRate;
    rusageUpdatePercent = sbdPackage.rusageUpdatePercent;
    jobTerminateInterval = sbdPackage.jobTerminateInterval;

    /* Hard code for now
     */
    cpuset_mount = strdup("/cgroup/cpuset");
    memory_mount = strdup("/cgroup/memory");

    for (i = 0; i < sbdPackage.nAdmins; i++)
	FREEUP(sbdPackage.admins[i]);
    FREEUP(sbdPackage.admins);
}


static void
houseKeeping(void)
{
    static time_t lastTime;
    static time_t lastCheckMbdTime;
    static time_t lastStartMbdTime;
    char *updMasterHost;
    char *myhostnm;

    if (now - lastTime >= sbdSleepTime / 2) {
	if (ls_servavail(2, 1) < 0)
            ls_syslog(LOG_ERR, "%s: ls_servavail : %M", __func__);
	lastTime = now;
    }

    /* Nice reverse logic
     */
    if (! (now - lastCheckMbdTime >= CHECK_MBD_TIME))
        return;

    lastCheckMbdTime = now;
    updMasterHost = ls_getmastername();

    if (updMasterHost == NULL) {
        master_unknown = TRUE;
        myStatus |= NO_LIM;
        lserrno = LSE_NO_ERR;
        return ;
    }

    masterHost = updMasterHost;
    myStatus = 0;
    master_unknown = FALSE;
    if ((myhostnm = ls_getmyhostname()) == NULL) {
        ls_syslog(LOG_ERR, "%s: ls_getmyhostname() failed: %M", __func__);
        die(SLAVE_FATAL);
    }

    if (equalHost_(masterHost, myhostnm)) {

        if (mbdPid != 0) {
            if (kill(mbdPid, 0) != 0) {
                if ((now - lastStartMbdTime >=
                     3 * CHECK_MBD_TIME) || !mbdStartedBySbd) {
                    start_master();
                    lastStartMbdTime = now;
                }
            }
        }
    } /* if (equalHost()) */

}

static int
authCmdRequest(struct clientNode *client,
               XDR *xdrs,
               struct LSFHeader *reqHdr)
{
    int s;
    struct lsfAuth auth;
    struct hostent *hp;

    s = chanSock_(client->chanfd);
    hp = Gethostbyaddr_(&client->from.sin_addr.s_addr,
                        sizeof(in_addr_t),
                        AF_INET);
    if (hp == NULL) {
	ls_syslog(LOG_ERR, "\
%s: gethostbyaddr() failed for %s", __func__,
                  sockAdd2Str_(&client->from));
        return LSBE_NOLSF_HOST;
    }

    if (!xdr_lsfAuth(xdrs, &auth, reqHdr)) {
	ls_syslog(LOG_ERR,"%s: xdr_lsfAuth() failed", __func__);
	return LSBE_XDR;
    }

    if (hostOk(hp->h_name, 0) < 0) {
	ls_syslog(LOG_ERR, "\
%s: host %s is not a valid openlava host", __func__,
                  hp->h_name,
                  sockAdd2Str_(&client->from));
	return LSBE_NOLSF_HOST;
    }

    putEauthClientEnvVar("user");
    putEauthServerEnvVar("sbatchd");

    if (!userok(s, &client->from, hp->h_name, NULL, &auth, debug)) {
	ls_syslog(LOG_ERR, "\
%s: userok() has faled from host %s", __func__, hp->h_name);
	return LSBE_PERMISSION;
    }

    if (!isLSFAdmin(&auth)) {

	return LSBE_PERMISSION;
    }

    return LSBE_NO_ERROR;
}


static int
isLSFAdmin(struct lsfAuth *auth)
{
    if (auth->uid == 0)
        return true;

    getLSFAdmins_();

    if (isLSFAdmin_(auth->lsfUserName)) {
        return true;
    }

    return false;

}

/* cpus_cmp()
 *
 * qsort() helper
 */
int
cmp_cpus(const void *c1, const void *c2)
{
    const struct infoCPUs *cpu1;
    const struct infoCPUs *cpu2;

    cpu1 = c1;
    cpu2 = c2;

    if (cpu1->numTasks > cpu2->numTasks)
        return 1;
    if (cpu1->numTasks < cpu2->numTasks)
        return -1;
    return 0;
}
