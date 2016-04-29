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
  SELECT ARRAY(SELECT b.e FROM (SELECT unnest(a)) AS b(e) WHERE b.e <> element) INTO result;
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
    EXECUTE 'LISTEN ' || ntfy_kvs;	-- A UI changed something we're listening for via px.kvs
    --
    -- Mark our station's presets to notify us on change
    --
    ntfy_kvsa := ('{' || ntfy_kvs || '}')::text[];
    UPDATE px.kvs SET kvnotify = kvnotify || ntfy_kvsa WHERE kvname like 'stns.' || the_stn || '.%.presets.%' and (not kvnotify @> ntfy_kvsa or kvnotify is null);


    -- Log the fact that we are connecting
    --
    INSERT INTO pmac.md2_registration (mr_stn) values (the_stn);

    PERFORM px.ininotifies( the_stn);


    -- Remove all the old information from the database queues
    --
    DELETE FROM pmac.md2_queue WHERE mq_stn=the_stn;
    PERFORM px.md2clearqueue( the_stn);
    PERFORM px.runqueue_clear( the_stn);
    PERFORM cats._clearqueue( the_stn);
    

    -- Prepare some common queries
    --
    PREPARE getkvs                 AS SELECT pmac.getkvs();
    PREPARE nextaction             AS SELECT action FROM px.nextaction();
    PREPARE md2_queue_next         AS SELECT pmac.md2_queue_next();
    PREPARE kvupdate( text[])      AS SELECT px.kvupdate($1);
    PREPARE nexterrors             AS SELECT * FROM px.nexterrors();
    PREPARE acknowledgeerror( int) AS SELECT px.acknowledgeerror( $1);

  END;
$$ LANGUAGE plpgsql SECURITY DEFINER;
ALTER FUNCTION pmac.md2_init( int) OWNER TO lsadmin;


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


COMMIT;


