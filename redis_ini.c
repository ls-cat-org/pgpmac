#include "pgpmac.h"
#include "pgpmac_rpc.h"

   static int stat_no;

//microdiff_hard.ini parser handler function for inih
static int microdiff_hard_handler(void* motor_name, const char* section, const char* name, const char* value) {

   int parse_mparms, gen_redis_record, len;
   char *redis_fostr;
   char maatel_mot_name[100];
   char hash_key[100];
   char hash_field[100];
   char hash_value[100];
   inih_config* pconfig = (inih_config*) motor_name;

   parse_mparms = -1;

   if (strcmp(section,"ApertureYZTable.ApertureYMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.appy",stat_no);
      sprintf(maatel_mot_name,"ApertureYZTable.ApertureYMotor");      
   }
   else if (strcmp(section,"ApertureYZTable.ApertureZMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.appz",stat_no);
      sprintf(maatel_mot_name,"ApertureYZTable.ApertureZMotor");
   }
   else if (strcmp(section,"CapillaryBSYZtable.CapillaryBSYMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.capy",stat_no);
      sprintf(maatel_mot_name,"CapillaryBSYZtable.CapillaryBSYMotor");   
   }
   else if (strcmp(section,"CapillaryBSYZtable.CapillaryBSZMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.capz",stat_no);
      sprintf(maatel_mot_name,"CapillaryBSYZtable.CapillaryBSZMotor");
   }
   else if (strcmp(section,"CentringXYTable.XCentringMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.centering.x",stat_no);
      sprintf(maatel_mot_name,"CentringXYTable.XCentringMotor");
   }
   else if (strcmp(section,"CentringXYTable.YCentringMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.centering.y",stat_no);
      sprintf(maatel_mot_name,"CentringXYTable.YCentringMotor");
   }
   else if (strcmp(section,"MiniKappa.Kappa1") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.kappa",stat_no);
      sprintf(maatel_mot_name,"MiniKappa.Kappa1");
   }
   else if (strcmp(section,"MiniKappa.Kappa2") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.phi",stat_no);
      sprintf(maatel_mot_name,"MiniKappa.Kappa2");
   }
   else if (strcmp(section,"PHIAxisXYZTable.PHIXMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.align.x",stat_no);
      sprintf(maatel_mot_name,"PHIAxisXYZTable.PHIXMotor");
   }
   else if (strcmp(section,"PHIAxisXYZTable.PHIYMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.align.y",stat_no);
      sprintf(maatel_mot_name,"PHIAxisXYZTable.PHIYMotor");
   }
   else if (strcmp(section,"PHIAxisXYZTable.PHIZMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.align.z",stat_no);
      sprintf(maatel_mot_name,"PHIAxisXYZTable.PHIZMotor");
   }
   else if (strcmp(section,"ScintillatorPhotodiode.Zmotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.scint",stat_no);
      sprintf(maatel_mot_name,"ScintillatorPhotodiode.Zmotor");
   }
   else if (strcmp(section,"CoaxZoom.ZoomMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.cam.zoom",stat_no);
      sprintf(maatel_mot_name,"CoaxZoom.ZoomMotor");
   }
   else if (strcmp(section,"PHIRotationAxis.PHIMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.omega",stat_no);
      sprintf(maatel_mot_name,"PHIRotationAxis.PHIMotor");
   }
   else if (strcmp(section,"Analyser.AnalyserMotor") == 0) {
      parse_mparms = 1;
      sprintf(redis_mot_name,"mdhard.%d.lightPolar",stat_no);
      sprintf(maatel_mot_name,"Analyser.AnalyserMotor");
   }

   #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
   if (parse_mparms == 1) {

      gen_redis_record = -1;

      if (MATCH(maatel_mot_name,"CoordinateSystem")) {
         sprintf(hash_field,"CoordinateSystem");
         gen_redis_record = 1;   
      }
      else if (MATCH(maatel_mot_name,"MotorNumber")) {
         sprintf(hash_field,"MotorNumber");
         gen_redis_record = 1;}
      else if (MATCH(maatel_mot_name,"Unit")) {
         sprintf(hash_field,"Unit");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"UnitRatio")) {
         sprintf(hash_field,"UnitRatio");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"MaxSpeedCts")) {
         sprintf(hash_field,"MaxSpeedCts");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"Precision")) {
         sprintf(hash_field,"Precision");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"SmallStep")) {
         sprintf(hash_field,"smallStep");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"LargeStep")) {
         sprintf(hash_field,"largeStep");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"MinPosition")) {
         sprintf(hash_field,"minPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"MaxPosition")) {
         sprintf(hash_field,"maxPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"Status1Addr")) {
         sprintf(hash_field,"Status1Addr");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"Status2Addr")) {
         sprintf(hash_field,"Status2Addr");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"PositionAddr")) {
         sprintf(hash_field,"PositionAddr");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"MotorModeAddr")) {
         sprintf(hash_field,"MotorModeAddr");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"HomeCaptureAddr")) {
         sprintf(hash_field,"HomeCaptureAddr");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_mot_name,"StopSafetyMargin")) {
         sprintf(hash_field,"StopSafetyMargin");
         gen_redis_record = 1;
      }        
      else {
         return 0;
      }

      if (gen_redis_record == 1) {
         sprintf(hash_key,"%s",redis_mot_name);
         sprintf(hash_value,"%s",value);
   
         redis_reply = redisCommand(redis_con,"HSET %s %s %s",hash_key,hash_field,hash_value);

      }
   }

   return 1;
 
}



