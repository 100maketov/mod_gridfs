#
#  Makefile -- Build procedure for mod_gridfs Apache module
#
#  See LICENSE file for licensing details.
#

#   Set build environment
APXS=apxs2
CXX=g++

#	Set MongoDB C++ driver location
mongo_includedir=../mongo-cxx-driver/src
mongo_libdir=../mongo-cxx-driver

#	Determine paths using apxs
builddir=.
top_srcdir=$(shell $(APXS) -q installbuilddir)/..
top_builddir=$(shell $(APXS) -q installbuilddir)/..
_CXX:=$(CXX)
include $(top_builddir)/build/special.mk
CXX=$(_CXX)

#   Additional defines, includes and libraries
#DEFS=-D
INCLUDES=-I$(mongo_includedir) -I$(mongo_includedir)/mongo
SH_LIBS=-L$(mongo_libdir) -lstdc++ -lmongoclient -lboost_thread -lboost_filesystem -lboost_system

#   the default target
all: local-shared-build

#   install the shared object file into Apache 
install: install-modules-yes

#   cleanup
clean:
	-rm -f mod_gridfs.o mod_gridfs.lo mod_gridfs.slo mod_gridfs.la 
