#! /usr/bin/python
#
# Class to support parsing windows ini files mainly for the md2 initialization
# encountered by the LS-CAT pgpmac project
#
# Copyright 2013 by Keith Brister
##########################################################################
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.
#
##########################################################################
#
# We assume the sections and options are case insensitive and that,
# although nested sections are implied by the format used by the md2,
# that the nesting has no practical importance.
#
# The current version is for READING the files.
#
# TODO: add writing.  We'll need to keep track of the preferred case
# used in the ini file as well as the existing comments.  This is
# mildly tricky since comments apparently can appear on both option
# lines and non-option lines so we'll need to track the line number
# within each section to preserve all the comments.  Strictly speaking
# this is not necessary as we can just spit stuff out all lower case
# without comments and, presumably, the md2 should be able to deal
# with it.  However, there is enough of a problem with the lack of
# documentation that willfully removing seems like a bad idea.
#

class iniParser:
    
    def __init__( self, fn):
        self.f = open( fn, "r")
        self.sd = {}


    def read( self):
        self.sd = {}
        current_section = "default"
        current_dict    = {}
        for l in self.f.readlines():
            sl = l.strip()
            if len(sl) > 0:
                if sl[0] == ";":
                    continue

                if sl[0] == "[" and sl.find("]") > 1:
                    self.sd[current_section] = current_dict
                    current_dict = {}
                    current_section = (sl[1:sl.find("]")]).lower()

                else:
                    if sl.find(";") > 0:
                        s = sl[0:sl.find(";")]
                    else:
                        s = sl

                    if s.find("=") > 0:
                        slist = s.split("=")
                        if len(slist) == 2:
                            k = (slist[0].strip()).lower()
                            v = slist[1].strip()
                            current_dict[k] = v
            
        self.sd[current_section] = current_dict

    
        self.f.close()


    def sections( self):
        ks = set(self.sd.keys())
        return list(ks.difference( ["default"]))

    def options( self, section):
        return self.sd[section.lower()].keys()


    def has_section( self, section):
        return self.sd.has_key( section.lower())

    def has_option( self, section, option):
        if self.has_section( section):
            return self.sd[section.lower()].has_key( option.lower())
        return False

    def get( self, section, option):
        return self.sd[section.lower()][option.lower()]


if __name__ == "__main__":
    ip = iniParser( "21-ID-D/microdiff_pref.ini")
    ip.read()
    print ip.get( "CentringXYTable", "PhiReference")
