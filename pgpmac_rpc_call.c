#include <stdio.h>
#include <rpc/rpc.h>	
#include "pgpmac_rpc.h"

#undef DEBUG

md2_rpc_res_t *md2_rpc_call_1(md2_rpc_arg_t *argp, CLIENT *clnt);

static CLIENT *cl_p=NULL;
static CLIENT cl_s;

int md2pmac_rpc_init(char *server)
{
    if (cl_p!=NULL) {
	md2pmac_rpc_exit ();
    }

    cl_p = clnt_create (server, MD2_SERVER_PROCEDURE, MD2_SERVER_VERS, "tcp");
    if (cl_p == NULL) {
	clnt_pcreateerror(server);
	// printf("Could not create client to server '%s' \n",server);
	return(-1);
    }
    // printf("md2pmac_rpc_init: client created %u\n",cl_p);
    cl_p->cl_auth = authunix_create_default();
    memmove(&cl_s ,cl_p, sizeof(CLIENT));
    return(0);
}    

int md2pmac_rpc_exit()
{
    int i;
    md2_rpc_res_t *result;
    md2_rpc_arg_t arguments;
    
    if (cl_p==NULL) {
      // printf("md2pmac_rpc_exit: no client opened\n");
	return(-1);
    }

    arguments.functcode=RPC_EXIT;
    arguments.line[0]=0;

    result = md2_rpc_call_1(&arguments,&cl_s);
    if (result == NULL) {
	clnt_perror(&cl_s, "md2pmac_rpc_exit: ");
	cl_p=NULL;
	return(-1);
    }
    cl_p=NULL;
    if (result->errorno != 0) {
      // printf("md2pmac_rpc_exit: server couldn't handle the request\n");
	return(-1);
    }
    return(0);
}
    
int md2pmac_rpc_open()
{
    int i;
    md2_rpc_res_t *result;
    md2_rpc_arg_t arguments;
    
    if (cl_p==NULL) {
      // printf("md2pmac_rpc_open: no client opened\n");
	return(-1);
    }

    arguments.functcode = RPC_OPEN;

    result = md2_rpc_call_1(&arguments,&cl_s);
    if (result == NULL) {
	clnt_perror(&cl_s, "md2pmac_rpc_open");
	return(-1);
    }
    if (result->errorno != 0) {
        if (result->errorno == -11) return (-11);
	if (result->errorno==RPC_ACCESS_REFUSED) {
	  // printf("md2pmac_rpc_open: RPC access refused\n");
	}
	else
	  // printf("md2pmac_rpc_open: server couldn't handle the request\n");
	return(-1);
    }
    return(0);
}
 
int md2pmac_rpc_puts(char *line)  
{
    int i;
    md2_rpc_res_t *result;
    md2_rpc_arg_t arguments;
    
    if (cl_p==NULL) {
      // printf("md2pmac_rpc_puts: no client opened\n");
	return(-1);
    }

    arguments.functcode=RPC_PUTS;
    if (strlen(line)>MAX_LEN) {
      // printf("md2pmac_rpc_puts: string too long (%s:%d) !",line,strlen(line));
	return(-1);
    }
    sprintf(arguments.line,"%s",line);
    
    result = md2_rpc_call_1(&arguments,&cl_s);
    if (result == NULL) {
	clnt_perror(&cl_s, "md2pmac_rpc_printf");
	return(-1);
    }
    if (result->errorno != 0) {
      // printf("md2pmac_rpc_puts: server couldn't handle the request\n");
	return(-1);
    }
    return(0);
}

int md2pmac_rpc_gets(char *cmd, char *line)  
{
    int i;
    char *result_line, *tmp;
    md2_rpc_res_t *result;
    md2_rpc_arg_t arguments;
    
    if (cl_p==NULL) {
	// printf("md2pmac_rpc_puts: no client opened\n");
	return(-1);
    }

    arguments.functcode=RPC_GETS;
    
    if (strlen(cmd)>MAX_LEN) {
      // printf("md2pmac_rpc_puts: string too long (%s:%d) !",cmd,strlen(cmd));
	return(-1);
    }
    /* remove blanks */
    for (tmp=cmd+strlen(cmd)-1; tmp>=cmd && (*tmp==' ' || *tmp=='\n'); tmp--)
      *tmp = 0;
    for (tmp=cmd; *tmp==' '; tmp++);
    sprintf(arguments.line,"%s",tmp);

    result = md2_rpc_call_1(&arguments,&cl_s);
    if (result == NULL) {
	clnt_perror(&cl_s, "rpc_gets");
	return(-1);
    }
    if (result->errorno != 0) {
      // printf("rpc_puts: server couldn't handle the request\n");
	return(-1);
    }
    result_line = result->md2_rpc_res_t_u.result.line;
    if (strlen(result_line)>MAX_LEN) {
      /* printf("rpc_gets: string too long (%s:%d) !",
	 result_line,strlen(result_line));*/
	return(-1);
    }
    sprintf(line,"%s",result_line);
    
    return(0);
}




