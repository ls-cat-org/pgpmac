#! /usr/bin/python
# coding=utf-8

import sys
import iniParser


if len(sys.argv) <= 1:
    print >> sys.stderr, "Usage: %s headOfRedisVariableNames [prefIniFileName [hardIniFileName]]"
    sys.exit(-1)

if len(sys.argv) > 1:
    head = sys.argv[1]

if len(sys.argv) > 2:
    pref_ini = sys.argv[2]
else:
    pref_ini = None

if len(sys.argv) > 3:
    hard_ini = sys.argv[3]
else:
    hard_ini = None


configs = {
    "orange-2"            : { "re" : "redis\.kvseq|stns\.2\.(.+)", "head" : "stns.2", "pub" : "MD2-21-ID-E"},
    "orange-2.ls-cat.org" : { "re" : "redis\.kvseq|stns\.2\.(.+)", "head" : "stns.2", "pub" : "MD2-21-ID-E"},
    "venison.ls-cat.org"  : { "re" : "redis\.kvseq|stns\.2\.(.+)", "head" : "stns.2", "pub" : "MD2-21-ID-E"}
}

#
# Bug/Feature: only fields listed in motor_dict will be searched for in the ini file.
#
# Also see the comments for the motor_field_lists list below
#
#   motor_dict keys
#         motor_num:    The pmac motor number between 1 and 32 inclusive.  Leave undefined or set to -1 for motor for DAC and Binary Output motor like objects
#         coord_num:    The coordinate system the said motor finds itself in between 1 and 16 inclusive.  Leave undefined or 0 for DAC and Binary Output motor like objects.
#         max_accel:    counts/msec/msec
#         max_speed:    counts/msec
#               u2c:    The conversion between counts and user units: Multiply user units by u2c to get counts.  Should never be zero.
#            active:    1 if the motor should be set up and used, 0 otherwise
#          hard_ini:    The section name for this motor in the microdiff_hard.ini file
#          moveMode:    freeRotation, rotation, or translation (default) used for the LS-CAT GUI
#         reference:    (omega only) The angle for which centering.y is up and centering.x is positive downstream
#              axis:    The axis letter for the PMAC in the specified coordinate system (X, Y, Z, etc)
#   neutralPosition:    The offset in user units between the home position and what we want to call zero
#            printf:    The printf format string for the position in the ncurses interface (uses a field width specifier *)
#            format:    The printf format string to update the redis value
#       maxPosition:    The software upper limit in user units relative to the home position
#       minPosition:    The software lower limit in user units relative to the home position
#         smallStep:    Recommened small step value for a user interface
#         largeStep:    Recommened large step value for a user interface
# update_resolution:    Don't update redis until the position has changed by this amount in user units
#
#     NOTE: active_init, home, and inactive_init should only be specified if the default string will not serve the purposes such as
#     for omega and the polarizer
#
#   active_init:        A comma separated list of strings (double quoted if spaces present) enclosed in braces to send to the PMAC when the motor is active.
#          home:`       A comma separated list of strings (double quoted if spaces present) enclosed in braces to send to the PMAC to home the motor
# inactive_init:        A comma separated list of strings (double quoted if spaces present) enclosed in braces to send to the PMAC when the motor is inactive.