//handler function (callback) for inih for parsing microdiff_pref.ini
static int microdiff_pref_handler(void* preset_name, const char* section, const char* name, const char* value) {

   int ii,jj,isel;
   int gen_redis_record, len;
   char *redis_fostr;
   char nsubstr[3];
   char redis_preset_section[100];
   char maatel_preset_section[100];
   char hash_key[100];
   char hash_field[100];
   char hash_value[100];
   inih_config* pconfig = (inih_config*) preset_name;

   #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0

   gen_redis_record = -1;

   if (strcmp(section,"ApertureYZTable") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.ApertureYZTable",stat_no);
      sprintf(maatel_preset_section,"ApertureYZTable");
   
      if (MATCH(maatel_preset_section,"BeamHorizontalPosition_Y0")) {
         sprintf(hash_field,"BeamHorizontalPosition_Y0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"OffVerticalPosition_Z0")) {
         sprintf(hash_field,"OffVerticalPosition_Z0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"BeamVerticalPosition_Z1")) {
         sprintf(hash_field,"BeamVerticalPosition_Z1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"HorizontalScanRange_YR")) {
         sprintf(hash_field,"HorizontalScanRange_YR");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"HorizontalScanStep_YS")) {
         sprintf(hash_field,"HorizontalScanStep_YS");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanRange_ZR")) {
         sprintf(hash_field,"VerticalScanRange_ZR");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanStep_ZS")) {
         sprintf(hash_field,"VerticalScanStep_ZS");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalOffScreenPosition_Z2")) {
         sprintf(hash_field,"VerticalOffScreenPosition_Z2");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"HideApertureForLoading")) {
         sprintf(hash_field,"HideApertureForLoading");
         gen_redis_record = 1;
      }
      else {
      }
      
   }

   else if (strcmp(section,"CapillaryBSYZtable") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.CapillaryBSYZtable",stat_no);
      sprintf(maatel_preset_section,"CapillaryBSYZtable");

      if (MATCH(maatel_preset_section,"HorizontalBeamPosition_Y0")) {
         sprintf(hash_field,"HorizontalBeamPosition_Y0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalOffPosition_Z0")) {
         sprintf(hash_field,"VerticalOffPosition_Z0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalBeamPosition_Z1")) {
         sprintf(hash_field,"VerticalBeamPosition_Z1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"HorizontalScanRange_ZR")) {
         sprintf(hash_field,"HorizontalScanRange_ZR");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"HorizontalScanStep_ZS")) {
         sprintf(hash_field,"HorizontalScanStep_ZS");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanRange_ZR")) {
         sprintf(hash_field,"VerticalScanRange_ZR");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanStep_ZS")) {
         sprintf(hash_field,"VerticalScanStep_ZS");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalOffScreenPosition_Z2")) {
         sprintf(hash_field,"VerticalOffScreenPosition_Z2");
         gen_redis_record = 1;
      }
      else {
      }

   }

   else if (strcmp(section,"CentringXYTable") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.CentringXYTable",stat_no);
      sprintf(maatel_preset_section,"CentringXYTable");

      if (MATCH(maatel_preset_section,"PhiReference")) {
         sprintf(hash_field,"PhiReference");
         gen_redis_record = 1;
         phiRef = atof(value);
      }
      else if (MATCH(maatel_preset_section,"CentringDevDZ")) {
         sprintf(hash_field,"CentringDevDZ");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strstr(section,"CoaxCam.Zoom") != NULL ) {
      jj = 0;
      len = strlen(section);
      for (ii=12;ii<len;ii++) {
         nsubstr[jj] = section[ii];
         jj++;
      }
      nsubstr[jj] = 0;

      sprintf(redis_preset_section,"mdpref.%d.Presets.CoaxCam.Zoom%s",stat_no,nsubstr);
      sprintf(maatel_preset_section,"CoaxCam.Zoom%s",nsubstr);

      if (MATCH(maatel_preset_section,"MotorPosition")) {
         sprintf(hash_field,"MotorPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ScaleX")) {
         sprintf(hash_field,"ScaleX");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ScaleY")) {
         sprintf(hash_field,"ScaleY");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"LightIntensity")) {
         sprintf(hash_field,"LightIntensity");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"FrontLightIntensity")) {
         sprintf(hash_field,"FrontLightIntensity");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strcmp(section,"CoaxCam") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.CoaxCam",stat_no);
      sprintf(maatel_preset_section,"CoacCam");

      if (MATCH(maatel_preset_section,"XFocus")) {
         sprintf(hash_field,"XFocus");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YFocus")) {
         sprintf(hash_field,"YFocus");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strcmp(section,"PHIAxisXYZTable") == 0) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.PHIAxisXYZTable",stat_no);
      sprintf(maatel_preset_section,"PHIAxisXYZTable");

      if (MATCH(maatel_preset_section,"XDefaultCentring_X0")) {
         sprintf(hash_field,"XDefaultCentring__X0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"XBeam_X1")) {
         sprintf(hash_field,"XBeam_X1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"XScintillatorOut_X2")) {
         sprintf(hash_field,"XScintillatorOut_X2");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YDefaultCentring_Y0")) {
         sprintf(hash_field,"YDefaultCentring_Y0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YBeam_Y1")) {
         sprintf(hash_field,"YBeam_Y1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YScintillatorOut_Y2")) {
         sprintf(hash_field,"YScintillatorOut_Y2");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YBack_Y3")) {
         sprintf(hash_field,"YBack_Y3");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YFront_Y4")) {
         sprintf(hash_field,"YFront_Y4");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZDefaultCentring_Z0")) {
         sprintf(hash_field,"ZDefaultCentring_Z0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZBeam_Z1")) {
         sprintf(hash_field,"ZBeam_Z1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZScintillatorOut_Z2")) {
         sprintf(hash_field,"ZScintillatorOut_Z2");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"SampleHolderLength")) {
         sprintf(hash_field,"SampleHolderLength");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"SampleHolderLengthRef")) {
         sprintf(hash_field,"SampleHolderLengthRef");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YminiKappaDefaultCentring_Y7")) {
         sprintf(hash_field,"YminiKappaDefaultCentring_Y7");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZOnOpticCentre")) {
         sprintf(hash_field,"ZOnOpticCentre");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"XMountPositionSC")) {
         sprintf(hash_field,"XMountPositoinSC");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZMountPositionSC")) {
         sprintf(hash_field,"ZMountPositionSC");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"XMountPostionKappaSC")) {
         sprintf(hash_field,"XMountPositionKappaSC");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ZMountPositionKappaSC")) {
         sprintf(hash_field,"ZMountPositionKappaSC");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"YSampledCentered")) {
         sprintf(hash_field,"YSampleCentered");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strcmp(section,"PHIRotationAxis") == 0) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.PHIRotationAxis",stat_no);
      sprintf(maatel_preset_section,"PHIRotationAxis");

      if (MATCH(maatel_preset_section,"ScanStartAngle")) {
         sprintf(hash_field,"ScanStartAngle");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ScanAngle")) {
         sprintf(hash_field,"ScanAngle");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ExposureTime")) {
         sprintf(hash_field,"ExposureTime");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"RisingDelay")) {
         sprintf(hash_field,"RisingDelay");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"FallingDelay")) {
         sprintf(hash_field,"FallingDelay");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"NumberOfPasses")) {
         sprintf(hash_field,"NumberOfPasses");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"StandardGonioMountPosition")) {
         sprintf(hash_field,"StandardGonioMountPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"KappaMountPosition")) {
         sprintf(hash_field,"KappaMountPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"Anticipation")) {
         sprintf(hash_field,"Anticipation");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strcmp(section,"ScintillatorPhotodiode") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.ScintillatorPhotodiode",stat_no);
      sprintf(maatel_preset_section,"ScintillatorPhotodiode");

      if (MATCH(maatel_preset_section,"OffVerticalPosition_Z0")) {
         sprintf(hash_field,"OffVerticalPostion_Z0");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ScintiOnBeamVerticalPosition_Z1")) {
         sprintf(hash_field,"ScintiOnBeamVerticalPosition_Z1");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"DiodeOnBeamVerticalPosition_Z2")) {
         sprintf(hash_field,"DiodeOnBeamVerticalPosition_Z2");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"OnFocusPiezoPosition")) {
         sprintf(hash_field,"OnFocusPiezoPosition");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanRange_ZR")) {
         sprintf(hash_field,"VerticalScanRange_ZR");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"VerticalScanStep_ZS")) {
         sprintf(hash_field,"VerticalScanStep_ZS");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"Gain")) {
         sprintf(hash_field,"Gain");
         gen_redis_record = 1;
      }
      else {
      }
   }

   else if (strcmp(section,"JAICamera") == 0 ) {

      sprintf(redis_preset_section,"mdpref.%d.Presets.JAICamera",stat_no);
      sprintf(maatel_preset_section,"JAICamera");

      if (MATCH(maatel_preset_section,"GainCrystalVisualization")) {
         sprintf(hash_field,"GainCrystalvisualization");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"GainBeamVisualization")) {
         sprintf(hash_field,"GainBeamVisualization");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"GainUVVisualization")) {
         sprintf(hash_field,"GainUVVisualization");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"IntegrationCrystalVisualization")) {
         sprintf(hash_field,"IntegrationCrystalVisualization");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"IntegrationBeamVisualization")) {
         sprintf(hash_field,"IntegrationBeamVisualization");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"Gamma")) {
         sprintf(hash_field,"Gamma");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"StartupWhiteBalanceMode")) {
         sprintf(hash_field,"StartupWhiteBalanceMode");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"StartupWhiteBalanceLevel")) {
         sprintf(hash_field,"StartupWhiteBalanceLevel");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ShutterSpeed")) {
         sprintf(hash_field,"ShutterSpeed");
         gen_redis_record = 1;
      }
      else if (MATCH(maatel_preset_section,"ShutterSpeedUV")) {
         sprintf(hash_field,"ShutterSPeedUV");
         gen_redis_record = 1;
      }

   }


   else {
      return 0;
   }

   
   if (gen_redis_record == 1) {

      sprintf(hash_key,"%s",redis_preset_section);
      sprintf(hash_value,"%s",value);

      redis_reply = redisCommand(redis_con,"HSET %s %s %s",hash_key,hash_field,hash_value);
   }

   return 1;

}


