#! /bin/bash
# Copyright 2018 by Northwestern University
# All rights reserved.
# Author: Keith Brister
#

# First, we need the default time stamps.  One does this on Ubuntu
# 16.04 by commenting out the traditional file format line.  Why is
# the rsyslog documentation so obtuse?
#
if ( grep -E -q '^\$ActionFileDefaultTemplate RSYSLOG_TraditionalFileFormat' /etc/rsyslog.conf ) then
   cp /etc/rsyslog.conf /etc/rsyslog.conf.bak
   sed 's/^\(\$ActionFileDefaultTemplate RSYSLOG_TraditionalFileFormat\)/# \1/' /etc/rsyslog.conf.bak >/etc/rsyslog.conf
   systemctl restart rsyslog.service
fi


diff -q pgpmac_rsyslogd.conf /etc/rsyslog.d/pgpmac_rsyslogd.conf >/dev/null 2>&1 || ( cp -f pgpmac_rsyslogd.conf /etc/rsyslog.d && systemctl restart rsyslog.service )

