#include <stdio.h>
#include <sys/time.h>
#include <fcntl.h>
#include <rpc/rpc.h>
#include "pgpmac.h"	
#include "pgpmac_rpc.h"

#define MAX_DEVICE 10


//dummy write message to screen
void WRT_SCROLL_MESS(char * mess_str, int mess_len) {
}

//dummy redist transaction
int transact_redis_socket(char * outstr) {
}


/*
 *  Main rpc function
 */

md2_rpc_res_t *md2_rpc_call_1_svc (md2_rpc_arg_t *md2_rpc_arg, struct svc_req *unused) {
    
    static md2_rpc_res_t tmp_res; 
    static int robot_config,last_phase_comit;

    int minor,ierr,length,tries;
    int ilen1,ilen2,icnt,lng_stat;
    int DEBUG_ON,REDIS_SYNC;
    long lng1,lng2,lng3;
    char cmd[5], buffer[2048], command[256], phase_cmd[50];
    char sa1[50],sa2[50],sa3[50],sa4[50],sa5[50],sa6[50],sa7[50],sa8[50],sa9[50],sa10[50],sa11[50],sa12[50],sa13[50],sa14[50],sa15[50],sa16[50];
    char sv[16][10];
    char scmd[25],arg1[50],arg2[50],arg3[50];
    char pmotornames[8][5];
    char pmotorpos[8][10];
    char host_name [19];
    char lpass[80],tpass[80];
    int  redis_ret;
    char tmpstr[100];
    char redis_out[100];
    char mov_cmd[256];

    //comwrapper client defs
    char faildesc[128],status[128],prev_position[128];
    int res,res1,res2,res3,res4,res5,res6,res7;
    int goup;
    enum MD_STATUS_CODE * state;
    enum MDPOSITION * phase;

    double dbl_attr_phi, dbl_attr_omega, dbl_attr_kappa, dbl_attr_kappaphi, dbl_attr_fluo; 
    double dbl_attr_zoom, dbl_attr_blightud, dbl_attr_blight, dbl_attr_flight, dbl_attr_flightoo;
    double dbl_attr_alignx, dbl_attr_aligny, dbl_attr_alignz, dbl_attr_cenx, dbl_attr_ceny;
    double dbl_attr_apery, dbl_attr_aperz, dbl_attr_capy, dbl_attr_capz, dbl_attr_scint, dbl_attr_fscint;
    double dbl_attr_shutter,dbl_stat;

    double cenx_mov,ceny_mov;
    double pi=3.14159266564;;

    int lng_attr_state;
    int bol_attr_kappa_enabled;

    double scan_StartAngle,scan_ScanRange,scan_ExposureTime,scan_Speed;
    short scan_No_Passes,sht_set_arg,sht_get_arg;
    long lng_set_arg;
    int bol_set_arg;
    double dbl_set_arg,dbl_get_arg;
    double dbl_set_arg1,dbl_set_arg2,dbl_set_arg3,dbl_set_arg4;
    double dbl_get_arg1,dbl_get_arg2,dbl_get_arg3,dbl_get_arg4;
    int bol_get_arg;

    char mess[256];
    char ltoa_buffer[20];
    long aLong[1],lc;
	 

    static int com[32];

    int ii,jj,kk,itmp,perr,int_set_arg,int_set_arg1,int_set_arg2,int_set_arg3,int_set_arg4;

    long MAXLEN=128;

    xdr_free((xdrproc_t)xdr_md2_rpc_res_t, (void *)&tmp_res);

    tmp_res.errorno=RPC_NOERROR;    

    struct sockaddr_in *addr;
    struct tm *tm_v;
    time_t tim;

    tmp_res.errorno=RPC_NOERROR;

    DEBUG_ON = 1;
    


    switch (md2_rpc_arg->functcode){

	case RPC_EXIT:
	     lslogging_log_message("md2pmac_rpc_call_1: RPC_EXIT\n");
	     break;
	    
	case RPC_OPEN:
	    
             perr = 0;
             tmp_res.errorno=0;
             tmp_res.md2_rpc_res_t_u.result.status=0;
             
	     lslogging_log_message("md2pmac_rpc_call_1: RPC_OPEN: %s\n",md2_rpc_arg->line); 

             return &tmp_res;
	     break;
	    
	case RPC_PUTS: // no return from server expected

	     tmp_res.md2_rpc_res_t_u.result.line[0]=0;

             //insufficient no. args
	     if (2>sscanf(md2_rpc_arg->line,"%s%s%s%s%s%s%s%s%s%s%s]",&cmd,&command,&sa1,&sa2,&sa3,&sa4,&sa5,&sa6,&sa7,&sa8,&sa9)) {

		sprintf(tmp_res.md2_rpc_res_t_u.result.line," Mis-Structured Put Command (%s)",md2_rpc_arg->line);
         	tmp_res.md2_rpc_res_t_u.result.status=-1;
	        	      
	        if (DEBUG_ON) {
		   sprintf(mess,"mis-structured RPC_PUTS command:%s",tmp_res.md2_rpc_res_t_u.result.line);
                   lslogging_log_message(mess);	       
		}

		return &tmp_res;
	     }

             else {

                //turn off server debug mode
	        if (strcmp(command,"disable_debug")==0 || strcmp(command,"DISABLE_DEBUG")==0) {
	           DEBUG_ON = 0;
		   return &tmp_res;
	        }

	        //turn on server debug mode
		else if (strcmp(command,"enable_debug")==0 || strcmp(command,"ENABLE_DEBUG")==0) {
	           DEBUG_ON = 1;
		   return &tmp_res;
	        }

                //Reset MD2
	        else if (strcmp(command,"reset_md2")==0 || strcmp(command,"RESET_MD2")==0) {
		   //res = MD2Reset(faildesc);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," MD2 Reset Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful MD2 Reset");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
					 }
                   if (DEBUG_ON) {
		      sprintf(mess," MD2Reset: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//set MD2 to robot load configuration args: xcen ycen zcen zcapbs
		else if (strcmp(command,"robot_load_config")==0 || strcmp(command,"ROBOT_LOAD_CONFIG")==0) {
	           dbl_set_arg1 = atof(sa1);
		   dbl_set_arg2 = atof(sa2);
		   dbl_set_arg3 = atof(sa3);
		   dbl_set_arg4 = atof(sa4);

                   //res = putRobotLoadConfig(faildesc, dbl_set_arg1, dbl_set_arg2, dbl_set_arg3, dbl_set_arg4);
		   
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," setRobotLoadConfig Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
                      sprintf(tmp_res.md2_rpc_res_t_u.result.line,
                              " Successful Set Robot Load Config XYZ Position: cenx:%f ceny:%f cenz:%f capbsz:%f",
	                      dbl_set_arg1,dbl_set_arg2,dbl_set_arg3,dbl_set_arg4);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		      robot_config = 1;
		   }

                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_SET_ROBOTMODE_SV\r\n$1\r\n1\r\n");
		      redis_ret = transact_redis_socket(redis_out);
                   }

		   if (DEBUG_ON) {
		      sprintf(mess," putRobotLoadConfig: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }	         
                   return &tmp_res;		         
		}

		//set MD2 to robot status configuration
	        else if (strcmp(command,"robot_status_config")==0 || strcmp(command,"ROBOT_STATUS_CONFIG")==0) {
		   dbl_set_arg1 = atof(sa1);
		   dbl_set_arg2 = atof(sa2);
		   dbl_set_arg3 = atof(sa3);
		   dbl_set_arg4 = atof(sa4);

                   //res = putRobotStatusConfig(faildesc, dbl_set_arg1, dbl_set_arg2, dbl_set_arg3, dbl_set_arg4);
		   
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," setRobotStatusConfig Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
                      sprintf(tmp_res.md2_rpc_res_t_u.result.line,
			      " Successful Set Robot Status Config XYZ Position: cenx:%f ceny:%f cenz:%f capbsz:%f",
			      dbl_set_arg1,dbl_set_arg2,dbl_set_arg3,dbl_set_arg4);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		      robot_config = -1;
		   }

                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_SET_ROBOTMODE_SV\r\n$2\r\n-1\r\n");
		      redis_ret = transact_redis_socket(redis_out);
		   }

	           if (DEBUG_ON) {
		      sprintf(mess," putRobotStatusConfig: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }	         
                   return &tmp_res;		         
		}

	        //set MD2 Phase Position
		else if (strcmp(command,"phase_position")==0 || strcmp(command,"PHASE_POSITION")==0) {
			/*	MDPOSITION_ReadyToLoad = 0,		manualMount
				MDPOSITION_ReadyToCentre = 1,		center
				MDPOSITION_ReadyToLocateBeam = 2,	beamLocation
				MDPOSITION_ReadyToAlign = 3,		robotMount
				MDPOSITION_ReadyToScan = 4,		dataCollection
				MDPOSITION_ReadyToUnload = 5,		safe
				MDPOSITION_PosUnknown =	6   */

		    int_set_arg = atoi(sa1);

		    switch (int_set_arg) {
			case 0:
				md2cmds_phase_change("phase manuMount");
				last_phase_comit = 0;
				break;
			case 1:
				md2cmds_phase_change("phase center");
				last_phase_comit = 1;
				break;
			case 2:
				md2cmds_phase_change("phase beamLocation");
				last_phase_comit = 2;
				break;
			case 3:
				md2cmds_phase_change("phase robotMount");
				last_phase_comit = 3;
				break;
			case 4:
				md2cmds_phase_change("phase dataCollection");
				last_phase_comit = 4;
				break;
			case 5:
				md2cmds_phase_change("phase safe");
				last_phase_comit = 5;
				break;
			case 6:
				md2cmds_phase_change("phase safe");
				last_phase_comit = 6;
				break;
			default:
				last_phase_comit = -1;
		   }
                   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," MDPhasePosition set to: = %d",int_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%d",int_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$25\r\nMD2_SET_PHASE_POSITION_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message(" putMDPhasePosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
                   return &tmp_res;
		}

                //home omega
                else if (strcmp(command,"home_omega")==0 || strcmp(command,"HOME_OMEGA")==0) {

	           lspmac_home1_queue( omega);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: Home Omega");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;
		}

		//set omega
		else if (strcmp(command,"omega_angle")==0 || strcmp(command,"OMEGA_ANGLE")==0 ||
			 strcmp(command,"omega")==0 || strcmp(command,"OMEGA")==0) {

	           sprintf (mov_cmd,"moveAbs omega %s", sa1);
                   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS omega: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_SET_OMEGA_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;
	        }

		//set relative omega (incremental move)
	        else if (strcmp(command,"rel_omega_angle")==0 || strcmp(command,"REL_OMEGA_ANGLE")==0 ||
			 strcmp(command,"rel_omega")==0 || strcmp(command,"REL_OMGEA")==0) {

		   dbl_attr_omega = lspmac_getPosition(omega); 
		   dbl_set_arg = atof(sa1) + dbl_attr_omega;

		   sprintf(mov_cmd,"moveAbs omega %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) omega: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_SETREL_OMEGA_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    

                   return &tmp_res;
		}

		//home Kappa
                else if (strcmp(command,"home_kappa")==0 || strcmp(command,"HOME_KAPPA")==0) {

	           lspmac_home1_queue(kappa);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: Home Kappa");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;
		}

		//set kappa
		else if (strcmp(command,"kappa_angle")==0 || strcmp(command,"KAPPA_ANGLE") == 0 ||
			 strcmp(command,"kappa")==0 || strcmp(command,"KAPPA") == 0)  {
                   
	           sprintf (mov_cmd,"moveAbs kappa %s", sa1);
                   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC moveAbs kappa: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_SET_KAPPA_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;
		}

		//set relative kappa
		else if (strcmp(command,"rel_kappa_angle")==0 || strcmp(command,"REL_KAPPA_ANGLE")==0 ||
			 strcmp(command,"rel_kappa")==0 || strcmp(command,"REL_KAPPA")==0) {

		   dbl_attr_omega = lspmac_getPosition(kappa); 
		   dbl_set_arg = atof(sa1) + dbl_attr_kappa;
		   sprintf(mov_cmd,"moveAbs kappa %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC moveAbs (incr) kappa: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_SETREL_KAPPA_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;		 
		}

		//home KappaPhi
                else if (strcmp(command,"home_kappa_phi")==0 || strcmp(command,"HOME_KAPPA_PHI")==0 ||
			 strcmp(command,"home_phi")==0 | strcmp(command,"HOME_PHI")==0) {

	           lspmac_home1_queue(phi);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: Home KappaPhi");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;
		}

		//set kappa_phi
	        else if (strcmp(command,"kappa_phi_angle")==0 || strcmp(command,"KAPPA_PHI_ANGLE")==0 ||
			 strcmp(command,"phi")==0 || strcmp(command,"PHI")==0) {

	           sprintf (mov_cmd,"moveAbs phi %s", sa1);
                   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveAbs kappaphi: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_SET_KAPPAPHI_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;		  

		}

	        //set relative kappa_phi
		else if (strcmp(command,"rel_kappa_phi_angle")==0 || strcmp(command,"REL_KAPPA_PHI_ANGLE")==0 ||
			 strcmp(command,"rel_phi")==0 || strcmp(command,"REL_PHI")==0) {
		   
                   dbl_attr_omega = lspmac_getPosition(phi); 
		   dbl_set_arg = atof(sa1) + dbl_attr_kappaphi;
		   sprintf(mov_cmd,"moveAbs phi %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveAbs (incr) kappaphi %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_SETREL_KAPPAPHI_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;		 		 
		}

		// absolute move all rotation axes
		else if (strcmp(command,"all_axis_positions")==0 || strcmp(command,"ALL_AXIS_POSITIONS")==0) {
	           dbl_set_arg1 = atof(sa1);
		   dbl_set_arg2 = atof(sa2);
		   dbl_set_arg3 = atof(sa3);
		   //res = putAllAxisPositions(faildesc, dbl_set_arg1, dbl_set_arg2, dbl_set_arg3);
					  
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set All Axis Positions Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,
			      " Successful Set All Axis Positions: x:%f y:%f z:%f",
			      dbl_set_arg1,dbl_set_arg2,dbl_set_arg3);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }

                   if (DEBUG_ON) {
		      sprintf(mess," putAllAxisPositions: %s",tmp_res.md2_rpc_res_t_u.result.line);
	  	      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

                //move motors synchronously (valid motor names: omega kappa phi x y z cx cy)
		//motor targets are doubles
	        else if (strcmp(command,"move_motors_sync")==0 || strcmp(command,"MOVE_MOTORS_SYNC")==0) {
                   icnt = sscanf(md2_rpc_arg->line,"%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s]",&cmd,&command,
						   &sv[0][0],&sv[1][0],&sv[2][0],&sv[3][0],&sv[4][0],&sv[5][0],&sv[6][0],&sv[7][0],
					           &sv[8][0],&sv[9][0],&sv[10][0],&sv[11][0],&sv[12][0],&sv[13][0],&sv[14][0],&sv[15][0]);
                   icnt = icnt - 2;
		   
                   for (kk=0;kk<icnt/2;kk++) {
                       for (ii=0;ii<5;ii++) pmotornames[kk][ii]=sv[kk][ii];
		       jj = kk + (icnt/2);
		       for (ii=0;ii<10;ii++) pmotorpos[kk][ii] = sv[jj][ii];
		   }
		   //res = putMoveMotorSync(faildesc, icnt/2, pmotornames, pmotorpos);

		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," set Move Motors Sync Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," putMoveMotorSync: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
		   return &tmp_res;
		}

                //null_all_axes (omega, kappa & kappa-phi -> 0.0)
		else if (strcmp(command,"null_all_axes")==0 || strcmp(command,"NULL_ALL_AXES")==0) {
		   //res = putNullAllAxes(faildesc);
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," null_all_axes Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
	    	      sprintf(tmp_res.md2_rpc_res_t_u.result.line," All Axes Nulled");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
	           }

                  if (REDIS_SYNC = 1) {
		     sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_SET_OMEGA_SV\r\n$5\r\n0.000\r\n");
                     redis_ret = transact_redis_socket(redis_out);
                     sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_SET_KAPPA_SV\r\n$5\r\n0.000\r\n");
		     redis_ret = transact_redis_socket(redis_out);
		     sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_SET_KAPPAPHI_SV\r\n$5\r\n0.000\r\n");
		     redis_ret = transact_redis_socket(redis_out);
		  }
                  if (DEBUG_ON) {
		     sprintf(mess," null_all_axes: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
	          }
                  return &tmp_res;
	        }

                //set scan_startangle
	        else if (strcmp(command,"scan_start_angle")==0 || strcmp(command,"SCAN_START_ANGLE")==0) {
	           dbl_set_arg = atof(sa1);
		   //res = putStartAngle(faildesc, dbl_set_arg);
		 
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," set StartAngle Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		     sprintf(tmp_res.md2_rpc_res_t_u.result.line," StartAngle set to: = %f",dbl_set_arg);
                     tmp_res.md2_rpc_res_t_u.result.status=0;
		   }


                   if (DEBUG_ON) {
		      sprintf(mess," putStartAngle: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //set scan_range
	        else if (strcmp(command,"scan_range")==0 || strcmp(command,"SCAN_Range")==0) {
	           dbl_set_arg = atof(sa1);
                   //res = putScanRange(faildesc, dbl_set_arg);
		  
                   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," set ScanRange Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," ScanRange set to: = %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }


                   if (DEBUG_ON) {
		      sprintf(mess," putScanRange: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //set scan_exposure_time
	        else if (strcmp(command,"scan_exposure_time")==0 || strcmp(command,"SCAN_EXPOSURE_TIME")==0) {
	           dbl_set_arg = atof(sa1);
		   //res = putScanExposureTime(faildesc, dbl_set_arg);
					
                   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," set ScanExposureTime Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," ScanExposureTime set to: = %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                     
                   if (DEBUG_ON) {
		      sprintf(mess," putScanExposureTime: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //set no. scan passes
	        else if (strcmp(command,"scan_no_passes")==0 || strcmp(command,"SCAN_NO_PASSES")==0) {
	           sht_set_arg = atoi(sa1);
		   //res = putScanNumberOfPasses(faildesc, sht_set_arg);
	           if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," set ScanNumberOfPasses Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
                   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," ScanNumberOfPasses set to: = %d",sht_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putScanNumberOfPasses: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //set Scan Anticipation mode
	        else if (strcmp(command,"scan_anticipation")==0 || strcmp(command,"SCAN_Anticipation")==0) {
	           sht_set_arg = atoi(sa1);
		   //res = putScanAnticipation(faildesc, sht_set_arg);
	           if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," set ScanAnticipation Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," ScanAnticipation set to: = %d",sht_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
                   }
                   if (REDIS_SYNC = 1) {
	              sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_SCAN_ANTICIPATION_SV\r\n$%d\r\n%d\r\n",1,sht_set_arg);
		      redis_ret = transact_redis_socket(redis_out);
	 	   }
                   if (DEBUG_ON) {
		      sprintf(mess," putScanAnticipation: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //set scan parameters ... all params set in one rpc call
	        else if (strcmp(command,"scan_params")==0 || strcmp(command,"SCAN_PARAMS")==0) {
                   scan_StartAngle = atof(sa1);
		   //res = putStartAngle(faildesc, scan_StartAngle);
		   scan_ScanRange = atof(sa2);
		   //res1 = putScanRange(faildesc, scan_ScanRange);
		   scan_ExposureTime = atof(sa3);
		   //res2 = putScanExposureTime(faildesc, scan_ExposureTime);
		   scan_No_Passes = atoi(sa4);
		   //res3 = putScanNumberOfPasses(faildesc, scan_No_Passes);
                   if ((res+res1+res2+res3) != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," setScanParameters Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," setScanParameters: start phi:%f delta phi:%f Exp time:%f No Passes:%d",
		                                                  scan_StartAngle,scan_ScanRange,scan_ExposureTime,scan_No_Passes);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
	           }
                   if (DEBUG_ON) {
		      sprintf(mess," putScanParams: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //start pre-programmed scan
	        else if (strcmp(command,"start_scan")==0 || strcmp(command,"START_SCAN")==0) {
	           //res = StartScan(faildesc);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," startscan Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
	           else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," successful start scan");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
	           }
                   if (DEBUG_ON) {
	              sprintf(mess," putStartScan: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //abort scan in progress
                else if (strcmp(command,"abort_scan")==0 || strcmp(command,"ABORT_SCAN")==0) {
	           //res = AbortScan(faildesc);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," startscan Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," successful abort scan");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putStartScan: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

	        //move organ to on beam preset
	        else if (strcmp(command,"move_organ_onbeam")==0 || strcmp(command,"MOVE_ORGAN_ONBEAM")==0) {
	           goup = atoi(sa1);
		   //res = putMoveOrganOnBeam(faildesc, goup);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," Move Organ On Beam Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Organ On Beam ");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveOrganOnBema: %s",tmp_res.md2_rpc_res_t_u.result.line);
	   	      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //move organ to scintillator preset
	        else if (strcmp(command,"move_organ_scintil")==0 || strcmp(command,"MOVE_ORGAN_SCINTIL")==0) {
	           goup = atoi(sa1);
		   //res = putMoveOrganScintil(faildesc, goup);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line,"  Move Organ Scintil Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Organ Scintil ");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveOrganScintil: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

	        //move organ direct
	        else if (strcmp(command,"move_organ_direct")==0 || strcmp(command,"MOVE_ORGAN_DIRECT")==0) {
	           goup = atoi(sa1);
		   //res = putMoveOrganDirect(faildesc, goup);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line,"  Move Organ Direct Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Organ Direct");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveOrganDirect: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

	        //move organ all
	        else if (strcmp(command,"move_organ_all")==0 || strcmp(command,"MOVE_ORGAN_ALL")==0) {

	           int_set_arg1 = atoi(sa1); //aperture position spec 0->idle 1->hidden 2->onbeam 3-offbeam
		   int_set_arg2 = atoi(sa2); //capillary beam stop pos spec ..aper arg1
		   int_set_arg3 = atoi(sa3); //scintillator-pin diode pos spec 0->idle 1->hidden 2->scint on beam 3->pin on beam

		   //res = putMoveOrganAll(faildesc,int_set_arg1,int_set_arg2,int_set_arg3);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line,"  Move Organ All Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Organ All");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveOrganAll: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

	        //move sample on beam preset			  
	        else if (strcmp(command,"move_sample_onbeam")==0 || strcmp(command,"MOVE_SAMPLE_ONBEAM")==0) {
                   //res = putMoveSampleOnBeam(faildesc);
		   if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," Move Sample On Beam Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Sample On Beam ");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveSampleOnBeam: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

                //move sample off beam preset
	        else if (strcmp(command,"move_sample_offbeam")==0 || strcmp(command,"MOVE_SAMPLE_OFFBEAM")==0) {
	           //res = putMoveSampleOffBeam(faildesc);
	           if (res != 0){
	              sprintf(tmp_res.md2_rpc_res_t_u.result.line," Move Sample Off Beam Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Move Sample Off Beam ");
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putMoveSampleOffBeam: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
	        }

	        //open data shutter
	        else if (strcmp(command,"open_shutter")==0 || strcmp(command,"OPEN_SHUTTER")==0) {

 		   md2cmds_moveAbs ("moveAbs fastshutter 1");
		 
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs fshut 1");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;

	        }

	        //close data shutter
	        else if (strcmp(command,"close_shutter")==0 || strcmp(command,"CLOSE_SHUTTER")==0) {

 		   md2cmds_moveAbs ("moveAbs fastshutter 0");
		 
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs fshut 0");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	          
                   return &tmp_res;
	        }

                //activate front light
                else if (strcmp(command,"front_light")==0 || strcmp(command,"FRONT_LIGHT")==0) {

		   sprintf(mov_cmd,"moveAbs frontlight %f",sa1);
 		   md2cmds_moveAbs (mov_cmd);
		 
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs fligh_oo: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res; 
	        }

	        //set front light level
	        else if (strcmp(command,"front_light_level")==0 || strcmp(command,"FRONT_LIGHT_LEVEL")==0) {
	          
		   sprintf(mov_cmd,"moveAbs frontlight.intensity %s",sa1);
		   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs flight: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;
	        }

	        //activate back light
	        else if (strcmp(command,"back_light")==0 || strcmp(command,"BACK_LIGHT")==0) {

                   dbl_stat = atof(sa1);
		   //lspmac_moveabs_bo_queue(blight,dbl_stat);
		
		   sprintf(mov_cmd,"moveAbs backLight %f",dbl_stat);
 		   md2cmds_moveAbs (mov_cmd);
		 
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs blight_ud: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;
                }

	        //set back light level
	        else if (strcmp(command,"back_light_level")==0 || strcmp(command,"BACK_LIGHT_LEVEL")==0) {
	          
		   sprintf(mov_cmd,"moveAbs backLight.intensity %s",sa1);
		   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs bligh: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;
	        }

	        //insert fluorescence detector head			  
                else if (strcmp(command,"move_fluodetector_front")==0 || strcmp(command,"MOVE_FLUODETECTOR_FRONT")==0) {

		   md2cmds_moveAbs("moveAbs fluo 1");
		  
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs Fluo Front ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;
	        }

	        //retract fluorescence detector head
	        else if (strcmp(command,"move_fluodetector_back")==0 || strcmp(command,"MOVE_FLUODETECTOR_BACK")==0) {

		   md2cmds_moveAbs("moveAbs fluo 0");
		  
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs Fluo Back ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;	          
	        }

	        //home zoom axis
	        else if (strcmp(command,"home_zoom")==0 || strcmp(command,"HOME_ZOOM")==0) {

	           lspmac_home1_queue(zoom);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"successful zoom  Home ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;
	        }

	        //set zoom level
	        else if (strcmp(command,"zoom_level")==0 || strcmp(command,"ZOOM_LEVEL")==0) {
	          
	           sprintf (mov_cmd,"moveAbs cam.zoom %s", sa1);
                   //md2cmds_moveAbs (mov_cmd);
  		   if( pthread_mutex_trylock( &md2cmds_mutex) == 0) {
    		 	strncpy( md2cmds_cmd, mov_cmd, MD2CMDS_CMD_LENGTH-1);
    			md2cmds_cmd[MD2CMDS_CMD_LENGTH-1] = 0;
    			pthread_cond_signal( &md2cmds_cond);
    			pthread_mutex_unlock( &md2cmds_mutex);
  		   } else {
    			lslogging_log_message( "MD2 command '%s' ignored.  Already running '%s'", mov_cmd, md2cmds_cmd);
  		   }

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveAbs zoom: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;
	        }

                //absolute omegatable xaxis move
	        else if (strcmp(command,"omegatable_xaxis_position")==0 || strcmp(command,"OMEGATABLE_XAXIS_POSITION")==0 ||
	                 strcmp(command,"x_align_table") == 0 || strcmp(command,"X_ALIGN_TABLE") == 0) {

		   sprintf(mov_cmd,"moveAbs alignx %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) alignx: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;	          
	        }

                //relative omegatable xaxis move
	        else if (strcmp(command,"rel_omegatable_xaxis_position")==0 || strcmp(command,"REL_OMEGATABLE_XAXIS_POSITION")==0 ||
	                 strcmp(command,"rel_x_align_table") == 0 || strcmp(command,"REL_X_ALIGN_TABLE") == 0) {

		   dbl_attr_alignx = lspmac_getPosition(alignx); 
		   dbl_set_arg = atof(sa1) + dbl_attr_alignx;

		   sprintf(mov_cmd,"moveAbs alignx %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) alignx: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }

	        // absolute omegatable yaxis move
                else if (strcmp(command,"omegatable_yaxis_position")==0 || strcmp(command,"OMEGATABLE_YAXIS_POSITION")==0 ||
	                 strcmp(command,"y_align_table") == 0 || strcmp(command,"Y_ALIGN_TABLE") == 0) {
		   
		   sprintf(mov_cmd,"moveAbs aligny %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) aligny: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }

	        // relative omegatable yaxis move
	        else if (strcmp(command,"rel_omegatable_yaxis_position")==0 || strcmp(command,"REL_OMEGATABLE_YAXIS_POSITION")==0 ||
	                 strcmp(command,"rel_y_align_table") == 0 || strcmp(command,"REL_Y_ALIGN_TABLE") == 0) {

		   dbl_attr_aligny = lspmac_getPosition(aligny); 
		   dbl_set_arg = atof(sa1) + dbl_attr_aligny;

		   sprintf(mov_cmd,"moveAbs aligny %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) aligny: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;		   
	        }

                //absolute omegatable zaxis move
	        else if (strcmp(command,"omegatable_zaxis_position")==0 || strcmp(command,"OMEGATABLE_ZAXIS_POSITION")==0 ||
	                 strcmp(command,"z_align_table") == 0 || strcmp(command,"Z_ALIGN_TABLE") == 0) {
		   
		   sprintf(mov_cmd,"moveAbs alignz %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) alignz: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }

                //relative omegatable zaxis move
	        else if (strcmp(command,"rel_omegatable_zaxis_position")==0 || strcmp(command,"REL_OMEGATABLE_ZAXIS_POSITION")==0 ||
	                strcmp(command,"rel_z_align_table") == 0 || strcmp(command,"REL_Z_ALIGN_TABLE") == 0) {

		   dbl_attr_alignz = lspmac_getPosition(alignz); 
		   dbl_set_arg = atof(sa1) + dbl_attr_alignz;

		   sprintf(mov_cmd,"moveAbs alignz %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) alignz: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }

                // home X Centring
                else if (strcmp(command,"home_xcentering")==0 || strcmp(command,"HOME_XCENTERING")==0 ||
			 strcmp(command,"home_cenx")==0 || strcmp(command,"HOME_CENX")==0) {
	          
	           lspmac_home1_queue(cenx);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"successful cenx  Home ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;

	        }

                //absolute centeringtable xaxis move
	        else if (strcmp(command,"centeringtable_xaxis_position")==0 || strcmp(command,"CENTERINGTABLE_XAXIS_POSITION")==0 ||
			 strcmp(command,"cenx")==0 || strcmp(command,"CENX")==0) {

		   sprintf(mov_cmd,"moveAbs centering.x %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) cenx: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }
                  
	        //relative centeringtable xaxis move
	        else if (strcmp(command,"rel_centeringtable_xaxis_position")==0 || strcmp(command,"REL_CENTERINGTABLE_XAXIS_POSITION")==0 ||
			 strcmp(command,"rel_cenx")==0 || strcmp(command,"REL_CENX")==0) {

		   dbl_attr_cenx = lspmac_getPosition(cenx); 
		   dbl_set_arg = atof(sa1) + dbl_attr_cenx;

		   sprintf(mov_cmd,"moveAbs centering.x %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) cenx: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;	           
	        }

                // home Y Centring
                else if (strcmp(command,"home_ycentering")==0 || strcmp(command,"HOME_YCENTERING")==0 ||
			 strcmp(command,"home_ceny")==0 || strcmp(command,"HOME_CENY")==0) {
	          
	           lspmac_home1_queue(ceny);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"successful ceny  Home ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;	          
	        }

	        //absolute centeringtable y-axis move
	        else if (strcmp(command,"centeringtable_yaxis_position")==0 || strcmp(command,"CENTERINGTABLE_YAXIS_POSITION")==0 ||
			 strcmp(command,"ceny")==0 || strcmp(command,"CENY")==0) {

		   sprintf(mov_cmd,"moveAbs centering.y %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) ceny: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
	        }

	        //relative centeringtable y-axis move
	        else if (strcmp(command,"rel_centeringtable_yaxis_position")==0 || strcmp(command,"REL_CENTERINGTABLE_YAXIS_POSITION")==0 ||
			 strcmp(command,"rel_ceny")==0 || strcmp(command,"REL_CENY")==0) {

		   dbl_attr_ceny = lspmac_getPosition(ceny); 
		   dbl_set_arg = atof(sa1) + dbl_attr_ceny;

		   sprintf(mov_cmd,"moveAbs centering.y %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) ceny: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;

		}
				  
	        //home PhiAxisTableY (Z-centering)
                else if (strcmp(command,"home_zcentering")==0 || strcmp(command,"HOME_ZCENTERING")==0 ||
			 strcmp(command,"home_alignz")==0 || strcmp(command,"HOME_ALIGNZ")==0 ||
			 strcmp(command,"home_cenz")==0 || strcmp(command,"HOME_CENZ")==0){
		   
	           lspmac_home1_queue(alignz);
		   
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"successful alignz  Home ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		 
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
                   
 		   return &tmp_res;

		}

		//absolute centeringtable z-axis move
		else if (strcmp(command,"centeringtable_zaxis_position")==0 || strcmp(command,"CENTERINGTABLE_ZAXIS_POSITION")==0 ||
			 strcmp(command,"alignz")==0 || strcmp(command,"ALIGNZ")==0 ||
			 strcmp(command,"cenz")==0 || strcmp(command,"CENZ")==0) {

		   sprintf(mov_cmd,"moveAbs align.z %s", sa1);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (abs) alignz: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;	 	  
		}

		//relative centeringtable z-axis move
		else if (strcmp(command,"rel_centeringtable_zaxis_position")==0 || strcmp(command,"REL_CENTERINGTABLE_ZAXIS_POSITION")==0 ||
			 strcmp(command,"rel_alignz")==0 || strcmp(command,"REL_ALIGNZ")==0 ||
			 strcmp(command,"rel_cenz")==0 || strcmp(command,"REL_CENZ")==0) {      

		   dbl_attr_alignz = lspmac_getPosition(alignz); 
		   dbl_set_arg = atof(sa1) + dbl_attr_alignz;

		   sprintf(mov_cmd,"moveAbs align.z %9.4f", dbl_set_arg);
                   md2cmds_moveAbs (mov_cmd);
		
	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveABS (incr) alignz: %9.4f",dbl_set_arg);
                   tmp_res.md2_rpc_res_t_u.result.status=0;  

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
		    
                   return &tmp_res;
		}

		//absolute centeringtable xyz position move
		else if (strcmp(command,"centeringtable_xyz_position")==0 || strcmp(command,"CENTERINGTABLE_XYZ_POSITION")==0) {
		   dbl_set_arg1 = atof(sa1);
		   dbl_set_arg2 = atof(sa2);
		   dbl_set_arg3 = atof(sa3);
		   //res = putCentringTableXYZPosition(faildesc, dbl_set_arg1, dbl_set_arg2, dbl_set_arg3);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Centering Table XYZ Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,
						    " Successful Set Centering Table XYZ Position: x:%f y:%f z:%f",
						    dbl_set_arg1,dbl_set_arg2,dbl_set_arg3);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putCentringTableXYZPosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//relative centeringtable xyz position move
		else if (strcmp(command,"rel_centeringtable_xyz_position")==0 || strcmp(command,"REL_CENTERINGTABLE_XYZ_POSITION")==0) {
		   //res = getCentringTableXYZPosition(faildesc,&dbl_get_arg1,&dbl_get_arg2,&dbl_get_arg3);
		   dbl_set_arg1 = atof(sa1) + dbl_get_arg1;
		   dbl_set_arg2 = atof(sa2) + dbl_get_arg2;
		   dbl_set_arg3 = atof(sa3) + dbl_get_arg3;
		   //res = putCentringTableXYZPosition(faildesc, dbl_set_arg1, dbl_set_arg2, dbl_set_arg3);	 
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Relative Centering Table XYZ Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,
				     " Successful Set Centering Table Relative XYZ Position: x:%f y:%f z:%f",
				     dbl_set_arg1,dbl_set_arg2,dbl_set_arg3);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		     sprintf(mess," putRelative CentringTableXYZPosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//lock md2 gui
		else if (strcmp(command,"gui_lock")==0 || strcmp(command,"GUI_LOCK")==0) {  

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC call: gui_lock");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated call: gui_lock: %s",tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;
		}

		//absolute aperture z position move
		else if (strcmp(command,"aperture_z_position")==0 || strcmp(command,"APERTURE_Z_POSITION")==0) {
		   dbl_set_arg = atof(sa1);
		   //res = putApertureZMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Aperture Z Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Aperture Z Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_SET_APERTURE_Z_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putApertureZMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//relative aperture z position move
		else if (strcmp(command,"rel_aperture_z_position")==0 || strcmp(command,"REL_APERTURE_Z_POSITION")==0) {
		   //res = getApertureZMotor(faildesc,&dbl_get_arg);
		   dbl_set_arg = atof(sa1) + dbl_get_arg;
		   //res = putApertureZMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Relative Aperture Z Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Relative Aperture Z Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$24\r\nMD2_SETREL_APERTURE_Z_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putRelative ApertureZMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//absolute aperture y position move
		else if (strcmp(command,"aperture_y_position")==0 || strcmp(command,"APERTURE_Y_POSITION")==0) {
		   dbl_set_arg = atof(sa1);
		   //res = putApertureYMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Aperture Y Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Aperture Y Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
                      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_SET_APERTURE_Y_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putApertureYMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//relative aperture y position move
		else if (strcmp(command,"rel_aperture_y_position")==0 || strcmp(command,"REL_APERTURE_Y_POSITION")==0) {
		   //res = getApertureYMotor(faildesc,&dbl_get_arg);
		   dbl_set_arg = atof(sa1) + dbl_get_arg;
		   //res = putApertureYMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Relative Aperture Y Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Relative Aperture Y Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$24\r\nMD2_SETREL_APERTURE_Y_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putRelative ApertureYMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//absolute capillary beam stop z position move
		else if (strcmp(command,"capillarybs_z_position")==0 || strcmp(command,"CAPILLARYBS_Z_POSITION")==0) {
		   dbl_set_arg = atof(sa1);
		   //res = putCapillaryBSZMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set CapillaryBS Z Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set CapillaryBS Z Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$24\r\nMD2_SET_CAPILLARYBS_Z_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putCapillaryBSZMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//relative capillary beam stop z position move
		else if (strcmp(command,"rel_capillarybs_z_position")==0 || strcmp(command,"REL_CAPILLARYBS_Z_POSITION")==0) {
		   //res = getCapillaryBSZMotor(faildesc,&dbl_get_arg);
		   dbl_set_arg = atof(sa1) + dbl_get_arg;
		   //res = putCapillaryBSZMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Relative CapillaryBS Z Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Relative CapillaryBS Z Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$27\r\nMD2_SETREL_CAPILLARYBS_Z_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putRelative CapillaryBSZMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//absolute capillary beam stop y position move
		else if (strcmp(command,"capillarybs_y_position")==0 || strcmp(command,"CAPILLARYBS_Y_POSITION")==0) {
		   dbl_set_arg = atof(sa1);
		   //res = putCapillaryBSYMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Capillary BS Y Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Capillary BS Y Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
                      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$24\r\nMD2_SET_CAPILLARYBS_Y_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putCapillaryBSYMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//relative capillary beam stop y position move
		else if (strcmp(command,"rel_capillarybs_y_position")==0 || strcmp(command,"REL_CAPILLARYBS_Y_POSITION")==0) {
		   //res = getCapillaryBSYMotor(faildesc,&dbl_get_arg);
		   dbl_set_arg = atof(sa1) + dbl_get_arg;
		   //res = putCapillaryBSYMotor(faildesc, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Relative CapillaryBS Y Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Relative CapillaryBS Y Position: %f",dbl_set_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(tmpstr,"%f",dbl_set_arg);
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$27\r\nMD2_SETREL_CAPILLARYBS_Y_SV\r\n$%d\r\n%s\r\n",strlen(tmpstr),tmpstr);
		      redis_ret = transact_redis_socket(redis_out);
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putRelative CapillaryBSYMotor: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//put previous centering position
		else if (strcmp(command,"previous_centering_position")==0 || strcmp(command,"PREVIOUS_CENTERING_POSITION")==0) {
		   //res = putPreviousCentringPosition(faildesc, sa1);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set Previous Centering Position Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set Previous Centering Position: %s",sa1);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," putPreviousCentringPosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//move sample vertically in focus plane     arg1 = current omega (deg)   arg2 = step size (mm)
		else if (strcmp(command,"move_in_focus_plane")==0 || strcmp(command,"MOVE_IN_FOCUS_PLANE")==0) {
		   
		   dbl_set_arg1 = atof(sa1);   //current omega
		   dbl_set_arg2 = atof(sa2);   //step size

                   dbl_attr_cenx = lspmac_getPosition(cenx);  //current cenx pos
		   dbl_attr_ceny = lspmac_getPosition(ceny);  //current ceny pos

                   cenx_mov =  dbl_set_arg2 * sin((dbl_set_arg1 - phiRef) / (180.0 * pi));
		   sprintf(mov_cmd,"moveAbs cenx %8.4f",cenx_mov);
		   md2cmds_moveAbs (mov_cmd);

		   ceny_mov = -1.0 * dbl_set_arg2 * cos((dbl_set_arg1 - phiRef) / (180.0 * pi));
		   sprintf(mov_cmd,"moveAbs ceny %8.4f",ceny_mov);
		   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful move in focus plane: %s %s",sa1,sa2);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: move in focus plane: moveAbs cenx: %8.4f moveAbs ceny: %8.4f",cenx_mov, ceny_mov);
		    
                   return &tmp_res;
		}

		//change xycentering perpendicular to focus plane (change sample focus)    arg1 = current omega (deg)   arg2 = step size (mm)
		else if (strcmp(command,"move_outof_focus_plane")==0 || strcmp(command,"MOVE_OUTOF_FOCUS_PLANE")==0) {
		   
		   dbl_set_arg1 = atof(sa1);   //current omega
		   dbl_set_arg2 = atof(sa2);   //step size

                   dbl_attr_cenx = lspmac_getPosition(cenx);  //current cenx pos
		   dbl_attr_ceny = lspmac_getPosition(ceny);  //current ceny pos

                   cenx_mov =  dbl_set_arg2 * sin((dbl_set_arg1 - phiRef + 90.0) / (180.0 * pi));
		   sprintf(mov_cmd,"moveAbs cenx %8.4f",cenx_mov);
		   md2cmds_moveAbs (mov_cmd);

		   ceny_mov = -1.0 * dbl_set_arg2 * cos((dbl_set_arg1 - phiRef + 90.0) / (180.0 * pi));
		   sprintf(mov_cmd,"moveAbs ceny %8.4f",ceny_mov);
		   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful move out of focus plane: %s %s",sa1,sa2);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: move out of focus plane: moveAbs cenx: %8.4f moveAbs ceny: %8.4f",cenx_mov, ceny_mov);
		    
                   return &tmp_res;

		}

		//change BGO Mast Piezo focus
	 	else if (strcmp(command,"bgo_piezofocus_pos")==0 || strcmp(command,"BGO_PIEZOFOCUS_POS	")==0) {
		  
		   sprintf(mov_cmd,"moveAbs fscint %s",sa1);
		   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"RPC_PUT: moveAbs fscint (BGO piezofocus): %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);

                   return &tmp_res;
		}

		//set BGO Mast Vertical Position
		else if (strcmp(command,"bgo_mast_position")==0 || strcmp(command,"BGO_MAST_POSITION")==0) {
                   
	           sprintf (mov_cmd,"moveAbs scint %s", sa1);
                   md2cmds_moveAbs (mov_cmd);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"PUT_RPC: moveAbs scint: %s",sa1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message(tmp_res.md2_rpc_res_t_u.result.line);
	
                   return &tmp_res;

		}

		//set beam size horizontal fiducial ellipse
		else if (strcmp(command,"beam_size_horizontal")==0 || strcmp(command,"BEAM_SIZE_HORIZONTAL")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC call: beam_size_horizontal");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated call: beam_size_horizontal");
	
                   return &tmp_res;
		}

		//set beam size vertical fiducial ellipse
		else if (strcmp(command,"beam_size_vertical")==0 || strcmp(command,"BEAM_SIZE_VERTICAL")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC call: beam_size_vertical");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated call: beam_size_vertical");
	
                   return &tmp_res;		  
		}

		//save parameter settings
		else if (strcmp(command,"save_param")==0 || strcmp(command,"SAVE_PARAM")==0) {
		   dbl_set_arg = atof(sa3);
		   //res = SaveParam(faildesc, sa1, sa2, dbl_set_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Set SaveParam Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," Successful Set SaveParam: %s %s %s",sa1,sa2,sa3);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (DEBUG_ON) {
		      sprintf(mess," SaveParam: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess)-1);
		   }
                   return &tmp_res;
		}

		//set captured image path
		else if (strcmp(command,"grab_image_path")==0 || strcmp(command,"GRAB_IMAGE_PATH")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC call: grab_image_path");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated call: grab_image_path");
	
                   return &tmp_res;		 
		}

		//capture image to file
		else if (strcmp(command,"grab_image_tofile")==0 || strcmp(command,"GRAB_IMAGE_TOFILE")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC call: grab_image_tofile");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated call: grab_image_tofile");
	
                   return &tmp_res;	
		}

		else {
                   sprintf(tmp_res.md2_rpc_res_t_u.result.line," Can't Parse RPC_PUTS Command:%s",command);
		   tmp_res.md2_rpc_res_t_u.result.status=-1;
		   lslogging_log_message("md2_rpc_call_1_svc: can't parse RPC_PUTS command token:%s",command);

		   return &tmp_res;
		}
 
	     }
                    
	     return &tmp_res;  
     
	case RPC_GETS: // send command and receive response

             tmp_res.md2_rpc_res_t_u.result.line[0]=0;

	     if (1>sscanf(md2_rpc_arg->line,"%s%[^\n]",&cmd,&command)) {

		sprintf(tmp_res.md2_rpc_res_t_u.result.line," Mis-Structured Put Command (%s)",md2_rpc_arg->line);
         	tmp_res.md2_rpc_res_t_u.result.status=-1;
	        	      
	        if (DEBUG_ON) {
		   sprintf(mess,"mis-structured RPC_GETS command:%s",tmp_res.md2_rpc_res_t_u.result.line);
                   lslogging_log_message(mess);	       
		}
	        
	     }
             
             else {

                //get status
		if (strcmp(command," status")==0 || strcmp(command," STATUS")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC_GETS: status");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated RPC_GETS: status");
	
                   return &tmp_res;
		}

                //get state
		else if (strcmp(command," state")==0 || strcmp(command," STATE")==0) {

		   lng_attr_state = lsredis_getl(md2cmds_md_status_code);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",lng_attr_state);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
               	   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$12\r\nMD2_STATE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("RPC_GETS: lsredit_getb md2cmds_md_state_code :%s",tmp_res.md2_rpc_res_t_u.result.line);

		   return &tmp_res;
		}	

                //get gui lock-out state
	        else if (strcmp(command," gui_lock")==0 || strcmp(command," GUI_LOCK")==0) {

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line," deprecated RPC_GETS: gui_lock ");
                   tmp_res.md2_rpc_res_t_u.result.status=0;

                   if (DEBUG_ON) lslogging_log_message("RPC_PUTS: deprecated RPC_GETS: gui_lock");
	
                   return &tmp_res;		   
		}

		//get MD2 Robot Configuration, locally managed 1=load config -1=status config
		else if (strcmp(command," robot_config") == 0 || strcmp(command," ROBOT_CONFIG")==0) {       	 
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",robot_config);
                   tmp_res.md2_rpc_res_t_u.result.status=0;	 
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_ROBOTMODE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getRobotConfig: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
                   return &tmp_res;
		}

                //get MD2 phase position
		else if (strcmp(command," phase_position")==0 || strcmp(command," PHASE_POSITION")==0) {
		
				/*	MDPOSITION_ReadyToLoad = 0,
					MDPOSITION_ReadyToCentre = 1,
					MDPOSITION_ReadyToLocateBeam = 2,
					MDPOSITION_ReadyToAlign = 3,
					MDPOSITION_ReadyToScan = 4,
					MDPOSITION_ReadyToUnload = 5, */
		
		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",last_phase_comit);
		   tmp_res.md2_rpc_res_t_u.result.status=0;
      
		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_PHASE_POSITION_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   
		   if (DEBUG_ON) lslogging_log_message("RPC_GETS: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}					 


		//get omega
		else if (strcmp(command," omega_angle")==0 || strcmp(command," OMEGA_ANGLE")==0 ||
			 strcmp(command," omega")==0 || strcmp(command," OMEGA")==0) {

		   dbl_attr_omega = lspmac_getPosition(omega); 

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.3f",dbl_attr_omega);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
               	   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$12\r\nMD2_OMEGA_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("RPC_GET: lspmac_getPosition omega:%s",tmp_res.md2_rpc_res_t_u.result.line);

		   return &tmp_res;
		}

		//get kappa angle
		else if (strcmp(command," kappa_angle")==0 || strcmp(command," KAPPA_ANGLE")==0 ||
			 strcmp(command," kappa")==0 || strcmp(command," KAPPA")==0) {

		   dbl_attr_kappa = lspmac_getPosition(kappa); 

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.3f",dbl_attr_kappa);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
               	   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$12\r\nMD2_KAPPA_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("RPC_GET: lspmac_getPosition kappa:%s",tmp_res.md2_rpc_res_t_u.result.line);

		   return &tmp_res;		   
		}			

		//get kappa enabled
		else if (strcmp(command," kappa_enabled")==0 || strcmp(command," KAPPA_ENABLED")==0) {

		   bol_attr_kappa_enabled = lsredis_getb(kappa->active);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",bol_attr_kappa_enabled);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		
		   if (DEBUG_ON) {
		      sprintf(mess,"RPC_GET: lsredis_getb kappa->active %s",tmp_res.md2_rpc_res_t_u.result.line);
		      lslogging_log_message(mess);
		   }

		   return &tmp_res;
		}			

		//get kappa-phi angle
		else if (strcmp(command," kappa_phi_angle")==0 || strcmp(command," KAPPA_PHI_ANGLE")==0 ||
			 strcmp(command," kappa_phi")==0 || strcmp(command," KAPPA_PHI")==0 )  {

		   dbl_attr_kappaphi = lspmac_getPosition(phi); 

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.3f",dbl_attr_kappaphi);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		  
               	   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$12\r\nMD2_KAPPAPHI_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("RPC_GET: lspmac_getPosition kappaphi:%s",tmp_res.md2_rpc_res_t_u.result.line);

		   return &tmp_res;	
		}	

		//get all axis positions return order: omega,kappa,kappa-phi
		else if (strcmp(command," all_axis_positions")==0 || strcmp(command," ALL_AXIS_POSITIONS")==0) {

		   dbl_attr_omega = lspmac_getPosition(omega);
		   dbl_attr_kappa = lspmac_getPosition(kappa);
		   dbl_attr_kappaphi = lspmac_getPosition(phi);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%9.4f %9.4f %9.4f", dbl_attr_omega, dbl_attr_kappa, dbl_attr_kappaphi);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$15\r\nMD2_ALL_AXES_SV\r\n$%d\r\n%s\r\n",strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
                      redis_ret = transact_redis_socket(redis_out);
		   } 

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: get all axis positions:%s",tmp_res.md2_rpc_res_t_u.result.line);

		   return &tmp_res;

		}

		//get fluodetector state
		else if (strcmp(command," fluodetector_is_back")==0 || strcmp(command," FLUODETECTOR_IS_BACK")==0) {
		   
		   dbl_attr_fluo = lspmac_getPosition(fluo);

		   if (dbl_attr_fluo > 0.0) {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"1");
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"0");
		   }
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_FLUODET_BACK_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position fluo: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}	

		//get shutter state
		else if (strcmp(command," shutter_is_open")==0 || strcmp(command," SHUTTER_IS_OPEN")==0) {

		   dbl_attr_shutter = lspmac_getPosition(fshut);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",(int)dbl_attr_shutter);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_SHUTTER_OPEN_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }		   
		   
		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position shutter: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		   
		}

		//get front light on state
		else if (strcmp(command," front_light")==0 || strcmp(command," FRONT_LIGHT")==0) {

		   dbl_attr_flightoo = lspmac_getPosition(flight_oo);
                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",(int)dbl_attr_zoom);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$18\r\nMD2_FRONT_LIGHT_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }	
		   
		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position frontlight: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}   
                   

		//get front light level
		else if (strcmp(command," front_light_level")==0 || strcmp(command," FRONT_LIGHT_LEVEL")==0) {

		   dbl_attr_flight = lspmac_getPosition(flight);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%9.4f", dbl_attr_flight);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$23\r\nMD2_FRONT_LIGHTLEVEL_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position frontlight level: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;

		}

		//get back light on (up/down) state
		else if (strcmp(command," back_light")==0 || strcmp(command," BACK_LIGHT")==0) {

		   dbl_attr_blightud = lspmac_getPosition(blight_ud);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",(int)dbl_attr_blightud);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$16\r\nMD2_BACKLIGHT_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position backlight: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}	

		//get back light level
		else if (strcmp(command," back_light_level")==0 || strcmp(command," BACK_LIGHT_LEVEL")==0) {

		   dbl_attr_blight = lspmac_getPosition(blight);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",(int)dbl_attr_blightud);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_BACKLIGHTLEVEL_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position backlight level: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}

				  
		//get zoom level
		else if (strcmp(command," zoom_level")==0 || strcmp(command," ZOOM_LEVEL")==0) {
		   
		   dbl_attr_zoom = lspmac_getPosition(zoom);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",(int)dbl_attr_zoom);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$19\r\nMD2_ZOOM_LEVEL_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position zoom: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}	

		//get all imaging params: return order zoom_level back_light_on_state back_light_level
		else if (strcmp(command," all_imaging_params")==0 || strcmp(command," ALL_IMAGING_PARAMS")==0) {

		   dbl_attr_zoom = lspmac_getPosition(zoom);
		   dbl_attr_blightud = lspmac_getPosition(blight_ud);
		   dbl_attr_blight = lspmac_getPosition(blight);

		   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d %d %5.2f",(int)dbl_attr_zoom,(int)dbl_attr_blightud,dbl_get_arg1);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$25\r\nMD2_ALL_IMAGING_PARAMS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("RPG_GET: lspmac_getPosition all imaging params %s",tmp_res.md2_rpc_res_t_u.result.line);
		    
	           return &tmp_res;
		}

		//get omega table X axis position (x_align_table)
		else if (strcmp(command," omegatable_xaxis_position")==0 || strcmp(command," OMEGATABLE_XAXIS_POSITION")==0 ||
		         strcmp(command," x_align_table") == 0 || strcmp(command," X_ALIGN_TABLE") == 0 ||
			 strcmp(command," align.x")==0 || strcmp(command," ALIGN.X")==0 ) {
		   
		   dbl_attr_alignx = lspmac_getPosition(alignx);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_alignx);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_X_ALIGN_TABLE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position alignx: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}

		//get omega table Y axis position (y_align_table)
	 	else if (strcmp(command," omegatable_yaxis_position")==0 || strcmp(command," OMEGATABLE_YAXIS_POSITION")==0 ||
		         strcmp(command," y_align_table") == 0 || strcmp(command," Y_ALIGN_TABLE") == 0 ||
			 strcmp(command," align.y")==0 || strcmp(command," ALIGN.Y")==0) {		             
		   
		   dbl_attr_aligny = lspmac_getPosition(aligny);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_aligny);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_Y_ALIGN_TABLE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position aligny: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		  
		}
	
		//get omega table Z axis position (z_align_table)
	 	else if (strcmp(command," omegatable_zaxis_position")==0 || strcmp(command," OMEGATABLE_ZAXIS_POSITION")==0 ||
			 strcmp(command," z_align_table") == 0 || strcmp(command," Z_ALIGN_TABLE") == 0 ||
			 strcmp(command," align.z")==0 || strcmp(command," ALIGN.Z")==0) {
		 
		   dbl_attr_alignz = lspmac_getPosition(alignz);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_alignz);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_Z_ALIGN_TABLE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position alignz: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;
		}

		//get centering table X axis position
		else if (strcmp(command," centeringtable_xaxis_position")==0 || strcmp(command," CENTERINGTABLE_XAXIS_POSITION")==0 ||
			 strcmp(command," cenx")==0 || strcmp(command," CENX")==0 ) {
		   
		   dbl_attr_cenx = lspmac_getPosition(cenx);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_cenx);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$28\r\nMD2_CENTERING_TABLE_XAXIS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position cenx: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		   
		}

		//get centering table Y axis position
		else if (strcmp(command," centeringtable_yaxis_position")==0 || strcmp(command," CENTERINGTABLE_YAXIS_POSITION")==0 ||
			 strcmp(command," ceny")==0 || strcmp(command," CENY")==0) {
		   
		   dbl_attr_ceny = lspmac_getPosition(ceny);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_ceny);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$28\r\nMD2_CENTERING_TABLE_YAXIS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position ceny: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		   
		}

		//get centering table Z axis position
		else if (strcmp(command," centeringtable_zaxis_position")==0 || strcmp(command," CENTERINGTABLE_ZAXIS_POSITION")==0 ||
			 strcmp(command," cenz")==0 || strcmp(command," CENZ")==0) {
		   
		   dbl_attr_alignz = lspmac_getPosition(alignz);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_alignz);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$28\r\nMD2_CENTERING_TABLE_ZAXIS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position alingz: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		  
		}	

		//get centering table vertical position
		else if (strcmp(command," centeringtable_vertical_position")==0 || strcmp(command," CENTERINGTABLE_VERTICAL_POSITION")==0) {
		   //res = getCentringTableVerticalPosition(faildesc,&dbl_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getCentringTableVerticalPosition Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%7.3f",dbl_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$31\r\nMD2_CENTERING_TABLE_VERTICAL_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getCentringTableVerticalPosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}

		//get centering table XYZ positions
		else if (strcmp(command," centeringtable_xyz_position")==0 || strcmp(command," CENTERINGTABLE_XYZ_POSITION")==0 ||
			 strcmp(command," cen_xyz")==0 || strcmp(command," CEN_XYZ")==0 ) {

		   dbl_get_arg1 = lspmac_getPosition(cenx);
                   dbl_get_arg2 = lspmac_getPosition(ceny);
                   dbl_get_arg3 = lspmac_getPosition(alignz);

	           sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%6.3f %6.3f %6.3f",dbl_get_arg1,dbl_get_arg2,dbl_get_arg3);
                   tmp_res.md2_rpc_res_t_u.result.status=0;
		   
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$26\r\nMD2_CENTERING_TABLE_XYZ_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
	           if (DEBUG_ON) lslogging_log_message("RPC_GET: lpmac_getPostion centering xyz:%s",tmp_res.md2_rpc_res_t_u.result.line);
		   
		   return &tmp_res;
		}

		//get scan start angle
		else if (strcmp(command," scan_start_angle")==0 || strcmp(command," SCAN_START_ANGLE")==0) {
		   //res = getStartAngle(faildesc,&dbl_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getStartAngle Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.3f",dbl_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$23\r\nMD2_SCAN_START_ANGLE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getStartAngle: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}

		//get scan angle range
		else if (strcmp(command," scan_range")==0 || strcmp(command," SCAN_RANGE")==0) {
		   //res = getScanRange(faildesc,&dbl_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getScanRange Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.3f",dbl_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$17\r\nMD2_SCAN_RANGE_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getScanRange: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}

		//get scan exposure time
		else if (strcmp(command," scan_exposure_time")==0 || strcmp(command," SCAN_EXPOSURE_TIME")==0) {
		   //res = getScanExposureTime(faildesc,&dbl_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getScanExposureTime Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%6.3f",dbl_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$25\r\nMD2_SCAN_EXPOSURE_TIME_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getScanExposureTime: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		   }

		//get scan no. passes
		else if (strcmp(command," scan_no_passes")==0 || strcmp(command," SCAN_NO_PASSES")==0) {
		   //res = getScanNumberOfPasses(faildesc,&sht_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getScanNumberOfPasses Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",sht_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_SCAN_NO_PASSES_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getScanNumberOfPasses: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}

		//get scan anticipation
		else if (strcmp(command," scan_anticipation")==0 || strcmp(command," SCAN_ANTICIPATION")==0) {
		   //res = getScanAnticipation(faildesc,&sht_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getScanAnticipation Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%d",sht_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_SCAN_ANTICIPATION_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getScanAnticipation: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}

		//get centering table X axis position
		else if (strcmp(command," centeringtable_xaxis_position")==0 || strcmp(command," CENTERINGTABLE_XAXIS_POSITION")==0) {
		   //res = getCentringTableXAxisPosition(faildesc,&dbl_get_arg);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getCentringTableXAxisPosition Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%6.3f",dbl_get_arg);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
                   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$28\r\nMD2_CENTERING_TABLE_XAXIS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getCentringTableXaxisPosition: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}


		//get aperture alignment table Z-position
		else if (strcmp(command," aperture_z_position")==0 || strcmp(command," APERTURE_Z_POSITION")==0  ||
			 strcmp(command," appz")==0 || strcmp(command," APPZ")==0 ) {
		   
		   dbl_attr_aperz = lspmac_getPosition(aperz);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_aperz);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$17\r\nMD2_APERTURE_Z_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position aperz: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		   
		}

		//get aperture alignment table Y-position
		else if (strcmp(command," aperture_y_position")==0 || strcmp(command," APERTURE_Y_POSITION")==0 ||
			 strcmp(command," appy")==0 || strcmp(command," APPY")==0 ) {
		   
		   dbl_attr_apery = lspmac_getPosition(apery);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_apery);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$17\r\nMD2_APERTURE_Y_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position apery: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;			  
		}

		//get capillary beam stop alignment table Z-position
		else if (strcmp(command," capillarybs_z_position")==0 || strcmp(command," CAPILLARYBS_Z_POSITION")==0 ||
			 strcmp(command," capz")==0 || strcmp(command," CAPZ")==0 ) {
		   
		   dbl_attr_capz = lspmac_getPosition(capz);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_capz);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_CAPILLARYBZ_Z_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position capz: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;			   
		}

		//get capillary beam stop alignment table Y-position
		else if (strcmp(command," capillarybs_y_position")==0 || strcmp(command," CAPILLARYBS_Y_POSITION")==0 ||
			 strcmp(command," capy")==0 || strcmp(command," CAPY")==0 ) {
		   
		   dbl_attr_capy = lspmac_getPosition(capy);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_capy);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$20\r\nMD2_CAPILLARYBZ_Y_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position capy: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;		  
		}

		//get BGO Mast Piezo Focus Pos
		else if (strcmp(command," bgo_piezofocus_pos")==0 || strcmp(command," BGO_PIEZOFOCUS_POS")==0 ||
			 strcmp(command," scint.focus")==0 || strcmp(command," SCINT.FOCUS")==0 ) {
		   
		   dbl_attr_fscint = lspmac_getPosition(fscint);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_fscint);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$21\r\nMD2_BGO_PIEZOFOCUS_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position fscint: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;			   
		}

		//get BGO Mast Position
		else if (strcmp(command," bgo_mast_position")==0 || strcmp(command," BGO_MAST_POSITION")==0 ||
			 strcmp(command," scint")==0 || strcmp(command," SCINT")==0 ) {
		   
		   dbl_attr_scint = lspmac_getPosition(scint);

                   sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%8.4f",dbl_attr_scint);
                   tmp_res.md2_rpc_res_t_u.result.status=0;

		   if (REDIS_SYNC = 1) {
		      sprintf(redis_out,"*3\r\n$3\r\nSET\r\n$24\r\nMD2_BGO_MAST_POSITION_SV\r\n$%d\r\n%s\r\n",
		      strlen(tmp_res.md2_rpc_res_t_u.result.line),tmp_res.md2_rpc_res_t_u.result.line);
		      redis_ret = transact_redis_socket(redis_out);
		   }

		   if (DEBUG_ON) lslogging_log_message("GET_RPC: lspmac_get_Position scint: %s",tmp_res.md2_rpc_res_t_u.result.line);
		     
		   return &tmp_res;			 
		}

		//get previous centering position
		else if (strcmp(command," previous_centering_position")==0 || strcmp(command," PREVIOUS_CENTERING_POSITION")==0) {
		   //res = getPreviousCentringPosition(faildesc,prev_position);
		   if (res != 0){
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line," getPreviousCentringPOsition Error = %s",faildesc);
                      tmp_res.md2_rpc_res_t_u.result.status=-1;
		   }
		   else {
		      sprintf(tmp_res.md2_rpc_res_t_u.result.line,"%s",prev_position);
                      tmp_res.md2_rpc_res_t_u.result.status=0;
		   }
		   if (DEBUG_ON) {
		      sprintf(mess," getPreviousCentringPOs: %s",tmp_res.md2_rpc_res_t_u.result.line);
		      WRT_SCROLL_MESS((char *)mess,strlen(mess));
		   }
		   return &tmp_res;
		}


		else {
                   sprintf(tmp_res.md2_rpc_res_t_u.result.line," Can't Parse RPC_GETS Command:%s",command);
		   tmp_res.md2_rpc_res_t_u.result.status=-1;
		   lslogging_log_message("md2_rpc_call_1_svc: can't parse RPC_GETS command  token:%s",command);
		   return &tmp_res;
	        }

	}	     
        return &tmp_res;
		
	default:
	    lslogging_log_message("md2_rpc_call_1_svc: Unknown RPC function code\n");
	    tmp_res.errorno=RPC_UNKNOWN_FUNCTION;
	    break;
	}

     tmp_res.md2_rpc_res_t_u.result.status=0;
     return (&tmp_res);
}
















