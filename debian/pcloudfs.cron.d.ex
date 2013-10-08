#
# Regular cron jobs for the pcloudfs package
#
0 4	* * *	root	[ -x /usr/bin/pcloudfs_maintenance ] && /usr/bin/pfs_maintenance
