type start.xml > task.xml
echo %1 >> task.xml
type end.xml >> task.xml
schtasks /create /tn "PCloud" /xml task.xml
del task.xml
pause
