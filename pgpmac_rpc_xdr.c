#include "pgpmac.h"
#include "pgpmac_rpc.h"

#define LF_CHAR 10


bool_t xdr_md2_rpc_arg_t (XDR *xdrs, md2_rpc_arg_t *objp) {

	int i;

	if (!xdr_int (xdrs, &objp->functcode)) return FALSE;
	 
        if (!xdr_int (xdrs, &objp->status)) return FALSE;
	 
        if (!xdr_vector (xdrs, (char *)objp->line, MAX_LEN, sizeof (char), (xdrproc_t) xdr_char)) return FALSE;
	
        return TRUE;
}


bool_t xdr_md2_rpc_res_t (XDR *xdrs, md2_rpc_res_t *objp) {

	if (!xdr_int (xdrs, &objp->errorno)) return FALSE;
	
        switch (objp->errorno) {

	case 0:
	   if (!xdr_md2_rpc_arg_t (xdrs, &objp->md2_rpc_res_t_u.result)) return FALSE;
	   break;
	
        default:
	   break;
	}
	
	return TRUE;
}
