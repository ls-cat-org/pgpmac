#! /usr/bin/python
# coding=utf-8

head = "stns.2"

motor_dict = {
    "omega" : { "motor_num" : "1", "max_accel" : "2", "max_speed" : "1664", "coord_num" : "1", "u2c" : "12800",
                "home" : '{"M401=1 M1115=1 #1$",&1E,#1&1B1R}',"active_init" : '{M31=1,&1#1->A,"M700=(M700 | $000001) ^ $000001", M1115=1}',
                "inactive_init" : '{M31=0,&1#1->0,"M700=M700 | $000001",M1115=0}',"moveMode" :  "freeRotation",
                "reference" :  "228.5", "format" :  "%.3f", "printf" : "%*.4f°"
                },
    "align.x" : { "motor_num" : "2", "max_accel" : "2", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "home" : '{#2$,M402=1,&3E,#2&3B2R}', "active_init" : '{M32=1,&3#2->X,"M700=(M700 | $000002) ^ $000002"}',
                  "inactive_init" : '{M32=0,&3#2->0,"M700=M700 | $000002"}',"smallStep" :  "0.001",
                  "axis" :  "X", "format" :  "%.3f",
                  "minPosition" :  "0.1", "maxPosition" :  "4.0"
                  },
    "align.y" : { "motor_num" : "3", "max_accel" : "0.5", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "home" : '{#3$,M403=1,&3E,#3&3B3R}', "active_init" : '{M33=1,&3#3->Y,"M700=(M700 | $000004) ^ $000004"}',
                  "inactive_init" : '{M33=0,&3#3->0,"M700=M700 | $000004"}',"smallStep" :  "0.001",
                  "axis" :  "Y", "format" :  "%.3f",
                  "minPosition" :  "0.16", "maxPosition" :  "16.15"
                  },
    "align.z" : { "motor_num" : "4", "max_accel" : "0.5", "max_speed" : "121", "coord_num" : "3", "u2c" : "60620.8",
                  "home" : '{#4$,M404=1,&3E,#4&3B4R}',"active_init" : '{M34=1,&3#4->Z,"M700=(M700 | $000008) ^ $000008"}',
                  "inactive_init" : '{M34=0,&3#4->0,"M700=M700 | $000008"}',"smallStep" :  "0.001",
                  "axis" :  "Z", "format" :  "%.3f",
                  "minPosition" :  "0.45", "maxPosition" :  "5.85"
                  },
    "lightPolar" : { "motor_num" : "5", "max_accel" : "0.2", "max_speed" : "3", "u2c" : "142",
                     "home" : '{#5$,#5HMZ}',
                     "largeStep" :  "45", "smallStep" :  "10", "format" : "%.1f",
                     "printf" :  "%*.1f°", "update_resolution" :  "1"
                     },
    "cam.zoom" : { "motor_num" : "6","max_accel" : "0.2", "max_speed" : "10", "coord_num" : "4", "u2c" : "1.0",
                   "home" : '{#6$,M406=1,&4E,#6&4B6R}', "active_init" : '{M36=1,&4#6->Z,"M700=(M700 | $000020) ^ $000020"}',
                   "inactive_init" : '{M36=0,&4#6->0,"M700=M700 | $000020"}',"smallStep" :  "1",
                   "axis" :  "Z","format" :  "%.0f",
                   "minPosition" :  "1","update_resolution" :  "1"
                   },
    "appy" : { "motor_num" : "7","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "121241.6",
               "home" : '{#7$,M407=1,&5E,#7&5B7R}', "active_init" : '{M37=1,&5#7->Y,"M700=(M700 | $000040) ^ $000040"}',
               "inactive_init" : '{M37=0,&5#7->0,"M700=M700 | $000040"}',"smallStep" :  "0.002",
               "axis" :  "Y","format" :  "%.3f",
               "minPosition" :  "0.2","maxPosition" :  "3.25"
               },
    "appz" : { "motor_num" : "8","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "60620.8",
               "home" : '{#8$,M408=1,&5E,#8&5B8R}', "active_init" : '{M38=1,&5#8->Z,"M700=(M700 | $000080) ^ $000080"}',
               "inactive_init" : '{M38=0,&5#8->0,"M700=M700 | $000080"}',"smallStep" :  "0.002",
               "axis" :  "Z","format" :  "%.3f",
               "minPosition" :  "0.3","maxPosition" :  "82.5"
               },
    "capy" : { "motor_num" : "9","max_accel" : "1", "max_speed" : "201", "coord_num" : "5", "u2c" : "121241.6",
               "home" : '{#9$,M409=1,&5E,#9&5B9R}', "active_init" : '{M39=1,&5#9->U,"M700=(M700 | $000100) ^ $000100"}',
               "inactive_init" : '{M39=0,&5#9->0,"M700=M700 | $000100"}',"smallStep" :  "0.002",
               "axis" :  "U","format" :  "%.3f",
               "minPosition" :  "0.05","maxPosition" :  "3.19"
              },
    "capz" : { "motor_num" : "10","max_accel" : "0.5", "max_speed" : "201", "coord_num" : "5", "u2c" : "19865.6",
               "home" : '{#10$,M410=1,&5E,#10&5B10R}', "active_init" : '{M40=1,&5#10->V,"M700=(M700 | $000200) ^ $000200"}',
               "inactive_init" : '{M40=0,&5#10->0,"M700=M700 | $000200"}', "smallStep" :  "0.002",
               "axis" :  "V","format" :  "%.3f",
               "minPosition" :  "0.57","maxPosition" :  "81.49"
              },
    "scint" : { "motor_num" : "11","max_accel" : "0.5", "max_speed" : "151", "coord_num" : "5", "u2c" : "19865.6",
                "home" : '{#11$,M411=1,&5E,#11&5B11R}', "active_init" : '{M41=1,&5#11->W,"M700=(M700 | $000400) ^ $000400"}',
                "inactive_init" : '{M41=0,&5#11->0,"M700=M700 | $000400"}',"smallStep" :  "0.002",
                "axis" :  "W","format" :  "%.3f",
                "minPosition" :  "0.2","maxPosition" :  "86.1"
                },
    "centering.x" : { "motor_num" : "17","max_accel" : "0.5",  "max_speed" : "150", "coord_num" : "2", "u2c" : "182400",
                      "home" : '{#17$,M417=1,&2E,#17&2B17R}', "active_init" : '{M47=1,&2#17->X,"M700=(M700 | $010000) ^ $010000"}',
                      "inactive_init" : '{M47=0,&2#17->0,"M700=M700 | $010000"}',"smallStep" :  "0.001",
                      "axis" :  "X","format" :  "%.3f",
                      "minPosition" :  "-2.56","maxPosition" :  "2.496"
                     },
    "centering.y" : {"motor_num" : "18","max_accel" : "0.5",  "max_speed" : "150", "coord_num" : "2", "u2c" : "182400",
                     "home" : '{#18$,M418=1,&2E,#18&2B18R}', "active_init" : '{M48=1,&2#18->Y,"M700=(M700 | $020000) ^ $020000"}',
                     "inactive_init" : '{M48=0,&2#18->0,"M700=M700 | $020000"}',"smallStep" :  "0.001",
                     "axis" :  "Y","format" :  "%.3f",
                     "minPosition" :  "-2.58","maxPosition" :  "2.4"
                     },
    "kappa" : { "motor_num" : "19","max_accel" : "0.2",  "max_speed" : "50", "coord_num" : "7", "u2c" : "2844.444",
                "home" : '{#19$,M419=1,&7E,#19&7B119R}', "active_init" : '{M49=1,&7#19->X,"M700=(M700 | $040000) ^ $040000"}',
                "inactive_init" : '{M49=0,&7#19->0,"M700=M700 | $040000"}',"moveMode" :  "rotation",
                "axis" :  "X","format" :  "%.2f",
                "minPosition" :  "-5","update_resolution" :  "1.0"
                },
    "phi" : { "motor_num" : "20","max_accel" : "0.2",  "max_speed" : "50", "coord_num" : "7", "u2c" : "711.111",
              "home" : '{#20$,M420=1,&7E,#20&7B20R}',  "active_init" : '{M50=1,&7#20->Y,"M700=(M700 | $080000) ^ $080000"}',
              "inactive_init" : '{M50=0,&7#20->0,"M700=M700 | $080000"}',"moveMode" :  "freeRotation",
              "axis" :  "Y","format" :  "%.2f",
              "update_resolution" :  "1.0"
              },
    "fastShutter" : { "canHome" :  "false","type" :  "BO",
                      "update_resolution" :  "0.5","canStop" :  "false"
                      },
    "frontLight.intensity" : { "canHome" :  "false","type" :  "DAC",
                               "update_resolution" :  "0.5","canStop" :  "false"
                              },
    "backLight.intensity" : { "canHome" :  "false","type" :  "DAC",
                              "update_resolution" :  "0.5","canStop" :  "false"
                             },
    "scint.focus" : { "canHome" :  "false","type" :  "DAC",
                      "update_resolution" :  "0.5","canStop" :  "false"
                      },
    "backLight" : { "canHome" :  "false","type" :  "BO",
                    "update_resolution" :  "0.5","canStop" :  "false"
                    },
    "cryo" : { "canHome" :  "false","type" :  "BO",
               "update_resolution" :  "0.5","canStop" :  "false"
               },
    "dryer" : { "canHome" :  "false","type" :  "BO",
                "update_resolution" :  "0.5","canStop" :  "false"
                },
    "fluo" : { "canHome" :  "false","type" :  "BO",
               "update_resolution" :  "0.5","canStop" :  "false"
               },
    "frontLight" : { "canHome" :  "false","type" :  "BO",
                     "update_resolution" :  "0.5","canStop" :  "false"
                     },
    "backLight.factor" : { "canHome" :  "false","type" :  "DAC",
                           "update_resolution" :  "0.5","canStop" :  "false"
                           },
    "frontLight.factor" : { "canHome" :  "false","type" :  "DAC",
                            "update_resolution" :  "0.5","canStop" :  "false"
                            },
    "smartMagnet"  : { "canHome" :  "false","type" :  "BO",
                     "update_resolution" :  "0.5","canStop" :  "false"
                        }
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
    # name,         default, dbrtype
    ["active",            "1",          1],     # 1 if the motor is to be enabled and used (not fully supported as of 121219
    ["active_init",       "",           0],     # postgresql style string array of initialization strings to send to PMAC if the motor is active
    ["axis",              "",           4],     # PMAC axis (single charater: X,Y,Z, etc)
    ["canHome",           "0",          1],     # 1 if a homing routine can be called
    ["canMove",           "true",       0],     # "true" if we can move this motor, "false" if we cannot.
    ["canStop",           "true",       0],     # "true" if it makes sense to display a stop button, "false" otherwise
    ["coord_num",         "",           1],     # PMAC coordinate system number for this motor
    ["currentPreset",     "",           0],     # Name of the current preset position
    ["format",            "%f",         0],     # format string for publish position to redis
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
    ["posLimitSet",       "0",          1],     # 1 if on the limit, 0 otherwise
    ["position",          "",           6],     # our position
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
    "appy" : [
        # name   value  canTune
        [ "In", "0.117", "1"]
        ],
    "appz" : [
        [ "In",    "80",     "1"],
        [ "Out",   "71.777", "0"],
        [ "Cover", "2.0",    "0"]
        ],
    "backLight" : [
        [ "On",    "1",       None],
        [ "Off",   "0",       None]
        ],
    "frontLight" : [
        [ "On",    "1",       None],
        [ "Off",   "0",       None]
        ],
    "capy" : [
        [ "In",    "0.082",     "1"]
        ],
    "capz" : [
        [ "In",    "78.2617",   "1"],
        [ "Out",   "69.944",    "0"],
        [ "Cover", "0.3",       "0"]
        ],

    "fastShutter" : [
        [ "Open",   "1",       None],
        [ "Close",  "0",       None]
        ],
    "kappa" : [
        [ "manualMount", "180.0", None]
        ],
    "omega" : [
        [ "manualMount", "180.0", None]
        ],
    "scint.focus" : [
        [ "tuner",       "53",    "1"]
        ],
    "scint" : [
        [ "Photodiode",   "53.0",   "1"],
        [ "Scintillator", "78.788", "1"],
        [ "Cover",        "2.0",    "0"]
        ]
    }