motor_dict = {
    "omega" : { "motor_num" : "1", "max_accel" : "2", "max_speed" : "1664", "coord_num" : "1", "u2c" : "12800",
                "home" : '{"M401=1 M1115=1 #1$",&1E,#1&1B1R}',"active_init" : '{M31=1,&1#1->X,"M700=(M700 | $000001) ^ $000001", M1115=1}',
                "inactive_init" : '{M31=0,&1#1->0,"M700=M700 | $000001",M1115=0}',"moveMode" :  "freeRotation",
                "reference" :  "228.5", "format" :  "%.3f", "printf" : "%*.4f°", "axis" : "X",
                "hard_ini"  : "PHIRotationAxis.PHIMotor", "neutralPosition" : "0", "active" : "1"
                },
    "align.x" : { "motor_num" : "2", "max_accel" : "2", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "smallStep" :  "0.001",
                  "axis" :  "X", "format" :  "%.3f",
                  "minPosition" :  "0.1", "maxPosition" :  "4.0",
                  "hard_ini"  : "PHIAxisXYZTable.PHIXMotor", "neutralPosition" : "0", "active" : "1"
                  },
    "align.y" : { "motor_num" : "3", "max_accel" : "0.5", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "smallStep" :  "0.001",
                  "axis" :  "Y", "format" :  "%.3f",
                  "minPosition" :  "0.16", "maxPosition" :  "16.15",
                  "hard_ini"  : "PHIAxisXYZTable.PHIYMotor", "neutralPosition" : "0", "active" : "1"
                  },
    "align.z" : { "motor_num" : "4", "max_accel" : "0.5", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "smallStep" :  "0.001",
                  "axis" :  "Z", "format" :  "%.3f",
                  "minPosition" :  "0.45", "maxPosition" :  "5.85",
                  "hard_ini"  : "PHIAxisXYZTable.PHIZMotor", "neutralPosition" : "0", "active" : "1"
                  },
    "lightPolar" : { "motor_num" : "5", "max_accel" : "0.2", "max_speed" : "3", "u2c" : "142", "coord_num" : "0",
                     "home" : '{#5$,#5HMZ}', "active_init" : '{}', "inactive_init" : '{}',
                     "largeStep" :  "45", "smallStep" :  "10", "format" : "%.1f",
                     "printf" :  "%*.1f°", "update_resolution" :  "1",
                     "hard_ini" : "Analyser.AnalyserMotor", "neutralPosition" : "0", "active" : "1"
                     },
    "cam.zoom" : { "motor_num" : "6","max_accel" : "0.2", "max_speed" : "10", "coord_num" : "4", "u2c" : "1.0",
                   "smallStep" :  "1",
                   "axis" :  "Z","format" :  "%.0f",
                   "minPosition" :  "1","update_resolution" :  "1",
                   "hard_ini" : "CoaxZoom.ZoomMotor", "neutralPosition" : "0", "in_position_band" : "1600", "active" : "1"
                   },
    "appy" : { "motor_num" : "7","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "121241.6",
               "smallStep" :  "0.002",
               "axis" :  "Y","format" :  "%.3f",
               "minPosition" :  "0.2","maxPosition" :  "3.25",
               "hard_ini" : "ApertureYZTable.ApertureYMotor", "neutralPosition" : "0", "active" : "1"
               },
    "appz" : { "motor_num" : "8","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "60620.8",
               "smallStep" :  "0.002",
               "axis" :  "Z","format" :  "%.3f",
               "minPosition" :  "0.3","maxPosition" :  "82.5",
               "hard_ini" : "ApertureYZTable.ApertureZMotor", "neutralPosition" : "0", "active" : "1"
               },
    "capy" : { "motor_num" : "9","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "121241.6",
               "smallStep" :  "0.002",
               "axis" :  "U","format" :  "%.3f",
               "minPosition" :  "0.05","maxPosition" :  "3.19",
               "hard_ini" : "CapillaryBSYZtable.CapillaryBSYMotor", "neutralPosition" : "0", "active" : "1"
              },
    "capz" : { "motor_num" : "10","max_accel" : "0.5", "max_speed" : "201", "coord_num" : "5", "u2c" : "19865.6",
               "smallStep" :  "0.002",
               "axis" :  "V","format" :  "%.3f",
               "minPosition" :  "0.57","maxPosition" :  "81.49",
               "hard_ini" : "CapillaryBSYZtable.CapillaryBSZMotor", "neutralPosition" : "0", "active" : "1"
              },
    "scint" : { "motor_num" : "11","max_accel" : "0.5", "max_speed" : "151", "coord_num" : "5", "u2c" : "19865.6",
                "smallStep" :  "0.002",
                "axis" :  "W","format" :  "%.3f",
                "minPosition" :  "0.2","maxPosition" :  "86.1",
                "hard_ini" : "ScintillatorPhotodiode.Zmotor", "neutralPosition" : "0", "active" : "1"
                },
    "centering.x" : { "motor_num" : "17","max_accel" : "0.5",  "max_speed" : "150", "coord_num" : "2", "u2c" : "182400",
                      "smallStep" :  "0.001",
                      "axis" :  "X","format" :  "%.3f",
                      "minPosition" :  "-2.56","maxPosition" :  "2.496",
                      "hard_ini" : "CentringXYTable.XCentringMotor", "neutralPosition" : "0", "active" : "1"
                     },
    "centering.y" : {"motor_num" : "18","max_accel" : "0.5",  "max_speed" : "150", "coord_num" : "2", "u2c" : "182400",
                     "smallStep" :  "0.001",
                     "axis" :  "Y","format" :  "%.3f",
                     "minPosition" :  "-2.58","maxPosition" :  "2.4",
                      "hard_ini" : "CentringXYTable.YCentringMotor", "neutralPosition" : "0", "active" : "1"
                     },
    "kappa" : { "motor_num" : "19","max_accel" : "0.2",  "max_speed" : "50", "coord_num" : "7", "u2c" : "2844.444",
                "moveMode" :  "rotation",
                "axis" :  "X","format" :  "%.2f",
                "minPosition" :  "-5","update_resolution" :  "1.0",
                "hard_ini" : "MiniKappa.Kappa1", "neutralPosition" : "0", "active" : "1"
                },
    "phi" : { "motor_num" : "20","max_accel" : "0.2",  "max_speed" : "50", "coord_num" : "7", "u2c" : "711.111",
              "moveMode" :  "freeRotation",
              "axis" :  "Y","format" :  "%.2f",
              "update_resolution" :  "1.0",
              "hard_ini" : "MiniKappa.Kappa2", "neutralPosition" : "0", "active" : "1"
              },
    "fastShutter" : { "canHome" :  "false","type" :  "BO",
                      "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                      },
    "frontLight.intensity" : { "canHome" :  "false","type" :  "DAC",
                               "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                              },
    "backLight.intensity" : { "canHome" :  "false","type" :  "DAC",
                              "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                             },
    "scint.focus" : { "canHome" :  "false","type" :  "DAC",
                      "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                      },
    "backLight" : { "canHome" :  "false","type" :  "BO",
                    "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                    },
    "cryo" : { "canHome" :  "false","type" :  "BO",
               "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
               },
    "dryer" : { "canHome" :  "false","type" :  "BO",
                "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                },
    "fluo" : { "canHome" :  "false","type" :  "BO",
               "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
               },
    "frontLight" : { "canHome" :  "false","type" :  "BO",
                     "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                     },
    "backLight.factor" : { "canHome" :  "false","type" :  "DAC",
                           "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                           },
    "frontLight.factor" : { "canHome" :  "false","type" :  "DAC",
                            "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                            },
    "smartMagnet"  : { "canHome" :  "false","type" :  "BO", "active_init" : '{m1100=0,m1106=1}', "inactive_init" : '{m1100=1,m1106=0}',
                     "update_resolution" :  "0.5","canStop" :  "false", "active" : "1", "in_position_band" : "0"
                        }
    }


def mk_home( mname, d):
    if not d.has_key("motor_num") or not d.has_key("coord_num"):
        return ""
    motor_num = int(d["motor_num"])
    coord_num = int(d["coord_num"])
    if motor_num < 1 or motor_num > 32:
        return ""
    
    if mname == "kappa":
        prog_num = 119
    else:
        prog_num = motor_num

    return '{#%d$,M%d=1,&%dE,#%d&%dB%dR}' % (motor_num, motor_num+400, coord_num, motor_num, coord_num, prog_num)

def mk_active_init( d):
    if not d.has_key("motor_num") or not d.has_key("coord_num") or not d.has_key( "axis"):
        return ""
    motor_num = int(d["motor_num"])
    coord_num = int(d["coord_num"])
    axis      = str(d["axis"])
    mask      = 1 << (motor_num - 1)
    if motor_num < 1 or motor_num > 32:
        return ""
    return '{M%d=1,&%d#%d->%s,"M700=(M700 | $%0x) ^ $%0x"}' % (motor_num + 30, coord_num, motor_num, axis, mask, mask)

def mk_inactive_init( d):
    if not d.has_key("motor_num") or not d.has_key("coord_num") or not d.has_key( "axis"):
        return ""
    motor_num = int(d["motor_num"])
    coord_num = int(d["coord_num"])
    axis      = str(d["axis"])
    mask      = 1 << (motor_num - 1)
    if motor_num < 1 or motor_num > 32:
        return ""
    return '{M%d=0,&%d#%d->0,"M700=M700 | $%0x"}' % (motor_num + 30, coord_num, motor_num, mask)

def active_simulation( sim):
    if str(sim) != "0":
        rtn = "0"
    else:
        rtn = "1"
    return rtn

def asis( arg):
    return arg

hard_ini_fields = {
    "active"          : ["Simulation", active_simulation],
    "coord_num"       : ["CoordinateSystem", asis],
    "largeStep"       : ["LargeStep", asis],
    "maxPosition"     : ["MaxPosition", asis],
    "minPosition"     : ["MinPosition", asis],
    "motor_num"       : ["MotorNumber", asis],
    "neutralPosition" : ["NeutralPosition", asis],
    "precision"       : ["Precision", asis],
    "smallStep"       : ["SmallStep", asis],
    "u2c"             : ["UnitRatio", asis]
    }

# DBR TYPES
# 0  String
# 1  Short   (16 bit)
# 2  Float   (32 bit)
# 3  Enum    (not supported as of 121219)
# 4  Char    (8 bit)
# 5  Int     (32 bit)
# 6  Double  (64 bit)

motor_field_lists = [
    # name,             default,      dbrtype
    ["active",            "1",          1],     # 1 if the motor is to be enabled and used (not fully supported as of 121219)
    ["active_init",       "",           0],     # postgresql style string array of initialization strings to send to PMAC if the motor is active
    ["axis",              "",           4],     # PMAC axis (single charater: X,Y,Z, etc)
    ["canHome",           "0",          1],     # 1 if a homing routine can be called
    ["canMove",           "true",       0],     # "true" if we can move this motor, "false" if we cannot.
    ["canStop",           "true",       0],     # "true" if it makes sense to display a stop button, "false" otherwise
    ["coord_num",         "",           1],     # PMAC coordinate system number for this motor
    ["currentPreset",     "",           0],     # Name of the current preset position
    ["in_position_band",  "160",        1],     # Motors within this amount are considered "In Position".  UNITS ARE 1/16 OF A COUNT
    ["format",            "%f",         0],     # format string for publish position to redis
    ["hard_ini",          None,         0],     # Name of section in microdiff_hard.ini
    ["home",              "",           0],     # postgresql style string array of strings to send to PMAC to home motor
    ["inPosition",        "true",       0],     # "true" if the motor is in position, "false" if it is moving
    ["inactive_init",     "",           0],     # postgresql style string array of initialization strings to send to PMAC if the motor is inactive
    ["largeStep",         "1.0",        6],     # increment for large step in a UI
    ["maxPosition",       "Infinity",   6],     # upper soft limit
    ["max_accel",         "",           0],     # maximum motor acceleration, used for motors that are too be scanned (ie, omega)
    ["max_speed",         "",           6],     # maximum motor speed, used for motors that are too be scanned (ie, omega)
    ["minPosition",       "-Infinity",  6],     # lower soft limit
    ["motor_num",         "-1",         1],     # PMAC motor number
    ["moveMode",          "translation",0],     # translation, rotation, freeRotation
    ["name",              "",           0],     # What we think the motor should be called in a UI
    ["negLimitSet",       "0",          1],     # 1 if on the limit, 0 otherwise
    ["neutralPosition",   "0",          6],     # Move here after a home and call it zero.  Should be called -offset or offset or somehting like that.
    ["posLimitSet",       "0",          1],     # 1 if on the limit, 0 otherwise
    ["position",          "",           6],     # our position
    ["precision",         "0.001",      6],     # precision of the motion: moves of less than this amount are ignored (use in_position_band instead)
    ["presets.length",    "0",          1],     # number of presets defined
    ["printPrecision",    "3",          1],     # for ui to print out position (see the printf field for another way of doing this)
    ["printf",            "%*.3f",      0],     # printf style format string for ncurses interface
    ["smallStep",         "0.1",        6],     # step size for UI for a fine movement
    ["status_str",        "",           0],     # Explanation of what the motor is doing
    ["type",              "PMAC",       0],     # type of motor: PMAC, DAC, BO, SOFT, etc
    ["u2c",               "1.0",        6],     # multipy user units times u2c to get motor counts
    ["unit",              "mm",         0],     # user units
    ["update_resolution", "0.001",      4]      # update redis when motor is moving only when a change of this magnetude is seen
    ]
bi_list = ["CryoSwitch"]

motor_presets = {
    "align.x" : [
        # name            value       canTune    pref_ini section           pref_ini option
        [ "Beam",         "0.0",      "1",    "PHIAxisXYZTable",         "XBeam_X1"],
        [ "Back",        "-1.8",      "1",    "PHIAxisXYZTable",         "XScintillatorOut_X2"],
        [ "Back_Vector", "-1.8",      "1",    "PHIAxisXYZTable",         "XScintillatorOut_X2"]
        ],
    "align.y" : [
        # name           value       canTune    pref_ini section           pref_ini option
        [ "Beam",        "0.0",        "1",    "PHIAxisXYZTable",         "YBeam_Y1"],
        [ "Back",        "1.0",        "1",    "PHIAxisXYZTable",         "YScintillatorOut_Y2"],
        [ "Back_Vector", "1.0",        "1",    "PHIAxisXYZTable",         "YScintillatorOut_Y2"]
        ],
    "align.z" : [
        # name           value       canTune    pref_ini section           pref_ini option
        [ "Beam",        "0.0",        "1",    "PHIAxisXYZTable",         "ZBeam_Z1"],
        [ "Back",        "1.9",        "1",    "PHIAxisXYZTable",         "ZScintillatorOut_Z2"],
        [ "Back_Vector", "1.9",        "1",    "PHIAxisXYZTable",         "ZScintillatorOut_Z2"]
        ],
    "appy" : [
        # name   value       canTune    pref_ini section           pref_ini option
        [ "In", "0.117",         "1",    "ApertureYZTable",        "BeamHorizontalPosition_Y0"]
        ],
    "appz" : [
        [ "In",    "80",         "1",    "ApertureYZTable",        "BeamVerticalPosition_Z1"],
        [ "Out",   "71.777",     "0",    "ApertureYZTable",        "VerticalOffScreenPosition_Z2"],
        [ "Cover", "2.0",        "0",    "ApertureYZTable",        "OffVerticalPosition_Z0"]
        ],
    "backLight" : [
        [ "On",    "1",           None,  None,                     None],
        [ "Off",   "0",           None,  None,                     None]
        ],
    "frontLight" : [
        [ "On",    "1",           None,  None,                     None],
        [ "Off",   "0",           None,  None,                     None]
        ],
    "capy" : [
        [ "In",    "0.082",       "1",   "CapillaryBSYZtable",     "HorizontalBeamPosition_Y0"]
        ],
    "capz" : [
        [ "In",    "78.2617",     "1",   "CapillaryBSYZtable",     "VerticalBeamPosition_Z1"],
        [ "Out",   "69.944",      "0",   "CapillaryBSYZtable",     "VerticalOffScreenPosition_Z2"],
        [ "Cover", "0.3",         "0",   "CapillaryBSYZtable",     "VeticalOffPosition_Z0"]
        ],

    "fastShutter" : [
        [ "Open",   "1",          None,  None,                     None],
        [ "Close",  "0",          None,  None,                     None]
        ],
    "kappa" : [
        [ "manualMount", "180.0", None,  "MiniKappa",              "Kappa1MountPosition"],
        [ "reference",   "228.5", None,  "CentringXYTable",        "PhiReference"]
        ],
    "omega" : [
        [ "manualMount", "180.0", None,  "PHIRotationAxis",        "KappaMountPosition"]
        ],
    "scint.focus" : [
        [ "tuner",       "53",    "1",   "ScintillatorPhotodiode", "OnFocusPiezoPosition"]
        ],
    "scint" : [
        [ "Photodiode",   "53.0",   "1", "ScintillatorPhotodiode", "DiodeOnBeamVerticalPosition_Z2"],
        [ "Scintillator", "78.788", "1", "ScintillatorPhotodiode", "ScintiOnBeamVerticalPosition_Z1"],
        [ "Cover",        "2.0",    "0", "ScintillatorPhotodiode", "OffVerticalPosition_Z0"]
        ]
    }


zoom_settings = [
    #lev   front  back  pos     scalex  scaley   section
    [1,     4.0,   8.0,  34100, 2.7083,  3.3442, "CoaxCam.Zoom1"],
    [2,     6.0,   8.1,  31440, 2.2487,  2.2776, "CoaxCam.Zoom2"],
    [3,     6.5,   8.2,  27460, 1.7520,  1.7550, "CoaxCam.Zoom3"],
    [4,     7.0,   8.3,  23480, 1.3360,  1.3400, "CoaxCam.Zoom4"],
    [5,     8.0,  10.0,  19500, 1.0140,  1.0110, "CoaxCam.Zoom5"],
    [6,     9.0,  12.0,  15520, 0.7710,  0.7760, "CoaxCam.Zoom6"],
    [7,    10.0,  17.0,  11540, 0.5880,  0.5920, "CoaxCam.Zoom7"],
    [8,    12.0,  25.0,   7560, 0.4460,  0.4480, "CoaxCam.Zoom8"],
    [9,    15.0,  37.0,   3580, 0.3410,  0.3460, "CoaxCam.Zoom9"],
    [10,   16.0,  42.0,      0, 0.2700,  0.2690, "CoaxCam.Zoom10"]
    ]


# config 
for c in configs.keys():
    print "HMSET config.%s HEAD '%s' PUB '%s' RE '%s'" % (c.lower(), configs[c]["head"], configs[c]["pub"], configs[c]["re"])


# motor stuff
if hard_ini:
    hi = iniParser.iniParser( hard_ini)
    hi.read()

for m in motor_dict.keys():
    print "HSETNX %s.%s.name VALUE '%s'" % (head, m, m)         #  These values are not part of any defaults
    print "PUBLISH mk_pgpmac_redis %s.%s.name"  % (head, m)     #
    print "HSETNX %s.%s.name DBRTYPE 0"  % (head, m)            #
    print "HSETNX %s.%s.position VALUE ''" % (head, m)          #
    print "PUBLISH mk_pgpmac_redis %s.%s.position" % (head, m)  #
    print "HSETNX %s.%s.position DBRTYPE 6" % (head, m)         #


    if hard_ini != None and motor_dict[m].has_key("hard_ini"):
        motor_dict[m]["motor_num"] = hi.get(motor_dict[m]["hard_ini"], "motornumber")
        motor_dict[m]["coord_num"] = hi.get(motor_dict[m]["hard_ini"], "coordinatesystem")

    # set home, active_init, and inactive_init based on current motor and coordinate numbers
    #
    if not motor_dict[m].has_key( "home"):
        motor_dict[m]["home"] = mk_home( m, motor_dict[m])
    if not motor_dict[m].has_key( "active_init"):
        motor_dict[m]["active_init"] = mk_active_init( motor_dict[m])
    if not motor_dict[m].has_key( "inactive_init"):
        motor_dict[m]["inactive_init"] = mk_inactive_init( motor_dict[m])


    for k in motor_dict[m]:
        if k == "hard_ini":     # this is sort of a meta field
            continue

        # Use the value from the hard ini file, if it is available
        # Overide the current value if it is available
        #

        if hard_ini == None or \
                not motor_dict[m].has_key("hard_ini") or \
                motor_dict[m]["hard_ini"] == None or \
                not hard_ini_fields.has_key( k) or \
                not hi.has_section( motor_dict[m]["hard_ini"]) or \
                not hi.has_option( motor_dict[m]["hard_ini"], hard_ini_fields[k][0]):

            # Use the hard coded value found in this file
            #
            v = motor_dict[m][k]
            f = "HSETNX"
        else:
            # Use the ini file value
            #
            xlate = hard_ini_fields[k][1]
            v = xlate(hi.get( motor_dict[m]["hard_ini"], hard_ini_fields[k][0]))
            f = "HSET"

        print "%s %s.%s.%s VALUE '%s'" % (f, head, m, k, v)
        print "PUBLISH mk_pgpmac_redis %s.%s.%s" % (f, head, m)

    # Throw out the default default value for fields not found any other way
    #
    for field, default, dbrtype  in motor_field_lists:
        print "HSETNX %s.%s.%s VALUE '%s'" % (head, m, field, default)
        print "PUBLISH mk_pgpmac_redis %s.%s.%s" % (head, m, field)
        print "HSETNX %s.%s.%s DBRTYPE '%s'" % (head, m, field, dbrtype)

    # Add the presets
    #
    if pref_ini:
        pi = iniParser.iniParser( pref_ini)
        pi.read()
        
    i = 0;
    if motor_presets.has_key( m):
        for pname, ppos, ptune, section, option in motor_presets[m]:
            print "HSETNX %s.%s.presets.%d.name VALUE %s"     % (head, m, i, pname)
            print "PUBLISH mk_pgpmac_redis %s.%s.presets.%d.name" % (head, m, i)

            f = "HSETNX"
            if pref_ini and section and option and pi.has_section( section) and pi.has_option( section, option):
                ppos = pi.get( section, option)
                f = "HSET"
                    
            print "%s %s.%s.presets.%d.position VALUE %s" % ( f, head, m, i, ppos)
            print "PUBLISH mk_pgpmac_redis %s.%s.presets.%d.position" % (head, m, i)

            if ptune != None:
                print "HSETNX %s.%s.presets.%d.canTune VALUE %s" % ( head, m, i, ppos)
                print "PUBLISH mk_pgpmac_redis %s.%s.presets.%d.canTune" % (head, m, i)
            i += 1
        print "HSET %s.%s.presets.length VALUE %d" % ( head, m, i)
        print "PUBLISH mk_pgpmac_redis %s.%s.presets.length" % (head, m)

        
# light and zoom settings

for lev, f, b, p, x, y, section in zoom_settings:

    fnc = "HSETNX"
    if pref_ini != None and pi.has_section( section) and pi.has_option( section, "FrontLightIntensity"):
        f = pi.get( section, "FrontLightIntensity")
        fnc = "HSET"
    print "%s %s.cam.zoom.%d.FrontLightIntensity VALUE %s" % (fnc, head, lev, f)
    print "PUBLISH mk_pgpmac_redis %s.cam.zoom.%d.FrontLightIntensity" % (head, lev)

    fnc = "HSETNX"
    if pref_ini != None and pi.has_section( section) and pi.has_option( section, "LightIntensity"):
        b = pi.get( section, "LightIntensity")
        fnc = "HSET"
    print "%s %s.cam.zoom.%d.LightIntensity VALUE %s"      % (fnc, head, lev, b)
    print "PUBLISH mk_pgpmac_redis %s.cam.zoom.%d.LightIntensity"      % (head, lev)

    fnc = "HSETNX"
    if pref_ini != None and pi.has_section( section) and pi.has_option( section, "MotorPosition"):
        p = pi.get( section, "MotorPosition")
        fnc = "HSET"
    print "%s %s.cam.zoom.%d.MotorPosition VALUE %s"       % (fnc, head, lev, p)
    print "PUBLISH mk_pgpmac_redis %s.cam.zoom.%d.MotorPosition"       % (head, lev)

    fnc = "HSETNX"
    if pref_ini != None and pi.has_section( section) and pi.has_option( section, "ScaleX"):
        x = pi.get( section, "ScaleX")
        fnc = "HSET"
    print "%s %s.cam.zoom.%d.ScaleX VALUE %s"              % (fnc, head, lev, x)
    print "PUBLISH mk_pgpmac_redis %s.cam.zoom.%d.ScaleX"              % (head, lev)

    fnc = "HSETNX"
    if pref_ini != None and pi.has_section( section) and pi.has_option( section, "ScaleY"):
        y = pi.get( section, "ScaleY")
        fnc = "HSET"
    print "%s %s.cam.zoom.%d.ScaleY VALUE %s"              % (fnc, head, lev, y)
    print "PUBLISH mk_pgpmac_redis %s.cam.zoom.%d.ScaleY"              % (head, lev)
