#
# Regular cron jobs for the acpi-mpd package
#
0 4	* * *	root	[ -x /usr/bin/acpi-mpd_maintenance ] && /usr/bin/acpi-mpd_maintenance
