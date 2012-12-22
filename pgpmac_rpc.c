#include "pgpmac.h"
#include "pgpmac_rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <rpc/pmap_clnt.h>
#include <string.h>
#include <memory.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef SIG_PF
#define SIG_PF void(*)(int)
#endif
#define RPC_EXIT 0
#define RPC_OPEN 1
#define RPC_PUTS 2
#define RPC_GETS 3
#define RPC_GETP 4
#define RPC_NOERROR 0
#define RPC_ERROR 1
#define RPC_UNKNOWN_FUNCTION -2
#define RPC_ACCESS_REFUSED -3
#define MAX_LEN 128
#define LF_CHAR 10

int md2pmac_rpc_exit();
int md2pmac_rpc_init();
int md2pmac_rpc_open();
int md2pmac_rpc_puts(char *line);
int md2pmac_rpc_gets(char *cmd, char *line);

static void
testprocedure_1(struct svc_req *rqstp, register SVCXPRT *transp)
{
	union {
		md2_rpc_arg_t md2_rpc_call_1_arg;
	} argument;
	char *result;
	xdrproc_t _xdr_argument, _xdr_result;
	char *(*local)(char *, struct svc_req *);

	switch (rqstp->rq_proc) {
	case NULLPROC:
		(void) svc_sendreply (transp, (xdrproc_t) xdr_void, (char *)NULL);
		return;

	case md2pmac_RPC_CALL:
		_xdr_argument = (xdrproc_t) xdr_md2_rpc_arg_t;
		_xdr_result = (xdrproc_t) xdr_md2_rpc_res_t;
		local = (char *(*)(char *, struct svc_req *)) md2_rpc_call_1_svc;
		break;

	default:
		svcerr_noproc (transp);
		return;
	}
	memset ((char *)&argument, 0, sizeof (argument));
	if (!svc_getargs (transp, (xdrproc_t) _xdr_argument, (caddr_t) &argument)) {
		svcerr_decode (transp);
		return;
	}
	result = (*local)((char *)&argument, rqstp);
	if (result != NULL && !svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) {
		svcerr_systemerr (transp);
	}
	if (!svc_freeargs (transp, (xdrproc_t) _xdr_argument, (caddr_t) &argument)) {
		fprintf (stderr, "%s", "unable to free arguments");
		exit (1);
	}
	return;
}

int
main (int argc, char **argv)
{
	register SVCXPRT *transp;

	pmap_unset (TESTPROCEDURE, TESTPROCEDUREVERS);

	transp = svcudp_create(RPC_ANYSOCK);
	if (transp == NULL) {
		fprintf (stderr, "%s", "cannot create udp service.");
		exit(1);
	}
	if (!svc_register(transp, TESTPROCEDURE, TESTPROCEDUREVERS, testprocedure_1, IPPROTO_UDP)) {
		fprintf (stderr, "%s", "unable to register (TESTPROCEDURE, TESTPROCEDUREVERS, udp).");
		exit(1);
	}

	transp = svctcp_create(RPC_ANYSOCK, 0, 0);
	if (transp == NULL) {
		fprintf (stderr, "%s", "cannot create tcp service.");
		exit(1);
	}
	if (!svc_register(transp, TESTPROCEDURE, TESTPROCEDUREVERS, testprocedure_1, IPPROTO_TCP)) {
		fprintf (stderr, "%s", "unable to register (TESTPROCEDURE, TESTPROCEDUREVERS, tcp).");
		exit(1);
	}

	svc_run ();
	fprintf (stderr, "%s", "svc_run returned");
	exit (1);
	/* NOTREACHED */
}
