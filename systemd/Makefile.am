## Process this file with automake to produce Makefile.in

MAINTAINERCLEANFILES = Makefile.in

unit_files =  \
    $(srcdir)/acpi-mpd.service

EXTRA_DIST = $(unit_files)

unit_dir = /usr/lib/systemd/user

install-data-hook: $(unit_files)
	mkdir -p $(DESTDIR)/$(unit_dir)
	cp $(unit_files) $(DESTDIR)/$(unit_dir)
