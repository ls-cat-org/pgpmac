DROP SCHEMA IF EXISTS pmac CASCADE;
CREATE SCHEMA pmac;
GRANT USAGE ON SCHEMA pmac TO PUBLIC;

BEGIN;

CREATE TABLE pmac.md2_registration (
       mr_key serial primary key,
       mr_stn int,
       mr_ts timestamptz default now(),
       mr_client inet default inet_client_addr()
);
ALTER TABLE pmac.md2_registration OWNER TO lsadmin;


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

CREATE OR REPLACE FUNCTION pmac.md2_init( the_stn int) returns void as $$
  DECLARE
    ntfy text;
    motor record;
    minit text;
  BEGIN
    SELECT INTO ntfy cnotifypmac FROM px._config WHERE cstnkey=the_stn;
    IF NOT FOUND THEN
      RAISE EXCEPTION 'Cannot find station %', the_stn;
    END IF;
    EXECUTE 'LISTEN ' || ntfy;

    INSERT INTO pmac.md2_registration (mr_stn) values (the_stn);

    PERFORM px.ininotifies( the_stn);


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

    -- FOR motor IN SELECT * FROM pmac.md2_motors ORDER BY mm_motor LOOP
    --  IF motor.mm_active THEN
    --    FOR minit IN SELECT unnest( motor.mm_active_init) LOOP
    --      PERFORM pmac.md2_queue_push( the_stn, minit);
    --    END LOOP;
    --  ELSE
    --    FOR minit IN SELECT unnest( motor.mm_inactive_init) LOOP
    --      PERFORM pmac.md2_queue_push( the_stn, minit);
    --    END LOOP;
    --  END IF;
    --END LOOP;

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


CREATE TABLE pmac.md2_motors (
       mm_key serial primary key,
       mm_stn int,                      -- the station
       mm_name text,                    -- name of motor
       mm_active boolean default TRUE,	-- 1 if active, 0 if simulated
       mm_active_init text[],		-- PMAC commands when motor is active
       mm_inactive_init text[],		-- PMAC commands when motor is inactive
       mm_motor int,                    -- motor number
       mm_coord int,                    -- coordinate system number
       mm_unit text,                    -- name of unit
       mm_u2c float,                    -- Conversion between encoder counts and units
       mm_max_speed float,              -- maximum speed (Ixx16) in counts/msec
       mm_max_accel float,              -- maximum acceleration (Ixx17) in counts/msec/msec
       mm_printf text,                  -- String for printf type conversions
       mm_min float,                    -- minimum position 
       mm_max float                     -- maximum position
);
ALTER TABLE pmac.md2_motors OWNER TO lsadmin;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     1,        1,        'omega',  'deg',   12800.0,  1664.0,       2.0,          '%*.4f°',   '-Infinity', 'Infinity');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M31=1", "&1#1->X", "M700=(M700 | $000001) ^ $000001", "M1115=1"}' WHERE mm_motor=1;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M31=0", "&1#1->0", "M700=M700 | $000001", "M1115=0"}' WHERE mm_motor=1;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     2,        3,        'alignx', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.01,         4.0);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M32=1", "&3#2->X", "M700=(M700 | $000002) ^ $000002"}' WHERE mm_motor=2;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M32=0", "&3#2->0", "M700=M700 | $000002"}' WHERE mm_motor=2;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     3,        3,        'aligny', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.16,         16.15);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M33=1", "&3#3->Y", "M700=(M700 | $000004) ^ $000004"}' WHERE mm_motor=3;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M33=0", "&3#3->0", "M700=M700 | $000004"}' WHERE mm_motor=3;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     4,        3,        'alignz', 'mm',    60620.8,  121.0,        0.5,           '%*.3f mm',  0.45,         5.85);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M34=1", "&3#4->Z", "M700=(M700 | $000008) ^ $000008"}' WHERE mm_motor=4;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M34=0", "&3#4->0", "M700=M700 | $000008"}' WHERE mm_motor=4;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     5,        0,        'anal',   'deg',   142.0,    3.0,          0.2,          '%*.0f°',   '-Infinity', 'Infinity');

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     6,        4,        'zoom',   'cts',   1.0,      10.0,         0.2,          '%*.0f cts',   0.0,         35700);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M36=1", "&4#6->Z", "M700=(M700 | $000020) ^ $000020"}' WHERE mm_motor=6;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M36=0", "&4#6->0", "M700=M700 | $000020"}' WHERE mm_motor=6;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     7,        5,        'apery',  'mm',    121241.6, 201.0,        1.0,          '%*.3f mm',   0.2,         3.25);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M37=1", "&5#7->Y", "M700=(M700 | $000040) ^ $000040"}' WHERE mm_motor=7;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M37=0", "&5#7->0", "M700=M700 | $000040"}' WHERE mm_motor=7;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     8,        5,        'aperz',  'mm',    60620.8,  201.0,        1.0,          '%*.3f mm',   0.3,         82.5);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M38=1", "&5#8->Z", "M700=(M700 | $000080) ^ $000080"}' WHERE mm_motor=8;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M38=0", "&5#8->0", "M700=M700 | $000080"}' WHERE mm_motor=8;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     9,        5,        'capy',   'mm',    121241.6, 201.0,        1.0,          '%*.3f mm',   0.05,        3.19);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M39=1", "&5#9->U", "M700=(M700 | $000100) ^ $000100"}' WHERE mm_motor=9;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M39=0", "&5#9->0", "M700=M700 | $000100"}' WHERE mm_motor=9;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     10,       5,        'capz',  'mm',     19865.6,  201.0,        0.5,          '%*.3f mm',   0.57,        81.49);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M40=1", "&5#10->V", "M700=(M700 | $000200) ^ $000200"}' WHERE mm_motor=10;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M40=0", "&5#10->0", "M700=M700 | $000200"}' WHERE mm_motor=10;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     11,       5,        'scin',  'mm',     19865.6,  151.0,        0.5,          '%*.3f mm',   0.02,        86.1);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M41=1", "&5#11->W", "M700=(M700 | $000400) ^ $000400"}' WHERE mm_motor=11;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M41=0", "&5#11->0", "M700=M700 | $000400"}' WHERE mm_motor=11;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     17,       2,        'cenx',  'mm',     182400.,  150.0,        0.5,          '%*.3f mm',   -2.56,       2.496);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M47=1", "&2#17->X", "M700=(M700 | $010000) ^ $010000"}' WHERE mm_motor=17;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M47=0", "&2#17->0", "M700=M700 | $010000"}' WHERE mm_motor=17;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     18,       2,        'ceny',  'mm',     182400.,  150.0,        0.5,          '%*.3f mm',   -2.58,       2.4);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M48=1", "&2#18->Y", "M700=(M700 | $020000) ^ $020000"}' WHERE mm_motor=18;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M48=0", "&2#18->0", "M700=M700 | $020000"}' WHERE mm_motor=18;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,    mm_min,      mm_max) VALUES
                            ( 2,     19,       7,        'kappa', 'deg',    2844.444, 50.0,         0.2,          '%*.2f°',     -5.0,        248.0);

