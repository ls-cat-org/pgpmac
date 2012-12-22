#include <string.h>
#include <memory.h>
#include <stdio.h>
#include <rpc/rpc.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "pgpmac.h"
#include "pgpmac_rpc.h"


static void md2_server_procedure_1(struct svc_req *rqstp, register SVCXPRT *transp) {

	union {
		md2_rpc_arg_t md2_rpc_call_1_arg;
	} argument;

	char *result;

	char mess[256];

	xdrproc_t xdr_argument;
	xdrproc_t xdr_result;

	char *(*local)(char *, struct svc_req *);


	switch (rqstp->rq_proc) {

	case NULLPROC:
	   (void)svc_sendreply(transp, (xdrproc_t) xdr_void, (char *)NULL);
	   return;

	case MD2_RPC_CALL:
	   xdr_argument = (xdrproc_t) xdr_md2_rpc_arg_t;
	   xdr_result = (xdrproc_t) xdr_md2_rpc_res_t;
	   local = (char *(*)(char *, struct svc_req *)) md2_rpc_call_1_svc;
           break;

	default:
	   svcerr_noproc(transp);
	   return;
	}

	memset((char *) &argument, 0x0,sizeof(argument));
	if (!svc_getargs(transp, (xdrproc_t) xdr_argument, (caddr_t) &argument)) {
	   svcerr_decode(transp);
	   return;
	}

	result = (*local)((char *)&argument, rqstp);
	if (result != NULL && !svc_sendreply(transp, (xdrproc_t) xdr_result, result)) {
           lslogging_log_message("md2_server_procedure_1: svcerr_systemerr"); 
	   svcerr_systemerr(transp);
	}

	if (!svc_freeargs(transp, (xdrproc_t) xdr_argument, (caddr_t) &argument)) {
	   lslogging_log_message("md2_server_procedure_1: unable to free arguments");
	   exit(1);
	}
	return;
}



void *md2_server_proc( void * dummy ) {
    
        register SVCXPRT *transp;

 	char mesg[256];

	(void)pmap_unset(MD2_SERVER_PROCEDURE, MD2_SERVER_VERS);
      
	transp = svcudp_create (RPC_ANYSOCK);

	if (transp == NULL) {
	   lslogging_log_message("md2_server_proc: cannot create udp service.");
	}
	else {
	   lslogging_log_message("md2_server_proc: udp service successfully created");
	}

	if (!svc_register(transp, MD2_SERVER_PROCEDURE, MD2_SERVER_VERS, md2_server_procedure_1, IPPROTO_UDP)) {
	   lslogging_log_message("md2_server_proc: unable to register (MD2_SERVER_PROCEDURE, MD2_SERVER_VERS)");
	}
	else {
	   lslogging_log_message("md2-serverProc: udp server procedure successfully registered");
	}

	transp = svctcp_create (RPC_ANYSOCK,0,0);

	if (transp == NULL) {
	   lslogging_log_message("md2_server_proc: cannot create tcp service.");
	}
	else {
	   lslogging_log_message("md2_server_proc: udp service successfully created");
	}

	if (!svc_register(transp, MD2_SERVER_PROCEDURE, MD2_SERVER_VERS, md2_server_procedure_1, IPPROTO_TCP)) {
	   lslogging_log_message("md2_server_proc: unable to register (MD2_SERVER_PROCEDURE, MD2_SERVER_VERS, tcp).");
	}
	else {
	   lslogging_log_message("md2-serverProc: tcp server procedure successfully registered");
	}

	lslogging_log_message("md2_server_proc: starting rpc service process");
	svc_run();
	lslogging_log_message("md2_server_porc: svc_run_ex returned");
	//return (0) ;
}


