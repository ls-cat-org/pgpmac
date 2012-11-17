DROP SCHEMA IF EXISTS pmac CASCADE;
CREATE SCHEMA pmac;
GRANT USAGE ON SCHEMA pmac TO PUBLIC;

BEGIN;

CREATE TABLE pmac.md2_registration (
       mr_key serial primary key,                       -- our key
       mr_stn int,                                      -- our station
       mr_ts timestamptz default now(),                 -- time we logged in
       mr_client inet default inet_client_addr(),       -- our ip address
       mr_kvseq int default 0                           -- the most recent kvseq for kv pairs sent to md2 process
);
ALTER TABLE pmac.md2_registration OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.getkvs( the_stn int) returns setof text AS $$
  DECLARE
    the_mrkey int;
    the_seq  int;
    new_seq  int;
    the_ntfy text;
    the_ntfya text[];
    the_k    text;
    the_v    text;

  BEGIN
    SELECT INTO the_mrkey, the_seq mr_key,mr_kvseq FROM pmac.md2_registration WHERE mr_stn=the_stn ORDER BY mr_key DESC LIMIT 1;
    IF NOT FOUND THEN
      return;
    END IF;

    SELECT INTO the_ntfy cnotifykvs FROM px._config WHERE cstnkey = the_stn;
    IF NOT FOUND THEN
      return;
    END IF;

    the_ntfya = ('{' || the_ntfy || '}')::text[];

    FOR new_seq, the_k, the_v IN SELECT kvseq, kvname, kvvalue FROM px.kvs WHERE kvseq > the_seq and kvnotify @> the_ntfya order by kvseq LOOP
      return next the_k;
      return next the_v;
    END LOOP;

    IF new_seq is not null and new_seq > the_seq THEN
      UPDATE pmac.md2_registration SET mr_kvseq = new_seq WHERE mr_key = the_mrkey;
    END IF;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.getkvs( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.getkvs() returns setof text AS $$
  SELECT pmac.getkvs( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.getkvs() OWNER TO lsadmin;


CREATE TABLE pmac.md2_mvars (
       mv_key serial primary key,
       mv_ts timestamptz default now(),
       mv_stn int not null,
       mv_num int not null,
       mv_def text not null
);

ALTER TABLE pmac.md2_mvars OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_mvar_set( the_stn int, num int, def text) returns void as $$
  DECLARE
    old_def text;
  BEGIN
    SELECT INTO old_def mv_def FROM pmac.md2_mvars WHERE mv_stn=the_stn and mv_num=num;
    IF FOUND THEN
      IF old_def != def THEN
        UPDATE pmac.md2_mvars SET mv_def=def, mv_ts=now() WHERE mv_stn=the_stn and mv_num=num;
      END IF;
    ELSE
      INSERT INTO pmac.md2_mvars (mv_stn, mv_num, mv_def) VALUES (the_stn, num, def);
    END IF;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_mvar_set( int, int, text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_mvar_set( int, text) returns void as $$
  SELECT pmac.md2_mvar_set( px.getstation(), $1, $2);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_mvar_set( int, text) OWNER TO lsadmin;

CREATE TABLE pmac.md2_ivars (
       iv_key serial primary key,
       iv_ts timestamptz default now(),
       iv_stn int not null,
       iv_num int not null,
       iv_val text not null
);

ALTER TABLE pmac.md2_ivars OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_ivar_set( the_stn int, num int, val text) returns void as $$
  DECLARE
    old_val text;
  BEGIN
    SELECT INTO old_val iv_val FROM pmac.md2_ivars WHERE iv_stn=the_stn and iv_num=num;
    IF FOUND THEN
      IF old_val != val THEN
        UPDATE pmac.md2_ivars SET iv_val=val, iv_ts=now() WHERE iv_stn=the_stn and iv_num=num;
      END IF;
    ELSE
      INSERT INTO pmac.md2_ivars (iv_stn, iv_num, iv_val) VALUES (the_stn, num, val);
    END IF;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_ivar_set( int, int, text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_ivar_set( int, text) returns void as $$
  SELECT pmac.md2_ivar_set( px.getstation(), $1, $2);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_ivar_set( int, text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.array_pop(a anyarray, element text) RETURNS anyarray AS $$
  DECLARE 
    result a%TYPE;
  BEGIN
  SELECT ARRAY(
    SELECT b.e FROM (SELECT unnest(a)) AS b(e) WHERE b.e <> element) INTO result;
  RETURN result;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.array_pop( anyarray, text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_init( the_stn int) returns void as $$
  --
  -- initialize a new connection with the md2 diffractometer
  --
  DECLARE
    ntfy_pmac text;
    ntfy_diff text;
    ntfy_kvs  text;
    ntfy_kvsa  text[];
    
    motor record;
    minit text;
  BEGIN

    --
    -- Listen for the correct notifies
    --
    SELECT INTO ntfy_pmac,ntfy_diff,ntfy_kvs cnotifypmac, cnotifydiffractometer, cnotifykvs FROM px._config WHERE cstnkey=the_stn;
    IF NOT FOUND THEN
      RAISE EXCEPTION 'Cannot find station %', the_stn;
    END IF;
    EXECUTE 'LISTEN ' || ntfy_pmac;     -- A raw PMAC command is in the queue
    EXECUTE 'LISTEN ' || ntfy_diff;     -- A diffractometer command awaits
    EXECUTE 'LISTEN ' || ntfy_kvs;      -- Learn of changed KV's
    EXECUTE 'NOTIFY ' || ntfy_kvs;      -- Immediately call this notify so the md2 can update its kv list


    
    --
    -- Add them back in
    --
    ntfy_kvsa := ('{' || ntfy_kvs || '}')::text[];
    --
    -- Remove our previous notifies on the off chance we changed which ones we are interested in
    --
    UPDATE px.kvs SET kvnotify = pmac.array_pop( kvnotify, ntfy_kvs) WHERE kvnotify @> ntfy_kvsa;

    --
    -- Now add the ones we want back in
    --
    UPDATE px.kvs SET kvnotify = kvnotify || ntfy_kvsa WHERE kvname ~ (E'stns\\.' || the_stn || E'\\..*\\.presets\\.[0-9]+\\.(name|position)');


    -- Log the fact that we are connecting
    --
    INSERT INTO pmac.md2_registration (mr_stn) values (the_stn);

    PERFORM px.ininotifies( the_stn);


    -- Remove all the old information from the database queue
    --
    DELETE FROM pmac.md2_queue WHERE mq_stn=the_stn;

    --
    -- I5=3 allows foreground (plcc 0) and background (all others)
    -- PLCC 1 fills DPRAM for EMBL VB code (unused here)
    -- PLCC 2 fills DPRAM for us
    --
    PERFORM pmac.md2_queue_push( the_stn, 'I5=3');
    PERFORM pmac.md2_queue_push( the_stn, 'ENABLE PLCC 0');
    PERFORM pmac.md2_queue_push( the_stn, 'DISABLE PLCC 1');
    PERFORM pmac.md2_queue_push( the_stn, 'ENABLE PLCC 2');


    --
    -- Run the initialzation code for each of the motors
    --
    FOR motor IN SELECT * FROM pmac.md2_motors ORDER BY mm_motor LOOP
      IF motor.mm_active THEN
        FOR minit IN SELECT unnest( motor.mm_active_init) LOOP
          PERFORM pmac.md2_queue_push( the_stn, minit);
        END LOOP;
      ELSE
        FOR minit IN SELECT unnest( motor.mm_inactive_init) LOOP
          PERFORM pmac.md2_queue_push( the_stn, minit);
        END LOOP;
      END IF;
    END LOOP;

  --
  -- Set flag for PLCC 0 to initialize (or reset) various motor settings
  --
  PERFORM pmac.md2_queue_push( the_stn, 'M2000=1');

  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_init( int) OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_init() returns void as $$
  SELECT pmac.md2_init( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_init() OWNER TO lsadmin;


CREATE TABLE pmac.md2_queue (
       mq_key serial primary key,
       mq_stn int NOT NULL,
       mq_on_ts  timestamptz default now(),
       mq_off_ts timestamptz default NULL,
       mq_cmd text NOT NULL
);
ALTER TABLE pmac.md2_queue OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_queue_push( the_stn int, the_cmd text) returns void as $$
  DECLARE
    ntfy text;
  BEGIN
    INSERT INTO pmac.md2_queue (mq_stn, mq_cmd) VALUES (the_stn, the_cmd);
    SELECT INTO ntfy cnotifypmac FROM px._config WHERE cstnkey=the_stn;
    EXECUTE 'NOTIFY ' || ntfy;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_queue_push( int, text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_queue_push( the_cmd text) returns void as $$
  SELECT pmac.md2_queue_push( px.getstation(), $1);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_queue_push( text) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_queue_next( the_stn int) returns text as $$
  DECLARE
    rtn text;
    the_key int;
  BEGIN
    SELECT INTO rtn, the_key  mq_cmd, mq_key FROM pmac.md2_queue WHERE mq_stn = the_stn and mq_off_ts is NULL ORDER BY mq_key LIMIT 1;
    IF FOUND THEN
      UPDATE pmac.md2_queue SET mq_off_ts=now() WHERE mq_key=the_key;
      return rtn;
    END IF;
    return NULL;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_queue_next( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_queue_next() returns text as $$
  SELECT pmac.md2_queue_next( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_queue_next() OWNER TO lsadmin;

CREATE TABLE pmac.md2_motor_types (
       mt text not null primary key
);
ALTER TABLE pmac.md2_motor_types OWNER TO lsadmin;

INSERT INTO pmac.md2_motor_types (mt) values ('PMAC');
INSERT INTO pmac.md2_motor_types (mt) values ('DAC');
INSERT INTO pmac.md2_motor_types (mt) values ('BIO');


CREATE TABLE pmac.md2_motors (
       mm_key serial primary key,
       mm_stn int,                      -- the station
       mm_name text,                    -- name of motor
       mm_type text not null
               references pmac.md2_motor_types (mt),
       mm_active boolean default TRUE,  -- 1 if active, 0 if simulated
       mm_active_init text[],           -- PMAC commands when motor is active
       mm_inactive_init text[],         -- PMAC commands when motor is inactive
       mm_home text[],                  -- PMAC commands to activate and home motor
       mm_motor int default -1,         -- motor number
       mm_coord int default 0,          -- coordinate system number
       mm_unit text,                    -- name of unit
       mm_u2c float,                    -- Conversion between encoder counts and units
       mm_max_speed float,              -- maximum speed (Ixx16) in counts/msec
       mm_max_accel float,              -- maximum acceleration (Ixx17) in counts/msec/msec
       mm_printf text,                  -- String for printf type conversions
       mm_min float,                    -- minimum position 
       mm_max float,                    -- maximum position
       mm_update_resolution float,      -- minimum magnetude of position change to trigger px.kvs update
       mm_update_format text            -- generate string for lsupdates to update px.kvs
);
ALTER TABLE pmac.md2_motors OWNER TO lsadmin;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  1,        1,        'omega',  'deg',   12800.0,  1664.0,       2.0,          '%*.4f°',   '-Infinity', 'Infinity', 0.001,               '"omega.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M31=1", "&1#1->X", "M700=(M700 | $000001) ^ $000001", "M1115=1"}' WHERE mm_motor=1;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M31=0", "&1#1->0", "M700=M700 | $000001", "M1115=0"}' WHERE mm_motor=1;
UPDATE pmac.md2_motors SET mm_home          = '{"M401=1 M1115=1 #1$","&1E","#1&1B1R"}' WHERE mm_motor=1;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  2,        3,        'align.x', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.01,         4.0,    0.001,                '"align.x.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M32=1", "&3#2->X", "M700=(M700 | $000002) ^ $000002"}' WHERE mm_motor=2;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M32=0", "&3#2->0", "M700=M700 | $000002"}' WHERE mm_motor=2;
UPDATE pmac.md2_motors SET mm_home          = '{"#2$","M402=1","&3E","#2&3B2R"}' WHERE mm_motor=2;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  3,        3,        'align.y', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.16,         16.15,  0.001,                '"align.y.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M33=1", "&3#3->Y", "M700=(M700 | $000004) ^ $000004"}' WHERE mm_motor=3;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M33=0", "&3#3->0", "M700=M700 | $000004"}' WHERE mm_motor=3;
UPDATE pmac.md2_motors SET mm_home          = '{"#3$","M403=1","&3E","#3&3B3R"}' WHERE mm_motor=3;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  4,        3,        'align.z', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.45,         5.85,   0.001,                '"align.z.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M34=1", "&3#4->Z", "M700=(M700 | $000008) ^ $000008"}' WHERE mm_motor=4;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M34=0", "&3#4->0", "M700=M700 | $000008"}' WHERE mm_motor=4;
UPDATE pmac.md2_motors SET mm_home          = '{"#4$","M404=1","&3E","#4&3B4R"}' WHERE mm_motor=4;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,   mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  5,        0,        'lightPolar',   'deg',   142.0,    3.0,          0.2,          '%*.0f°',   '-Infinity', 'Infinity', 1.0,                  '"lightPolar.position",%.1f');

UPDATE pmac.md2_motors SET mm_home          = '{#5$,#5HMZ}' WHERE mm_motor=5;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  6,        4,        'zoom',   'X mag',   1.0,      10.0,         0.2,          '%*.0fX Mag',   0.0,      10,       0.5,                   '"zoom.position",%.0f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M36=1", "&4#6->Z", "M700=(M700 | $000020) ^ $000020"}' WHERE mm_motor=6;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M36=0", "&4#6->0", "M700=M700 | $000020"}' WHERE mm_motor=6;
UPDATE pmac.md2_motors SET mm_home          = '{"#6$","M406=1","&4E","#6&4B6R"}' WHERE mm_motor=6;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  7,        5,        'appy',  'mm',    121241.6, 201.0,        1.0,          '%*.3f mm',   0.2,         3.25,    0.001,                '"appy.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M37=1", "&5#7->Y", "M700=(M700 | $000040) ^ $000040"}' WHERE mm_motor=7;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M37=0", "&5#7->0", "M700=M700 | $000040"}' WHERE mm_motor=7;
UPDATE pmac.md2_motors SET mm_home          = '{"#7$","M407=1","&5E","#7&5B7R"}' WHERE mm_motor=7;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  8,        5,        'appz',  'mm',    60620.8,  201.0,        1.0,          '%*.3f mm',   0.3,         82.5,    0.001,                '"appz.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M38=1", "&5#8->Z", "M700=(M700 | $000080) ^ $000080"}' WHERE mm_motor=8;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M38=0", "&5#8->0", "M700=M700 | $000080"}' WHERE mm_motor=8;
UPDATE pmac.md2_motors SET mm_home          = '{"#8$","M408=1","&5E","#8&5B8R"}' WHERE mm_motor=8;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  9,        5,        'capy',   'mm',    121241.6, 201.0,        1.0,          '%*.3f mm',   0.05,        3.19,    0.001,                '"capy.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M39=1", "&5#9->U", "M700=(M700 | $000100) ^ $000100"}' WHERE mm_motor=9;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M39=0", "&5#9->0", "M700=M700 | $000100"}' WHERE mm_motor=9;
UPDATE pmac.md2_motors SET mm_home          = '{"#9$","M409=1","&5E","#9&5B9R"}' WHERE mm_motor=9;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  10,       5,        'capz',  'mm',     19865.6,  201.0,        0.5,          '%*.3f mm',   0.57,        81.49,   0.001,                '"capz.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M40=1", "&5#10->V", "M700=(M700 | $000200) ^ $000200"}' WHERE mm_motor=10;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M40=0", "&5#10->0", "M700=M700 | $000200"}' WHERE mm_motor=10;
UPDATE pmac.md2_motors SET mm_home          = '{"#10$","M410=1","&5E","#10&5B10R"}' WHERE mm_motor=10;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  11,       5,        'scint',  'mm',     19865.6,  151.0,        0.5,          '%*.3f mm',   0.02,        86.1,    0.001,                '"scint.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M41=1", "&5#11->W", "M700=(M700 | $000400) ^ $000400"}' WHERE mm_motor=11;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M41=0", "&5#11->0", "M700=M700 | $000400"}' WHERE mm_motor=11;
UPDATE pmac.md2_motors SET mm_home          = '{"#11$","M411=1","&5E","#11&5B11R"}' WHERE mm_motor=11;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  17,       2,        'centering.x',  'mm',     182400.,  150.0,        0.5,          '%*.3f mm',   -2.56,       2.496,   0.001,                '"centering.x.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M47=1", "&2#17->X", "M700=(M700 | $010000) ^ $010000"}' WHERE mm_motor=17;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M47=0", "&2#17->0", "M700=M700 | $010000"}' WHERE mm_motor=17;
UPDATE pmac.md2_motors SET mm_home          = '{"#17$","M417=1","&2E","#17&2B17R"}' WHERE mm_motor = 17;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  18,       2,        'centering.y',  'mm',     182400.,  150.0,        0.5,          '%*.3f mm',   -2.58,       2.4,     0.001,                '"centering.y.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M48=1", "&2#18->Y", "M700=(M700 | $020000) ^ $020000"}' WHERE mm_motor=18;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M48=0", "&2#18->0", "M700=M700 | $020000"}' WHERE mm_motor=18;
UPDATE pmac.md2_motors SET mm_home          = '{"#18$","M418=1","&2E","#18&2B18R"}' WHERE mm_motor=18;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max,  mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  19,       7,        'kappa', 'deg',    2844.444, 50.0,         0.2,          '%*.2f°',     -5.0,        248.0,   0.1,                  '"kappa.position",%.3f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M49=1", "&7#19->X", "M700=(M700 | $040000) ^ $040000"}' WHERE mm_motor=19;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M49=0", "&7#19->0", "M700=M700 | $040000"}' WHERE mm_motor=19;
UPDATE pmac.md2_motors SET mm_home          = '{"#19$","M419=1","&7E","#19&7B119R"}' WHERE mm_motor=19;

INSERT INTO pmac.md2_motors (mm_stn, mm_type, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,   mm_min,      mm_max,      mm_update_resolution, mm_update_format) VALUES
                            ( 2,     'PMAC',  20,       7,        'phi',   'deg',    711.111,  50,           0.2,          '%*.2f°',    '-Infinity', 'Infinity',  1.0,                  '"phi.position",%.2f');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M50=1", "&7#20->Y", "M700=(M700 | $080000) ^ $080000"}' WHERE mm_motor=20;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M50=0", "&7#20->0", "M700=M700 | $080000"}' WHERE mm_motor=20;
UPDATE pmac.md2_motors SET mm_home          = '{"#20$","M420=1","&7E","#20&7B20R"}' WHERE mm_motor=20;

INSERT INTO pmac.md2_motors ( mm_stn, mm_type, mm_name,       mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                            ( 2,      'BIO',  'fastShutter', 1.0,    0,      1,      0.5,                  '"fastShutter.position",%.0f');


INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,                mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'DAC',   'frontLight.intensity', 1,      0,      10,      0.5,                  '"frontLight.intensity",%.0f');

INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,                mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'DAC',   'backLight.intensity', 1,      0,      10,      0.5,                  '"backLight.intensity",%.0f');

INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,                mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'DAC',   'scint.focus', 1,      0,      100,      0.5,                  '"scint.focus.position",%.0f');

INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,        mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'BIO',   'cryo',         1.0,    0,      1,      0.5,                  '"cryo.position",%.0f');

INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,        mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'BIO',   'backLight',         1.0,    0,      1,      0.5,                  '"backLight.position",%.0f');

INSERT INTO pmac.md2_motors( mm_stn, mm_type, mm_name,        mm_u2c, mm_min, mm_max, mm_update_resolution, mm_update_format) VALUES
                           ( 2,      'BIO',   'dryer',         1.0,    0,      1,      0.5,                  '"dryer.position",%.0f');


CREATE OR REPLACE FUNCTION pmac.md2_getmotors( the_stn int) returns setof pmac.md2_motors AS $$
  SELECT * FROM pmac.md2_motors WHERE mm_stn=$1 order by mm_motor;
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_getmotors( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_getmotors( ) returns setof pmac.md2_motors AS $$
  SELECT * FROM pmac.md2_getmotors( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_getmotors( ) OWNER TO lsadmin;




CREATE OR REPLACE FUNCTION pmac.md2_home_omega( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 1: home omega
    --
    --    M401=1    flag indicating program is running
    --    M1115=1   Enable the Etel amplifier
    --    &1        Omega is in coordinate system 1
    --    E         Enable omega
    --    B1        Run program 1 from the beginning
    --    R         GO!
    --
    -- For some reason the pmac thinks the B command is part of a motion program if it's on the same line as the E command
    --
    PERFORM pmac.md2_queue_push( the_stn, 'M401=1 M1115=1 #1$ &1B1R');

    
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_omega( int) OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_home_centers( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 53: centerx and centery homing
    --
    --  M453=1          flag indicating program is running
    --  &2              The centering stages are in coordinate system 2
    --  E               Enable centerx and centery
    --  B53             Run program 53 from the beginning
    --  R               GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M453=1&2E'); --
    PERFORM pmac.md2_queue_push( the_stn, '&2B53R');    -- make sure another coordinate system didn't sneak in
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_centers( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_alignment_x( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 2: alignment x homing
    --
    -- M402=1    flag indicating program is running
    -- &3        The alignment stages are in coordinate system 3
    -- E         Enable the alignment stages
    -- B2        Run program 2 from the beginning
    -- R         GO!
    --

    PERFORM pmac.md2_queue_push( the_stn, 'M402=1 &3E');
    PERFORM pmac.md2_queue_push( the_stn, '&3B2R');

  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_alignment_x( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_alignment_y( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 3: alignment y homing
    --
    --  M403=1    flag indicating program is running
    --  &3        The alignment stages are in coordinate system 3
    --  E         Enable the alignment stages
    --  B3        Run program 3 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M403=1&3E');
    PERFORM pmac.md2_queue_push( the_stn, '&3B3R');

  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_alignment_y( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_alignment_z( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 4: alignment z homing
    --
    -- M404=1    flag indicating program is running
    -- &3        The alignment stages are in coordinate system 3
    -- E         Enable the alignment stages
    -- B4        Run program 4 from the beginning
    -- R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M404=1&3E');
    PERFORM pmac.md2_queue_push( the_stn, '&3B4R');

  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_alignment_z( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_zoom( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 6: camera zoom
    --
    --  M406=1    flag indicating program is running
    --  &4        The zoom is in coordinate system 4
    --  E         Enable the alignment stages
    --  B6        Run program 6 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M406=1&4E');
    PERFORM pmac.md2_queue_push( the_stn, '&4B6R');
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_zoom( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_organs( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 35: home organs
    --
    --  M435=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B35       Run program 35 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M435=1&5E');
    PERFORM pmac.md2_queue_push( the_stn, '&5B35R');
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_organs( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_aperture_y( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 7: home aperture y
    --
    --  M407=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B7        Run program 7 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M407=1&5E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&5B7R');        -- The organs are in coordinate system 5
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_aperture_y( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_aperture_z( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 8: home aperture z
    --
    --  M408=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B8        Run program 8 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M408=1&5E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&5B8R');        -- The organs are in coordinate system 5
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_aperture_z( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_capillary_y( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 9: home capillary y
    --
    --  M409=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B9        Run program 9 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M409=1&5E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&5B9R');        -- The organs are in coordinate system 5
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_capillary_y( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_capillary_z( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 10: home capillary z
    --
    --  M410=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B10       Run program 10 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M410=1&5E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&5B10R');        -- The organs are in coordinate system 5
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_capillary_z( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_scintillator_z( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 11: home scintillator z
    --
    --  M411=1    flag indicating program is running
    --  &5        The organs are in coordinate system 5
    --  E         Enable the organs
    --  B11       Run program 11 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M411=1&5E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&5B11R');        -- The organs are in coordinate system 5
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_scintillator_z( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_center_x( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 17: home center x
    --
    --  M417=1    flag indicating program is running
    --  &2        The centers are in coordinate system 2
    --  E         Enable the centerx and centery
    --  B17       Run program 17 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M417=1&2E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&2B17R');        -- The centers are in coordinate system 2
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_center_x( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_center_y( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 18: home center y
    --
    --  M418=1    flag indicating program is running
    --  &2        The centers are in coordinate system 2
    --  E         Enable centerx and centery
    --  B18       Run program 18 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M418=1&2E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&2B18R');        -- The centers are in coordinate system 2
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_center_y( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_kappa( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 19: home kappa
    --
    --  M419=1    flag indicating program is running
    --  &7        The kappa and phi are in coordinate system 7
    --  E         Enable kappa and phi
    --  B19       Run program 19 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M419=1&7E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&7B19R');        -- The kappa and phi are in coordinate system 7
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_kappa( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_home_phi( the_stn int) returns void as $$
  DECLARE
  BEGIN
    --
    -- Run pmac program 20: home phi
    --
    --  M420=1    flag indicating program is running
    --  &7        The kappa and phi are in coordinate system 7
    --  E         Enable kappa and phi
    --  B20       Run program 20 from the beginning
    --  R         GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M420=1&7E');    -- flag indicating program is running
    PERFORM pmac.md2_queue_push( the_stn, '&7B20R');        -- The kappa and phi are in coordinate system 7
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_phi( int) OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_home_all( the_stn int) returns void as $$
  DECLARE
  BEGIN
    PERFORM pmac.md2_home_omega( the_stn);
    PERFORM pmac.md2_home_centers( the_stn);
    PERFORM pmac.md2_home_alignment_x( the_stn);
    PERFORM pmac.md2_home_alignment_y( the_stn);
    PERFORM pmac.md2_home_zoom( the_stn);
    PERFORM pmac.md2_home_aperture_y( the_stn);
    PERFORM pmac.md2_home_aperture_z( the_stn);
    PERFORM pmac.md2_home_capillary_y( the_stn);
    PERFORM pmac.md2_home_capillary_z( the_stn);
    PERFORM pmac.md2_home_scintillator_z( the_stn);
    PERFORM pmac.md2_home_kappa( the_stn);
    PERFORM pmac.md2_home_phi( the_stn);
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_home_all( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_moveabs( the_stn int, the_motor text, requested_position float) returns void as $$
  DECLARE
    motor_type           text;
    requested_counts     int;
    low_limit_violation  boolean;
    high_limit_violation boolean;
    motor_number         int;
    motor_coord          int;
  BEGIN
    
    SELECT INTO motor_type, motor_number, motor_coord, requested_counts,                 low_limit_violation,         high_limit_violation
                mm_type,    mm_motor,     mm_coord,    (requested_position*mm_u2c)::int, requested_position < mm_min, requested_position > mm_max
      FROM pmac.md2_motors
      WHERE mm_name=the_motor and mm_stn=the_stn;

    IF motor_type != 'PMAC' THEN
      RAISE NOTICE 'Cannot yet move motor type %', motor_type;
      return;
    END IF;

    IF NOT FOUND THEN
      RAISE EXCEPTION 'Motor % not found', the_motor;
    END IF;

    IF not low_limit_violation and not high_limit_violation THEN
      PERFORM pmac.md2_queue_push( the_stn, '&' || motor_coord || '#' || motor_number || 'J=' || requested_counts);
    END IF;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_moveabs( int, text, float) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_moveabs( the_motor text, requested_position float) returns void as $$
  SELECT pmac.md2_moveabs( px.getstation(), $1, $2);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_moveabs( text, float) OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_scan_omega( the_stn int, start_angle float, delta float, exposure_time float) returns void as $$
  DECLARE
  p170 float;           -- PMAC start angle in counts
  p171 float;           -- PMAC stop angle in counts
  p173 float;           -- PMAC omega velocity (cts/msec)
  p175 float;           -- PMAC acceleration time (msec)
  p180 float;           -- PMAC exposure time (msec)
  BEGIN

   p180 := exposure_time * 1000.0;

    SELECT INTO p170,       p171,        p173,      p175
                mm_u2c * start_angle,
                            mm_u2c * (start_angle+delta),
                                         CASE WHEN abs(p180)<1.e-4 THEN 0.0 ELSE mm_u2c*delta/p180 END,
                                                    CASE WHEN abs(p180)<1.e-4 THEN 0.0 ELSE mm_u2c*delta/p180/mm_max_accel END
       FROM pmac.md2_motors
       WHERE mm_stn=the_stn and mm_motor=1;

    IF NOT FOUND THEN
      RAISE EXCEPTION 'Omega motor not found for station %', the_stn;
    END IF;

    PERFORM pmac.md2_queue_push( the_stn, 'P170='||p170||' P171='||p171||' P173='||p173||' P174=0 P175='||p175||' P176=0 P177=1 P178=0 P179=0 P180='||p180||' M431=1'||' &1B31R');


  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_scan_omega( int, float, float, float) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_scan_omega( start_angle float, delta float, exposure_time float) returns void as $$
  SELECT pmac.md2_scan_omega( px.getstation(), $1, $2, $3);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_scan_omega( float, float, float) OWNER TO lsadmin;



CREATE OR REPLACE FUNCTION pmac.md2_zoom_lut( the_stn int) returns setof text as $$
  DECLARE
    v text;
  BEGIN

  FOR I IN 1..10 LOOP
    v := px.kvget( the_stn, 'cam.zoom.' || I || '.MotorPosition')::int;
    return next I::text;
    return next v;
  END LOOP;
  
  return;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_zoom_lut( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_zoom_lut() returns setof text as $$
  SELECT pmac.md2_zoom_lut( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_zoom_lut() OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_flight_lut( the_stn int) returns setof text as $$
  DECLARE
    v text;
  BEGIN

    return next '0';
    return next '0';
    FOR I IN 1..10 LOOP
      v := (32767 * px.kvget( the_stn, 'cam.zoom.' || I || '.FrontLightIntensity')::float / 100.0)::int;
      return next I::text;
      return next v;
    END LOOP;
  
  return;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_flight_lut( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_flight_lut() returns setof text as $$
  SELECT pmac.md2_flight_lut( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_flight_lut() OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_blight_lut( the_stn int) returns setof text as $$
  DECLARE
    v text;
  BEGIN

  return next '0';
  return next '0';
  FOR I IN 1..10 LOOP
    v := (20000.0 * px.kvget( the_stn, 'cam.zoom.' || I || '.LightIntensity')::float / 100.0)::int;
    return next I::text;
    return next v;
  END LOOP;
  
  return;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_blight_lut( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_blight_lut() returns setof text as $$
  SELECT pmac.md2_blight_lut( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_blight_lut() OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_scint_lut( the_stn int) returns setof text as $$
  DECLARE
    v text;
  
  BEGIN
    return next '0';
    return next '0';
    FOR I IN 1..100 LOOP
      v := (320 * I)::text;
      return next I::text;
      return next v;
    END LOOP;

    return;
  END;
$$ LANGUAGE PLPGSQL SECURITY DEFINER;
alter function pmac.md2_scint_lut( int) OWNER TO lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_scint_lut() returns setof text as $$
  SELECT pmac.md2_scint_lut( px.getstation());
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_scint_lut() OWNER TO lsadmin;


CREATE OR REPLACE FUNCTION pmac.md2_get_presets( the_stn int, the_motor text) returns setof text as $$
  DECLARE
    i int;
    v float;
  BEGIN
    I := 0;
    LOOP
      v := px.kvget( the_stn, the_motor || '.presets.' || i || '.position')::float;
      EXIT WHEN v is null;
      RETURN NEXT v;
      I := I + 1;
    END LOOP;
    RETURN;
  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_get_presets( int, text) OWNER TO  lsadmin;

CREATE OR REPLACE FUNCTION pmac.md2_get_presets( the_motor text) returns setof text as $$
  SELECT pmac.md2_get_presets( px.getstation(), $1);
$$ LANGUAGE SQL SECURITY DEFINER;
ALTER FUNCTION pmac.md2_get_presets( text) OWNER TO lsadmin;

COMMIT;