UPDATE pmac.md2_motors SET mm_active_init   = '{"M49=1", "&7#19->X", "M700=(M700 | $040000) ^ $040000"}' WHERE mm_motor=19;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M49=0", "&7#19->0", "M700=M700 | $040000"}' WHERE mm_motor=19;

INSERT INTO pmac.md2_motors (mm_stn, mm_motor, mm_coord, mm_name,  mm_unit, mm_u2c,   mm_max_speed, mm_max_accel, mm_printf,   mm_min,      mm_max) VALUES
                            ( 2,     20,       7,        'phi',   'deg',    711.111,  50,           0.2,          '%*.2f°',    '-Infinity', 'Infinity');

UPDATE pmac.md2_motors SET mm_active_init   = '{"M50=1", "&7#20->Y", "M700=(M700 | $080000) ^ $080000"}' WHERE mm_motor=20;
UPDATE pmac.md2_motors SET mm_inactive_init = '{"M50=0", "&7#20->0", "M700=M700 | $080000"}' WHERE mm_motor=20;




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
    --  M453=1		flag indicating program is running
    --  &2              The centering stages are in coordinate system 2
    --  E               Enable centerx and centery
    --  B53             Run program 53 from the beginning
    --  R               GO!

    PERFORM pmac.md2_queue_push( the_stn, 'M453=1&2E'); --
    PERFORM pmac.md2_queue_push( the_stn, '&2B53R');	-- make sure another coordinate system didn't sneak in
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
    --  M406=1	  flag indicating program is running
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
    requested_counts     int;
    low_limit_violation  boolean;
    high_limit_violation boolean;
    motor_number         int;
    motor_coord          int;
  BEGIN
    
    SELECT INTO motor_number, motor_coord, requested_counts,                 low_limit_violation,         high_limit_violation
                mm_motor,     mm_coord,    (requested_position*mm_u2c)::int, requested_position < mm_min, requested_position > mm_max
      FROM pmac.md2_motors
      WHERE mm_name=the_motor and mm_stn=the_stn;

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
  p170 float;		-- PMAC start angle in counts
  p171 float;		-- PMAC stop angle in counts
  p173 float;		-- PMAC omega velocity (cts/msec)
  p175 float;		-- PMAC acceleration time (msec)
  p180 float;		-- PMAC exposure time (msec)
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

COMMIT;