zoom_settings = [
    #lev   front  back  pos     scalex  scaley
    [1,     4.0,   8.0,  34100, 2.7083,  3.3442],
    [2,     6.0,   8.1,  31440, 2.2487,  2.2776],
    [3,     6.5,   8.2,  27460, 1.7520,  1.7550],
    [4,     7.0,   8.3,  23480, 1.3360,  1.3400],
    [5,     8.0,  10.0,  19500, 1.0140,  1.0110],
    [6,     9.0,  12.0,  15520, 0.7710,  0.7760],
    [7,    10.0,  17.0,  11540, 0.5880,  0.5920],
    [8,    12.0,  25.0,   7560, 0.4460,  0.4480],
    [9,    15.0,  37.0,   3580, 0.3410,  0.3460],
    [10,   16.0,  42.0,      0, 0.2700,  0.2690]
    ]


# pmac initializer
print "HSETNX %s.md2_pmac.init VALUE '%s'" % (head, '{\"ENABLE PLCC 0\",\"DISABLE PLCC 1\",\"ENABLE PLCC 2\",I5=3}')
print "HSETNX %s.md2_status_code VALUE 7" % (head)

# motor stuff
for m in motor_dict.keys():
    print "HSETNX %s.%s.name VALUE '%s'" % (head, m, m)
    print "HSETNX %s.%s.name DBRTYPE 0" % (head, m)
    print "HSETNX %s.%s.position VALUE ''" % (head, m)
    print "HSETNX %s.%s.position DBRTYPE 6" % (head, m)
    for k in motor_dict[m]:
        v = motor_dict[m][k]
        print "HSETNX %s.%s.%s VALUE '%s'" % (head, m, k, v)

    for field, default, dbrtype  in motor_field_lists:
        print "HSETNX %s.%s.%s VALUE '%s'" % (head, m, field, default)
        print "HSETNX %s.%s.%s DBRTYPE '%s'" % (head, m, field, dbrtype)

    i = 0;
    if motor_presets.has_key( m):
        for pname, ppos, ptune in motor_presets[m]:
            print "HSETNX %s.%s.presets.%d.name VALUE %s"     % (head, m, i, pname)
            print "HSETNX %s.%s.presets.%d.position VALUE %s" % (head, m, i, ppos)
            if ptune != None:
                print "HSETNX %s.%s.presets.%d.canTune VALUE %s" % (head, m, i, ppos)
            i += 1
        print "HSETNX %s.%s.presets.length VALUE %d" % (head, m, i)
        
# light and zoom settings

for lev, f, b, p, x, y in zoom_settings:
    print "HSETNX %s.cam.zoom.%d.FrontLightIntensity VALUE %s" % (head, lev, f)
    print "HSETNX %s.cam.zoom.%d.LightIntensity VALUE %s"      % (head, lev, b)
    print "HSETNX %s.cam.zoom.%d.MotorPosition VALUE %s"       % (head, lev, p)
    print "HSETNX %s.cam.zoom.%d.ScaleX VALUE %s"              % (head, lev, x)
    print "HSETNX %s.cam.zoom.%d.ScaleY VALUE %s"              % (head, lev, y)