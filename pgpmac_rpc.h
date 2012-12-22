
#include <rpc/rpc.h>


#ifdef __cplusplus
extern "C" {
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
#define MAX_LEN 2048
#define LF_CHAR 10

int md2pmac_rpc_exit();
int md2pmac_rpc_init();
int md2pmac_rpc_open();
int md2pmac_rpc_puts(char *line);
int md2pmac_rpc_gets(char *cmd, char *line);

struct md2_rpc_arg_t {
	int functcode;
	int status;
	char line[MAX_LEN];
};

typedef struct md2_rpc_arg_t md2_rpc_arg_t;

struct md2_rpc_res_t {
	int errorno;
	union {
		md2_rpc_arg_t result;
	} md2_rpc_res_t_u;
};

typedef struct md2_rpc_res_t md2_rpc_res_t;

#define MD2_SERVER_PROCEDURE ((u_long) 32799)
#define MD2_SERVER_VERS ((u_long) 1)
#define MD2_RPC_CALL 1

extern md2_rpc_res_t * md2_rpc_call_1 (md2_rpc_arg_t *, CLIENT *);
extern md2_rpc_res_t * md2_rpc_call_1_svc (md2_rpc_arg_t *, struct svc_req *);

/* the xdr functions */
extern  bool_t xdr_md2_rpc_arg_t (XDR *, md2_rpc_arg_t*);
extern  bool_t xdr_md2_rpc_res_t (XDR *, md2_rpc_res_t*);

double phiRef;



