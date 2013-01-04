#! /usr/bin/python


class iniParse:
    
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
                    current_section = sl[1:sl.find("]")]

                else:
                    if sl.find(";") > 0:
                        s = sl[0:sl.find(";")]
                    else:
                        s = sl

                    if s.find("=") > 0:
                        slist = s.split("=")
                        if len(slist) == 2:
                            k = slist[0].strip()
                            v = slist[1].strip()
                            current_dict[k] = v
            
        self.sd[current_section] = current_dict

    
        self.f.close()


    def sections( self):
        ks = set(self.sd.viewkeys())
        return list(ks.difference( ["default"]))

    def options( self, section):
        return self.sd[section].keys()

if __name__ == "__main__":
    ip = iniParse( "microdiff_hard.ini")
    ip.read()
    for s in ip.sections():
        print s
        print ip.options(s)



