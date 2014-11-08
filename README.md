pfs
===

pCloud filesystem client
NOTE: this filesystem is obsolete. The new version can be found in the
pclsync repo at https://github.com/pcloudcom/pclsync .

To compile, you need fuse and the openssl headers. In debian,
they're in libssl-dev and libfuse-dev, in fedora in fuse-devel and
openssl-devel.

## Setup instructions on `systemd` based machines

### Install pfs

```sh
(sudo) yum install fuse-devel openssl-devel
git clone https://github.com/pcloudfs/pfs.git
cd pfs
make
(sudo) make install
```

### Get auth token

```sh 
curl https://api.pcloud.com/userinfo?getauth=1&username=<email>&password=<password>
```

And keep the auth bit.
Note that this is not required, but otherwise you'll have to put your username
and password in the service file.

### Autostart

Create a systemd service

```sh
gedit /usr/lib/systemd/system/pfs.service
```

And paste in:

```
[Unit]
Description=pCloud mount

[Service]
Type=oneshot
User=<your user>
Group=<your user>
RemainAfterExit=yes
ExecStart=/usr/bin/mount.pfs --auth <you auth token here> /run/media/<your user>/pCloud
ExecStop=/usr/bin/umount /run/media/<your user>/pCloud

[Install]
WantedBy=multi-user.target
```

Activate it via:

```sh
(sudo) systemctl enable pfs.service
```


## Compilation and manual mount on debian/ubuntu


### Install dependencies and compile pfs
```sh
(sudo) apt-get install fuse-dbg libssl-dbg
git clone https://github.com/pcloudfs/pfs.git
cd pfs
make
(sudo) make install
```

### Get auth token

```sh 
curl https://api.pcloud.com/userinfo?getauth=1&username=<email>&password=<password>
```

You will see output similar to the following:
````json
{
	"auth": "PhOAAAZ2YPXZEf999Rj8Ewz7abHR28hgNmxN9YGX",
	"emailverified": true,
	"quota": 11811160064,
	"result": 0,
	"premium": false,
	"usedquota": 1590902549,
	"language": "en",
	"userid": 12345,
	"email": "my@email.com",
	"registered": "Thu, 01 Nov 2014 10:10:10 +0000"
}
````

Take note of the `auth` token which you need to mount the drive.

### Mount pDrive

```sh
mkdir /mnt/pdrive
mount.pfs --auth <you auth token here> /mnt/pdrive
```
