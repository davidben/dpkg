#!/bin/sh
# Script to regenerate proof-of-concept cache
test -f /tmp/list-files.tar && rm /tmp/list-files.tar
cd /var/lib/dpkg/info/
tar -cf /tmp/list-files.tar *.list
