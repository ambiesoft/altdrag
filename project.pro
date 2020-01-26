TEMPLATE = subdirs  # This changes to the subdirs function.  You can't combine 
                    # compiling code and the subdirs function in the same .pro
                    # file.

# By default, you assign a directory to the SUBDIRS variable, and qmake looks
# inside that directory for a <dirname>.pro file.
SUBDIRS += AltDrag.pro

# You can append as many items as desired.  You can also specify the .pro file
# directly if need be.
SUBDIRS += hooks.pro

AltDrag.pro.depends = hooks.pro