int parse_microdiff_hard(const char * ini_fname, int station_no) {
   inih_config iniconf;
   printf("ini file:%s\n",ini_fname);
   stat_no = station_no;
   if (ini_parse(ini_fname, microdiff_hard_handler, &iniconf) > 0) {
      return 1;
   }
   else {
      return -1;
   }
   
}


int parse_microdiff_pref(const char * ini_fname, int station_no) {
   inih_config iniconf;
   printf("ini file:%s\n", ini_fname);
   stat_no = station_no;
   if (ini_parse(ini_fname, microdiff_pref_handler, &iniconf) > 0) {
      return 1;
   }
   else {
      return -1;
   }
   
}


// parse keith's edited kvpair dump file ... generate corresponding redis hash records
// last record in pgpmac.conf specifies full path to dump file
// each kvp generates a redis hash record with fields "value" and "pair".
// key string same identical to name in kvp dump file
int parse_kv_pairs(char* kvpair_file) {
   FILE *kvp_file;
   char in_line[200];
   char hash_key[100];
   char value[100];
   char type[100];
   
   if (NULL != (kvp_file = fopen(kvpair_file, "r"))) {
      while (fgets(in_line, 200, kvp_file)){ 
         sscanf(in_line,"%s%s%s",&hash_key,&value,&type);
         redis_reply = redisCommand(redis_con,"HMSET %s value %s type %s",hash_key,value,type);
      }
     fclose(kvp_file);

     return(1);
   }

   else {
     return(-1);
   }
}


  // recover allrecords specified by key_spec using the "KEYS key_spec*" redis cmd
  // write out all found keys to tmp file....read kesy from tmp file
  // and issue "HVALS" redis cmd on all keys
  // write out new kvp file containing updated values
  int save_kvpair_vals (char *new_kvpair_file, char * key_spec) {
     int ii,jj;
     char hash_str[100];
     char in_line[200];
     
     if (NULL != (kvp_file = fopen("./hash_keys.tmp", "w+"))) {
        redis_reply = redisCommand(redis_con, "keys %s*", key_spec);
        no_elements = redis_reply->elements;
        printf("No. elements in reply:%lu\n",redis_reply->elements);
        if (redis_reply->type == REDIS_REPLY_ERROR)
           printf("Error: %s\n",redis_reply->str);
        else if (redis_reply->type != REDIS_REPLY_ARRAY)
           printf("Unexpected type: %d\n", redis_reply->type);
        else {
           for (ii=0; ii<no_elements; ++ii) {
               //printf("Result:%lu: %s\n", ii, redis_reply->element[ii]->str);
               fprintf(kvp_file,"%s\n",redis_reply->element[ii]->str);
           }
        }
        fclose(kvp_file);
     }
     else {
        printf("can't open temp keys list file\n");
     }
  
     //read back (from redis) & write out new values using the keys list 
     
     if (NULL != (kvp_file = fopen(new_kvpair_file, "w+"))) {
        hl_file = fopen("./hash_keys.tmp", "r");
        for (ii=0; ii<no_elements; ii++) {
            fgets(in_line, 200, hl_file);
            sscanf(in_line,"%s",&hash_str);
            redis_reply = redisCommand(redis_con, "hvals %s", hash_str);
            fprintf(kvp_file,"%s ",hash_str);
            for (jj=0; jj<redis_reply->elements; ++jj) {
               fprintf(kvp_file,"%s ",redis_reply->element[jj]->str);
            }
            fprintf(kvp_file,"\n");
        }
        fclose(hl_file);
        fclose(kvp_file);
     }
     else {
        printf("can't open new kvp file\n");
        sleep(5);
     }

   }
