mod_gridfs.la: mod_gridfs.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_gridfs.lo
DISTCLEAN_TARGETS = modules.mk
shared =  mod_gridfs.la
